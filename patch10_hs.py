F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_arm2dsp.c"
s = open(F).read()

anchor_st = "static int      a2d_noredir;      /* if set, arm only; do not redirect PC        */\n"
add_st = ("static int      a2d_hs_set;       /* hold data[0x098a]/[0x098c] each step        */\n"
          "static uint16_t a2d_hs;           /* value held into the go-live handshake cells */\n")
assert s.count(anchor_st) == 1
if "a2d_hs_set" not in s:
    s = s.replace(anchor_st, anchor_st + add_st, 1)

anchor_rs = '    a2d_noredir = getenv("CALYPSO_ARM2DSP_NOREDIR") ? 1 : 0;\n'
add_rs = ('    const char *hs = getenv("CALYPSO_ARM2DSP_HS");\n'
          "    a2d_hs_set = (hs && *hs) ? 1 : 0;\n"
          "    a2d_hs = a2d_hs_set ? (uint16_t)strtoul(hs, NULL, 0) : 0;\n")
assert s.count(anchor_rs) == 1
if 'getenv("CALYPSO_ARM2DSP_HS")' not in s:
    s = s.replace(anchor_rs, anchor_rs + add_rs, 1)

# unique to on_dsp_step: the block is followed by the "Sustained interrupt window" comment
anchor_step = (
    "    a2d_resolve();\n"
    "    if (!a2d_on) {\n"
    "        return;\n"
    "    }\n"
    "    /* Sustained interrupt window:"
)
add_step_body = (
    "    a2d_resolve();\n"
    "    if (!a2d_on) {\n"
    "        return;\n"
    "    }\n"
    "    if (a2d_hs_set) {\n"
    "        s->data[0x098a] = a2d_hs;   /* real ARM->DSP go-live handshake asserted */\n"
    "        s->data[0x098c] = a2d_hs;\n"
    "    }\n"
    "    /* Sustained interrupt window:"
)
assert s.count(anchor_step) == 1, "step anchor x%d" % s.count(anchor_step)
if "real ARM->DSP go-live handshake asserted" not in s:
    s = s.replace(anchor_step, add_step_body, 1)

open(F, "w").write(s)
print("PATCHED handshake-hold (CALYPSO_ARM2DSP_HS)")
