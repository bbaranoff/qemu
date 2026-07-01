F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_c54x.c"
s = open(F).read()

anchor = (
    "static int c54x_exec_one(C54xState *s)\n"
    "{\n"
    "    uint16_t op = prog_fetch(s, s->pc);\n"
)
assert s.count(anchor) == 1, "anchor x%d" % s.count(anchor)

helper = (
    "/* Faithful per-instruction interrupt LEVEL check (gated CALYPSO_C54X_IRQ_LEVEL).\n"
    " * The base model services interrupts only at the c54x_interrupt_ex call edge: an\n"
    " * IFR bit latched while INTM=1 is never taken later. Real C54x re-checks pending\n"
    " * unmasked interrupts at each instruction boundary. This restores that, so an\n"
    " * armed frame IT (INT3/bit3) fires once INTM drops -> native frame ISR runs. */\n"
    "static bool c54x_irq_level_check(C54xState *s)\n"
    "{\n"
    "    static int en = -1;\n"
    "    if (en < 0) en = getenv(\"CALYPSO_C54X_IRQ_LEVEL\") ? 1 : 0;\n"
    "    if (!en) return false;\n"
    "    if ((s->st1 & ST1_INTM) || s->delay_slots != 0) return false;\n"
    "    uint16_t pend = (uint16_t)(s->ifr & s->imr);\n"
    "    if (!pend) return false;\n"
    "    int b = __builtin_ctz(pend);          /* lowest set bit = highest priority */\n"
    "    int vec = b + 16;                     /* C54x: maskable IMR bit b -> vector b+16 */\n"
    "    s->ifr &= ~(1u << b);\n"
    "    s->sp--; data_write(s, s->sp, (uint16_t)s->pc);\n"
    "    s->sp--; data_write(s, s->sp, s->xpc);\n"
    "    s->st1 |= ST1_INTM;\n"
    "    s->xpc = 0;\n"
    "    uint16_t iptr = (s->pmst >> PMST_IPTR_SHIFT) & 0x1FF;\n"
    "    s->pc = (uint16_t)((iptr * 0x80) + vec * 4);\n"
    "    static unsigned lvln = 0;\n"
    "    if (lvln++ < 60)\n"
    "        fprintf(stderr, \"[c54x] IRQ-LEVEL take bit=%d vec=%d -> PC=0x%04x \"\n"
    "                \"IPTR=0x%03x IMR=0x%04x IFR=0x%04x insn=%u\\n\",\n"
    "                b, vec, s->pc, iptr, s->imr, s->ifr, s->insn_count);\n"
    "    return true;\n"
    "}\n\n"
)

new = (
    helper +
    "static int c54x_exec_one(C54xState *s)\n"
    "{\n"
    "    if (c54x_irq_level_check(s)) {\n"
    "        return 1;   /* per-instruction IRQ vectoring consumed this step */\n"
    "    }\n"
    "    uint16_t op = prog_fetch(s, s->pc);\n"
)
s = s.replace(anchor, new, 1)
open(F, "w").write(s)
print("PATCHED irq-level")
