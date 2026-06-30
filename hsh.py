#!/usr/bin/env python3
# harvard-sh -- shell sur le DSP TMS320C54x du TI Calypso (emulation QEMU).
# Architecture Harvard : bus PROGRAMME (prog[]) et bus DONNEES (data[]) separes.
# Usage :  python3 hsh.py [cmd ...]        (one-shot)
#          python3 hsh.py                  (REPL :  c54x> )
import sys
B = "/opt/GSM/"
def _rd(p):
    try: return open(p, "rb").read()
    except OSError: return b""
# ---------- espace PROGRAMME ----------
PROM0 = _rd(B+"calypso_dsp.PROM0.bin")   # prog[0x7000-0xDFFF]
PROM1 = _rd(B+"calypso_dsp.PROM1.bin")   # prog[0x18000-0x1FFFF], mirror 0x8000-0xFFFF
def progw(a):
    a &= 0xFFFF
    if 0x7000 <= a <= 0xDFFF:
        o=(a-0x7000)*2; return PROM0[o]|(PROM0[o+1]<<8)
    if 0x8000 <= a <= 0xFFFF:
        o=(a-0x8000)*2; return PROM1[o]|(PROM1[o+1]<<8)
    return None
# ---------- espace DONNEES ----------
DATA = _rd(B+"data.bin")                  # snapshot 64K data space
REGB = _rd(B+"calypso_dsp.Registers.bin") # MMR 0x00-0x5F
def dataw(a):
    a &= 0xFFFF
    return DATA[2*a]|(DATA[2*a+1]<<8) if 2*a+1 < len(DATA) else None
def regw(i): return REGB[2*i]|(REGB[2*i+1]<<8) if 2*i+1 < len(REGB) else 0
MMR = {0:"IMR",1:"IFR",6:"ST0",7:"ST1",8:"AL",9:"AH",10:"AG",11:"BL",12:"BH",
13:"BG",14:"T",15:"TRN",16:"AR0",17:"AR1",18:"AR2",19:"AR3",20:"AR4",21:"AR5",
22:"AR6",23:"AR7",24:"SP",25:"BK",26:"BRC",27:"RSA",28:"REA",29:"PMST",30:"XPC"}
# ---------- mini-desassembleur (espace prog) ----------
def dis1(a):
    op = progw(a); nx = progw(a+1)
    if op is None: return (1, "<unmapped>")
    if (op>>8)==0x77: return (2,"STM   #0x%04x,%s"%(nx,MMR.get(op&0x7f,"MMR%02x"%(op&0x7f))))
    if op==0x69f8:   return (3,"ORM   #0x%04x,*(0x%04x)"%(progw(a+2),nx))
    if (op&0xff00)==0x6900 and not(op&0x80): return (2,"ORM   #0x%04x,dma0x%02x"%(nx,op&0x7f))
    if (op&0xff00)==0x6800: return (2,"ANDM  #0x%04x,dma0x%02x"%(nx,op&0x7f))
    if (op&0xff00)==0x6b00: return (2,"ADDM  #0x%04x,dma0x%02x"%(nx,op&0x7f))
    if op==0x76f8: return (3,"ST    #0x%04x,*(0x%04x)"%(progw(a+2),nx))
    if op==0x10f8: return (2,"LD    *(0x%04x),A"%nx)
    if op==0x70f8: return (3,"MVKD  *(0x%04x)->*(0x%04x)"%(nx,progw(a+2)))
    if op==0x74f8: return (3,"PORTR PA=0x%04x,*(0x%04x)"%(progw(a+2),nx))
    if op==0x75f8: return (3,"PORTW *(0x%04x),PA=0x%04x"%(nx,progw(a+2)))
    if op==0xf073: return (2,"B     0x%04x"%nx)
    if op==0xf074: return (2,"CALL  0x%04x"%nx)
    if (op&0xff80)==0xf880: return (2,"BC?   0x%04x"%nx)
    if (op&0xff00)==0xf800: return (2,"BC    0x%04x"%nx)
    if op==0xfc00: return (1,"RET")
    if op==0xf7bb: return (1,"SSBX  INTM")
    if op==0xf6bb: return (1,"RSBX  INTM")
    if op==0xf5e1: return (1,"IDLE")
    if op==0xf4e2: return (1,"BACC  A")
    if op==0xf495: return (1,"NOP")
    return (1, "0x%04x"%op)
