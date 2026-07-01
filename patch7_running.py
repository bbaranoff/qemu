F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_bsp.c"
s = open(F).read()

old = ('!getenv("CALYPSO_BSP_INT3_OFF") && bsp.dsp && bsp.dsp->running && '
       '(getenv("CALYPSO_BSP_INT3_UNGATED") || !(bsp.dsp->ifr & (1 << 3)))')
new = ('!getenv("CALYPSO_BSP_INT3_OFF") && bsp.dsp && '
       '(getenv("CALYPSO_BSP_INT3_UNGATED") || '
       '(bsp.dsp->running && !(bsp.dsp->ifr & (1 << 3))))')

n = s.count(old)
assert n == 2, "expected 2, got %d" % n
s = s.replace(old, new)
open(F, "w").write(s)
print("PATCHED running-bypass (%d sites)" % n)
