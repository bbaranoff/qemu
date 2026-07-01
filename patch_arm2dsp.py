import sys
BASE = "/opt/GSM/qemu-src/hw/arm/calypso/"

def patch(f, anchor, ins, before=True, n=1):
    p = BASE + f
    s = open(p).read()
    if s.count(anchor) < 1:
        print("!! anchor missing in %s: %r" % (f, anchor))
        sys.exit(1)
    if ins.strip() in s:
        print("== already patched (%s), skipping one" % f)
        return
    rep = (ins + anchor) if before else (anchor + ins)
    open(p, "w").write(s.replace(anchor, rep, n))
    print("patched", f)

# 1) meson.build : add the source file to the build
patch("meson.build", "    'calypso_c54x.c',\n",
      "    'calypso_arm2dsp.c',\n", before=False)

# 2) calypso_c54x.c : include + per-step hook (exec_pc, s in scope near FORCE-098)
patch("calypso_c54x.c", '#include "calypso_c54x.h"\n',
      '#include "calypso_arm2dsp.h"\n', before=False)
patch("calypso_c54x.c", "        /* FORCE-098 (etape A faithful",
      "        calypso_arm2dsp_on_dsp_step(s, exec_pc);\n\n", before=True)

# 3) calypso_trx.c : include + ARM-write hook (offset, value in scope near FORCE-HS)
patch("calypso_trx.c", '#include "qemu/osdep.h"\n',
      '#include "calypso_arm2dsp.h"\n', before=False)
patch("calypso_trx.c", "    /* FORCE-HS (etape A, gated",
      "    calypso_arm2dsp_on_arm_write((uint16_t)offset, (uint16_t)(value & 0xFFFF));\n\n",
      before=True)

print("ALL PATCHED")
