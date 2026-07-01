F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_arm2dsp.c"
s = open(F).read()

old = (
    "    a2d_resolve();\n"
    "    if (!a2d_on || !a2d_pending) {\n"
    "        return;\n"
    "    }\n"
    "    if (exec_pc != a2d_at) {\n"
    "        return;\n"
    "    }\n"
)
new = (
    "    a2d_resolve();\n"
    "    if (!a2d_on) {\n"
    "        return;\n"
    "    }\n"
    "    /* Sustained interrupt window: once armed, keep INTM cleared each pass\n"
    "     * through the wait-loop PC so the armed frame IT (INT3) can actually be\n"
    "     * taken. Done only at a2d_at (in the bring-up loop, never inside an ISR)\n"
    "     * to avoid nesting; the ISR sets INTM=1 on entry and RETE restores it. */\n"
    "    if (a2d_enai && a2d_done > 0 && exec_pc == a2d_at) {\n"
    "        s->st1 &= ~ST1_INTM;\n"
    "    }\n"
    "    if (!a2d_pending || exec_pc != a2d_at) {\n"
    "        return;\n"
    "    }\n"
)
assert s.count(old) == 1, "anchor x%d" % s.count(old)
s = s.replace(old, new, 1)
open(F, "w").write(s)
print("PATCHED intm-window")
