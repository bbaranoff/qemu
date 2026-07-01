F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_c54x.c"
s = open(F).read()

anchor = "        if (exec_pc == 0xde97 || exec_pc == 0xde9c || exec_pc == 0xdddb || exec_pc == 0xde8b) {"
assert s.count(anchor) == 1, "anchor x%d" % s.count(anchor)

probe = (
    "        /* SM-TRACE (gated CALYPSO_SM_TRACE) : trace instruction-par-instruction\n"
    "         * l'etat-machine handshake 0xdde0-0xde9f (route reclear 0xde8b vs setter\n"
    "         * 0xde9c). Montre PC/op/A/TC + les 5 cellules 0x098a..0x098e a chaque\n"
    "         * pas, pour voir OU le flot devie du chemin de9c et quelles valeurs le\n"
    "         * routeraient. Cap 400. */\n"
    "        if (exec_pc >= 0xdde0 && exec_pc <= 0xde9f) {\n"
    "            static int smt = -1; static unsigned smn = 0;\n"
    '            if (smt < 0) smt = getenv("CALYPSO_SM_TRACE") ? 1 : 0;\n'
    "            if (smt && smn < 400) {\n"
    "                smn++;\n"
    '                fprintf(stderr, "[c54x] SM-TRACE PC=0x%04x op=0x%04x A=0x%04x TC=%d '
    '098[a=%04x b=%04x c=%04x d=%04x e=%04x] insn=%u\\n",\n'
    "                        exec_pc, exec_op, (unsigned)(s->a & 0xFFFF),\n"
    "                        (s->st0 & ST0_TC) ? 1 : 0,\n"
    "                        s->data[0x098a], s->data[0x098b], s->data[0x098c],\n"
    "                        s->data[0x098d], s->data[0x098e], s->insn_count);\n"
    "            }\n"
    "        }\n"
)
s = s.replace(anchor, probe + anchor, 1)
open(F, "w").write(s)
print("PATCHED SM-TRACE probe")
