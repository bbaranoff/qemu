F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_bsp.c"
s = open(F).read()

old = "&& !(bsp.dsp->ifr & (1 << 3))) {"
new = ('&& (getenv("CALYPSO_BSP_INT3_UNGATED") || !(bsp.dsp->ifr & (1 << 3)))) {')

n = s.count(old)
assert n == 2, "expected 2 sites, got %d" % n
if new.split("||")[0] in s and "CALYPSO_BSP_INT3_UNGATED" in s:
    print("already patched")
else:
    s = s.replace(old, new)
    open(F, "w").write(s)
    print("PATCHED bsp ungate (%d sites)" % n)