# ---------- commandes ----------
def cmd(args):
    if not args: args=["help"]
    c, rest = args[0], args[1:]
    H = lambda x:int(x,16)
    if c in ("help","i","?"):
        print("harvard-sh :: TMS320C54x @ Calypso (Harvard, 2 bus)")
        print("  PROGRAMME : p <a>[n] read | u <a>[n] disasm | m <hex> grep tic54x")
        print("  DONNEES   : d <a>[n] read | api <a>[n] | r regs | vec | uname | id")
    elif c=="uname":
        pmst=regw(29); print("C54x-DSP calypso-lead ROM-3606 #1 Harvard tms320c54x 16le")
        print("  prog: PROM0 %dB PROM1 %dB | pmst=0x%04x IPTR=0x%03x vec@0x%04x OVLY=%d"%(
            len(PROM0),len(PROM1),pmst,(pmst>>7)&0x1ff,((pmst>>7)&0x1ff)*0x80,(pmst>>5)&1))
    elif c=="id":
        imr,ifr,st1,sp=regw(0),regw(1),regw(7),regw(24)
        print("uid=0(l1) gid=0(dsp) groups=1(fb),2(sb),3(afc),12(sched-vec28)")
        print("  SP=0x%04x ST1=0x%04x INTM=%d IMR=0x%04x[%s] IFR=0x%04x"%(sp,st1,(st1>>11)&1,
            imr,",".join("b%d"%b for b in range(16) if imr&(1<<b)),ifr))
    elif c=="p":
        a=H(rest[0]); n=int(rest[1]) if len(rest)>1 else 8
        for i in range(0,n,8):
            print("P:0x%04x  "%(a+i)+" ".join("%04x"%(progw(a+i+j) or 0) for j in range(min(8,n-i))))
    elif c in ("d","api"):
        a=H(rest[0]); n=int(rest[1]) if len(rest)>1 else 8
        for i in range(0,n,8):
            t=" [api_ram]" if 0x0800<=a<=0x27ff else (" [MMR]" if a<0x60 else "")
            print("D:0x%04x  "%(a+i)+" ".join("%04x"%(dataw(a+i+j) or 0) for j in range(min(8,n-i)))+t)
    elif c in ("u","dis"):
        a=H(rest[0]); n=int(rest[1]) if len(rest)>1 else 12
        for _ in range(n):
            ln,m=dis1(a); print("  %04x:  %-15s %s"%(a," ".join("%04x"%(progw(a+k) or 0) for k in range(ln)),m)); a+=ln
    elif c in ("r","reg"):
        for i in (0,1,6,7,14,24,29,30): print("  %-5s = 0x%04x"%(MMR[i],regw(i)))
        print("  ARx  = "+" ".join("%04x"%regw(16+k) for k in range(8)))
    elif c=="vec":
        for v in range(16,32):
            b=0x80+v*4; w0=dataw(b)
            k="STUB-RETE" if w0==0xf4eb else ("->0x%04x"%dataw(b+3) if w0==0x731e else "0x%04x"%w0)
            print("  vec%-2d bit%-2d @%02x: %s"%(v,v-16,b,k))
    elif c=="m":
        import subprocess
        for f in ("/tmp/prom0.txt","/tmp/prom1.txt"):
            out=subprocess.run(["grep","-n",rest[0],f],capture_output=True,text=True).stdout
            if out: print("# %s"%f); print(out[:1500])
    elif c in ("q","quit","exit"): raise SystemExit
    else: print("?? %s  (help)"%c)
# ---------- entree ----------
if len(sys.argv)>1:
    cmd(sys.argv[1:])
else:
    print("harvard-sh :: TMS320C54x  (help, ctrl-D pour sortir)")
    try:
        while True:
            try: line=input("c54x> ").split()
            except EOFError: break
            if line:
                try: cmd(line)
                except SystemExit: break
                except Exception as e: print("err:",e)
    except KeyboardInterrupt: pass
