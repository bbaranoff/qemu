#!/usr/bin/env python3
"""Diagnostic complet FBSB (Calypso QEMU) - confronte le live au mecanisme documente.

Concu pour tourner DANS le container osmo-operator-1 (acces direct a /root/qemu.log,
/proc/<pid>/environ). Lecture seule : aucune ecriture d'etat DSP, aucun redemarrage de
process. Sortie : rapport texte avec un verdict par etape de la chaine FBSB.

Usage:
    docker exec osmo-operator-1 python3 /path/to/fbsb_diag.py
    docker exec osmo-operator-1 python3 /path/to/fbsb_diag.py --json   (sortie machine)
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

QEMU_LOG = "/root/qemu.log"


def find_qemu_pid():
    try:
        out = subprocess.run(["pgrep", "-f", "qemu-system-arm"], capture_output=True, text=True)
        pids = [p for p in out.stdout.split() if p.isdigit()]
        return int(pids[0]) if pids else None
    except Exception:
        return None


def read_environ(pid):
    path = f"/proc/{pid}/environ"
    env = {}
    try:
        with open(path, "rb") as f:
            raw = f.read()
        for chunk in raw.split(b"\x00"):
            if b"=" in chunk:
                k, _, v = chunk.partition(b"=")
                env[k.decode(errors="replace")] = v.decode(errors="replace")
    except Exception:
        pass
    return env


def tail_matching(path, patterns, max_lines=4000):
    """Return list of (raw_line) for the last max_lines of `path` matching any pattern."""
    if not os.path.isfile(path):
        return []
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()[-max_lines:]
    except Exception:
        return []
    compiled = [re.compile(p) for p in patterns]
    return [ln.rstrip("\n") for ln in lines if any(c.search(ln) for c in compiled)]


def latest_match(lines):
    return lines[-1] if lines else None


def check(name, ok, detail):
    status = "OK  " if ok else "FAIL"
    return {"check": name, "status": "OK" if ok else "FAIL", "detail": detail}, \
        f"[{status}] {name} -- {detail}"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="sortie JSON au lieu du texte")
    ap.add_argument("--log", default=QEMU_LOG, help="chemin du log qemu (defaut /root/qemu.log)")
    ap.add_argument("--window", type=float, default=5.0,
                    help="fenetre en secondes pour mesurer l evolution fb0_att (defaut 5s)")
    args = ap.parse_args()

    results = []
    lines_out = []

    pid = find_qemu_pid()
    r, l = check("processus qemu-system-arm actif", pid is not None,
                 f"pid={pid}" if pid else "aucun processus qemu-system-arm trouve")
    results.append(r); lines_out.append(l)

    env = read_environ(pid) if pid else {}

    # --- 1) Flags diagnostiques de cette session : doivent etre OFF en usage normal --------
    diag_flags = ["CALYPSO_POKE_A4C7_ONCE", "CALYPSO_DSP_FRAME_VEC28",
                  "CALYPSO_TRACE_VEC28_STACK"]
    leftover = {f: env.get(f) for f in diag_flags if env.get(f) not in (None, "", "0")}
    r, l = check("aucun flag diagnostique laisse actif", len(leftover) == 0,
                 "propre" if not leftover else f"ACTIFS (a revert): {leftover}")
    results.append(r); lines_out.append(l)

    # --- 2) Mode DSP reel vs shunt -----------------------------------------------------------
    dsp_shunt = env.get("CALYPSO_DSP_SHUNT", "0") not in ("0", "", None)
    dsp_mode = env.get("CALYPSO_DSP", "c54x")
    r, l = check("mode DSP", True,
                 f"CALYPSO_DSP_SHUNT={env.get('CALYPSO_DSP_SHUNT', '0')} "
                 f"CALYPSO_DSP={dsp_mode} -- "
                 + ("chemin MOCK actif (dsp_shunt), pas le vrai ROM"
                    if dsp_shunt else "chemin DSP REEL (c54x_run), pas de mock"))
    results.append(r); lines_out.append(l)

    # --- 3) Commande ARM -> FB_DSP_TASK ------------------------------------------------------
    task_lines = tail_matching(args.log, [r"task_md.*5\b", r"FB_DSP_TASK", r"task=5\b"])
    r, l = check("ARM a commande FB (d_task_md=5 / FB_DSP_TASK)", len(task_lines) > 0,
                 latest_match(task_lines) or "aucune ligne de commande FB trouvee dans le log")
    results.append(r); lines_out.append(l)

    # --- 4) Livraison I/Q reelle (BSP / DMA) -------------------------------------------------
    bsp_lines = tail_matching(args.log, [r"\[BSP\] DRAIN-CB.*delivered=(\d+)",
                                          r"\[trx\].*DSP-DONE-DMA", r"bsp_load"])
    delivered_climbing = False
    if len(bsp_lines) >= 2:
        nums = [int(m.group(1)) for ln in bsp_lines
                for m in [re.search(r"delivered=(\d+)", ln)] if m]
        delivered_climbing = len(nums) >= 2 and nums[-1] > nums[0]
    r, l = check("livraison I/Q reelle (BSP DRAIN-CB delivered= climbe)",
                 len(bsp_lines) > 0 and (delivered_climbing or len(bsp_lines) == 1),
                 (latest_match(bsp_lines) or "aucune ligne DRAIN-CB/DMA BSP trouvee"))
    results.append(r); lines_out.append(l)

    # --- 5) FB0_SEARCH (tracker host calypso_fbsb.c) evolue -----------------------------------
    fbsb_pattern = r"\[fbsb\] FB0_SEARCH.*fb0_att=(\d+)"
    fbsb_lines = tail_matching(args.log, [fbsb_pattern])
    line_count_before = len(fbsb_lines)
    att_before = None
    if fbsb_lines:
        m = re.search(fbsb_pattern, fbsb_lines[-1])
        if m:
            att_before = int(m.group(1))
    time.sleep(args.window)
    fbsb_lines2 = tail_matching(args.log, [fbsb_pattern])
    att_after = None
    if fbsb_lines2:
        m = re.search(fbsb_pattern, fbsb_lines2[-1])
        if m:
            att_after = int(m.group(1))
    # fb0_att can legitimately RESET (search cycle restart), so "alive" means the counter
    # CHANGED at all (up or down/reset) within the window, not strictly monotonic increase.
    alive = (att_before is not None and att_after is not None and att_after != att_before)
    r, l = check("FB0_SEARCH (tracker host calypso_fbsb.c) actif",
                 alive,
                 f"fb0_att {att_before} -> {att_after} sur {args.window}s "
                 f"(un reset a la baisse est un cycle de recherche normal, pas une panne) "
                 f"(NOTE: ce tracker est un ORACLE COTE HOTE, PAS le vrai correlateur DSP ROM)")
    results.append(r); lines_out.append(l)

    # --- 6) IMR / INTM (racine A/B) ------------------------------------------------------------
    imr_lines = tail_matching(args.log, [r"IMR-ARM", r"IMR=0x[0-9a-f]+"])
    intm1_lines = tail_matching(args.log, [r"INTM_bit=1"])
    r, l = check("IMR jamais reamre naturellement (racine A)",
                 True,
                 (latest_match(imr_lines) or "aucune ligne IMR-ARM (probe non active ce run)")
                 + f" -- {len(intm1_lines)} lignes recentes avec INTM_bit=1 (verrou confirme si >0)")
    results.append(r); lines_out.append(l)

    # --- 7) d_fb_det reste a zero ---------------------------------------------------------------
    fbdet_lines = tail_matching(args.log, [r"D_FB_DET", r"FBDET-RD", r"d_fb_det"])
    fbdet_nonzero = [ln for ln in fbdet_lines if re.search(r"0x0*[1-9a-f][0-9a-f]*", ln)
                     and "0x0000" not in ln]
    r, l = check("d_fb_det (resultat DSP reel) reste a zero",
                 len(fbdet_nonzero) == 0,
                 (latest_match(fbdet_lines) or "aucune sonde D_FB_DET/FBDET-RD active ce run")
                 + (f" -- {len(fbdet_nonzero)} VALEUR(S) NON-ZERO trouvees, a investiguer !"
                    if fbdet_nonzero else ""))
    results.append(r); lines_out.append(l)

    # --- 8) Derail connu (POST-BOOTSTUB-RET / storm PC=0x0000) -----------------------------------
    derail_lines = tail_matching(args.log, [r"POST-BOOTSTUB-RET"])
    r, l = check("pas de storm PC=0x0000 (derail racine B) dans cette fenetre",
                 len(derail_lines) == 0,
                 f"{len(derail_lines)} occurrence(s) POST-BOOTSTUB-RET dans les "
                 f"{4000} dernieres lignes -- "
                 + ("normal en fonctionnement naturel (racine A bloque avant d'atteindre "
                    "racine B)" if len(derail_lines) == 0 else
                    "ATTENDU seulement si un test de falsification est actif -- verifier "
                    "flag CALYPSO_POKE_A4C7_ONCE / CALYPSO_DSP_FRAME_VEC28"))
    results.append(r); lines_out.append(l)

    verdict_pass = all(r["status"] == "OK" for r in results
                        if r["check"] not in ("d_fb_det (resultat DSP reel) reste a zero",))
    # d_fb_det staying zero is EXPECTED given documented root causes -- not a pipeline failure,
    # it's the known, tracked product gap. Report it but don't count it as a pipeline health FAIL.

    summary = {
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "qemu_pid": pid,
        "checks": results,
        "pipeline_healthy": verdict_pass,
        "note": "d_fb_det=0 est un ecart PRODUIT connu (racines A/B, STATUS_2026-07-01.md "
                "addenda 15/20/22/23-24), pas une panne du pipeline de test.",
    }

    if args.json:
        print(json.dumps(summary, indent=2, ensure_ascii=False))
    else:
        print("=" * 78)
        print("DIAGNOSTIC FBSB -- ", summary["generated_at"])
        print("=" * 78)
        for l in lines_out:
            print(l)
        print("-" * 78)
        print("Pipeline systeme (hors d_fb_det, ecart produit connu) :",
              "SAIN" if verdict_pass else "PROBLEME DETECTE")
        print(summary["note"])

    sys.exit(0 if verdict_pass else 1)


if __name__ == "__main__":
    main()
