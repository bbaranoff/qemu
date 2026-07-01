F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_arm2dsp.c"
s = open(F).read()

def ins_after(anchor, add):
    global s
    assert s.count(anchor) == 1, "anchor x%d: %r" % (s.count(anchor), anchor)
    if add.strip() in s:
        print("already present, skip")
        return
    s = s.replace(anchor, anchor + add, 1)

# 1) declare statics for the 0x435b selector
ins_after(
    "static unsigned a2d_max;          /* how many drives allowed                    */\n",
    "static int      a2d_435b_set;     /* whether to set data[0x435b] at drive       */\n"
    "static uint16_t a2d_435b;         /* value to write into data[0x435b]           */\n",
)

# 2) resolve from env CALYPSO_ARM2DSP_435B
ins_after(
    "    a2d_sp = a2d_sp_set ? (uint16_t)strtoul(sp, NULL, 0) : 0;\n",
    '    const char *r = getenv("CALYPSO_ARM2DSP_435B");\n'
    "    a2d_435b_set = (r && *r) ? 1 : 0;\n"
    "    a2d_435b = a2d_435b_set ? (uint16_t)strtoul(r, NULL, 0) : 0;\n",
)

# 3) apply the 0x435b selector at drive time (right after the SP wiring)
ins_after(
    "    if (a2d_sp_set) {\n        s->sp = a2d_sp;\n    }\n",
    "    if (a2d_435b_set) {\n        s->data[0x435b] = a2d_435b;\n    }\n",
)

open(F, "w").write(s)
print("ALL PATCHED 435b")
