F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_arm2dsp.c"
s = open(F).read()

def ins_after(anchor, add):
    global s
    assert s.count(anchor) == 1, "anchor x%d: %r" % (s.count(anchor), anchor)
    if add.strip().split("\n")[0].strip() in s:
        print("already present, skip:", add.strip()[:40]); return
    s = s.replace(anchor, anchor + add, 1)

def replace_once(old, new):
    global s
    assert s.count(old) == 1, "replace x%d: %r" % (s.count(old), old)
    s = s.replace(old, new, 1)

# 1) statics
ins_after(
    "static uint16_t a2d_435b;         /* value to write into data[0x435b]           */\n",
    "static int      a2d_imr_set;      /* whether to OR bits into IMR at drive        */\n"
    "static uint16_t a2d_imr;          /* IMR bits to arm (e.g. 0x08 = INT3 frame)    */\n"
    "static int      a2d_enai;         /* whether to clear INTM (enable interrupts)   */\n"
    "static int      a2d_noredir;      /* if set, arm only; do not redirect PC        */\n",
)

# 2) resolve from env
ins_after(
    "    a2d_435b = a2d_435b_set ? (uint16_t)strtoul(r, NULL, 0) : 0;\n",
    '    const char *im = getenv("CALYPSO_ARM2DSP_IMR");\n'
    "    a2d_imr_set = (im && *im) ? 1 : 0;\n"
    "    a2d_imr = a2d_imr_set ? (uint16_t)strtoul(im, NULL, 0) : 0;\n"
    '    a2d_enai = getenv("CALYPSO_ARM2DSP_ENAI") ? 1 : 0;\n'
    '    a2d_noredir = getenv("CALYPSO_ARM2DSP_NOREDIR") ? 1 : 0;\n',
)

# 3) drive: arm IMR + enable interrupts, redirect only if not arm-only
replace_once(
    "    if (a2d_sp_set) {\n"
    "        s->sp = a2d_sp;\n"
    "    }\n"
    "    if (a2d_435b_set) {\n"
    "        s->data[0x435b] = a2d_435b;\n"
    "    }\n"
    "    s->pc = a2d_tgt;\n",
    "    if (a2d_imr_set) {\n"
    "        s->imr |= a2d_imr;          /* arm frame IT (INT3/bit3) so BSP IT vectorizes */\n"
    "    }\n"
    "    if (a2d_enai) {\n"
    "        s->st1 &= ~ST1_INTM;        /* RSBX INTM: enable maskable interrupts */\n"
    "    }\n"
    "    if (!a2d_noredir) {\n"
    "        if (a2d_sp_set) {\n"
    "            s->sp = a2d_sp;\n"
    "        }\n"
    "        if (a2d_435b_set) {\n"
    "            s->data[0x435b] = a2d_435b;\n"
    "        }\n"
    "        s->pc = a2d_tgt;\n"
    "    }\n",
)

open(F, "w").write(s)
print("ALL PATCHED imr/enai/noredir")
