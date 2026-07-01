F = "/opt/GSM/qemu-src/hw/arm/calypso/calypso_arm2dsp.c"
s = open(F).read()

# 1) static array
st_anchor = "static uint16_t a2d_hs;           /* value held into the go-live handshake cells */\n"
st_add = "static uint16_t a2d_hsv[5];       /* per-cell 0x098a..0x098e (HS_A..HS_E)         */\n"
assert s.count(st_anchor) == 1, "st x%d" % s.count(st_anchor)
if "a2d_hsv[5]" not in s:
    s = s.replace(st_anchor, st_anchor + st_add, 1)

# 2) resolve : uniform + per-cell overrides
rs_old = ('    const char *hs = getenv("CALYPSO_ARM2DSP_HS");\n'
          "    a2d_hs_set = (hs && *hs) ? 1 : 0;\n"
          "    a2d_hs = a2d_hs_set ? (uint16_t)strtoul(hs, NULL, 0) : 0;\n")
rs_new = ('    const char *hs = getenv("CALYPSO_ARM2DSP_HS");\n'
          "    a2d_hs = (hs && *hs) ? (uint16_t)strtoul(hs, NULL, 0) : 0;\n"
          '    a2d_hsv[0] = a2d_env_u16("CALYPSO_ARM2DSP_HS_A", a2d_hs);\n'
          '    a2d_hsv[1] = a2d_env_u16("CALYPSO_ARM2DSP_HS_B", a2d_hs);\n'
          '    a2d_hsv[2] = a2d_env_u16("CALYPSO_ARM2DSP_HS_C", a2d_hs);\n'
          '    a2d_hsv[3] = a2d_env_u16("CALYPSO_ARM2DSP_HS_D", a2d_hs);\n'
          '    a2d_hsv[4] = a2d_env_u16("CALYPSO_ARM2DSP_HS_E", a2d_hs);\n'
          "    a2d_hs_set = ((hs && *hs) ||\n"
          '                  getenv("CALYPSO_ARM2DSP_HS_A") || getenv("CALYPSO_ARM2DSP_HS_B") ||\n'
          '                  getenv("CALYPSO_ARM2DSP_HS_C") || getenv("CALYPSO_ARM2DSP_HS_D") ||\n'
          '                  getenv("CALYPSO_ARM2DSP_HS_E")) ? 1 : 0;\n')
assert s.count(rs_old) == 1, "rs x%d" % s.count(rs_old)
s = s.replace(rs_old, rs_new, 1)

# 3) hold : per-cell writes
h_old = ("        s->data[0x098a] = a2d_hs;\n"
         "        s->data[0x098b] = a2d_hs;\n"
         "        s->data[0x098c] = a2d_hs;\n"
         "        s->data[0x098d] = a2d_hs;\n"
         "        s->data[0x098e] = a2d_hs;\n"
         "        /* the DSP reads the API region (0x0800+) from api_ram, NOT data[] */\n"
         "        if (s->api_ram) {\n"
         "            s->api_ram[0x098a - 0x0800] = a2d_hs;\n"
         "            s->api_ram[0x098b - 0x0800] = a2d_hs;\n"
         "            s->api_ram[0x098c - 0x0800] = a2d_hs;\n"
         "            s->api_ram[0x098d - 0x0800] = a2d_hs;\n"
         "            s->api_ram[0x098e - 0x0800] = a2d_hs;\n"
         "        }\n")
h_new = ("        s->data[0x098a] = a2d_hsv[0];\n"
         "        s->data[0x098b] = a2d_hsv[1];\n"
         "        s->data[0x098c] = a2d_hsv[2];\n"
         "        s->data[0x098d] = a2d_hsv[3];\n"
         "        s->data[0x098e] = a2d_hsv[4];\n"
         "        /* the DSP reads the API region (0x0800+) from api_ram, NOT data[] */\n"
         "        if (s->api_ram) {\n"
         "            s->api_ram[0x098a - 0x0800] = a2d_hsv[0];\n"
         "            s->api_ram[0x098b - 0x0800] = a2d_hsv[1];\n"
         "            s->api_ram[0x098c - 0x0800] = a2d_hsv[2];\n"
         "            s->api_ram[0x098d - 0x0800] = a2d_hsv[3];\n"
         "            s->api_ram[0x098e - 0x0800] = a2d_hsv[4];\n"
         "        }\n")
assert s.count(h_old) == 1, "h x%d" % s.count(h_old)
s = s.replace(h_old, h_new, 1)

open(F, "w").write(s)
print("PATCHED per-cell handshake (HS_A..HS_E)")
