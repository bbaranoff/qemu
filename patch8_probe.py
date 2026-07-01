F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_bsp.c"
s = open(F).read()

anchor = "        while ((sl = bsp_take_for_fn(tn, current_fn)) != NULL) {\n"
assert s.count(anchor) == 1, "anchor x%d" % s.count(anchor)
probe = (
    anchor +
    "        { static unsigned _dlp = 0;\n"
    "          if (_dlp++ < 20)\n"
    '              fprintf(stderr, "[BSP] DELIVER-LOOP tn=%d dsp=%p running=%d '
    'ifr=0x%04x off=%d ungated=%d\\n", tn, (void*)bsp.dsp,\n'
    "                      bsp.dsp ? bsp.dsp->running : -1,\n"
    "                      bsp.dsp ? bsp.dsp->ifr : 0,\n"
    '                      !!getenv("CALYPSO_BSP_INT3_OFF"),\n'
    '                      !!getenv("CALYPSO_BSP_INT3_UNGATED")); }\n'
)
s = s.replace(anchor, probe, 1)
open(F, "w").write(s)
print("PATCHED deliver-loop probe")
