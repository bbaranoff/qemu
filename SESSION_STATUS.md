# Calypso QEMU — Session Status 2026-04-12

## TL;DR
Full L1CTL path working: mobile → bridge → QEMU l1ctl_sock → sercomm → firmware ARM → TDMA scheduler → DSP.
Firmware does PM scan, FB detect (d_task_md=5) and SB detect (d_task_md=6).
**Current blocker**: DSP FB correlator doesn't detect frequency burst in DL data (d_fb_det stays 0).

## Architecture

```
osmo-bts-trx (local 5800/5801/5802)
       │
       ▼  TRXD/TRXC UDP via calypso_orch
       │
QEMU Calypso (calypso_trx.c + calypso_orch.c)
  ├── ARM7TDMI: OsmocomBB layer1.highram.elf firmware
  ├── C54x DSP: calypso_dsp.txt ROM + calypso_c54x.c emulator
  ├── BSP: DMA burst delivery to DSP DARAM
  ├── INTH: level-sensitive + round-robin interrupt controller
  ├── UART modem: sercomm relay with baud-rate staging buffer
  └── l1ctl_sock: L1CTL unix socket server (poll-based)
       │
       ▼  L1CTL length-prefixed messages
       │
l1ctl_pace.py (optional pacing bridge)
       │
       ▼  L1CTL
       │
mobile (OsmocomBB layer23)
```

## Bugs fixed (2026-04-12)

### Critical: staging buffer ring bug
- `calypso_uart_inject_raw` wrote bytes at wrong offset after first batch drained
- `stg_head` never advanced → 2nd batch written at offset 0, read from offset 40 → all zeros
- Same bug in drip→FIFO: `rx_head` never advanced
- **Fix**: advance `stg_head` and `rx_head` on each push (standard ring buffer pattern)
- **Impact**: this was THE root cause blocking FBSB since April 11

### INTH level-sensitive IRQ fix
- `IRQ_CTRL` ACK cleared level bits for ALL IRQs including level-sensitive ones (UART, API)
- QEMU's `qemu_irq_raise` is edge-triggered — re-raise after clear is a no-op
- **Fix**: only clear edge-triggered sources (IRQ4=TPU_FRAME, IRQ5=TINT0) on ACK

### INTH round-robin
- Priority scan started at IRQ 0 every time → IRQ4 always beat IRQ7 at same priority
- **Fix**: `rr_start` field advances to `last_serviced + 1` after each ACK

### DSP force-IDLE safety net
- DSP wakes on masked SINT17 (per SPRU131) but runs off into garbage code
- **Fix**: if DSP runs full budget with d_task_md=0, force idle=true

### DSP budget reduction
- When IMR has SINT17 bit cleared, reduce budget from 500K to 5K instructions

### TPU lifecycle fix
- `l1s_reset()` in FBSB handler clears TPU_CTRL_EN → DSP stopped running
- **Fix**: gate DSP run on `dsp_init_done` (not just `tpu_en_now`)

### l1ctl_sock overhaul
- Replaced `qemu_set_fd_handler` with manual `l1ctl_sock_poll()` (fd_handlers don't fire from QEMU_CLOCK_VIRTUAL timer callbacks)
- Poll called from TINT0 tick + DSP run loop + UART rx_poll timer
- Fixed `recv()` EAGAIN closing client socket
- Removed fd_handler race that caused EPIPE

### msgb pool extension (runtime patch)
- Firmware has static pool of 32 msgb (MSGB_NUM in comm/msgb.c)
- Exhaustion → `while(1)` panic (not graceful)
- **Fix**: patch `cmp r3, #32` → `cmp r3, #64` at runtime + zero-init extra entries
- Patch address for .bak firmware: 0x0082c33c

### Firmware console stubs
- `cons_puts` (0x0082a1b0) and `puts` (0x00829ea0) busy-wait on UART TX
- **Fix**: replace first instruction with `BX LR` at runtime

## Files modified

| File | Changes |
|------|---------|
| `hw/intc/calypso_inth.c` | Level-clear fix, round-robin |
| `include/hw/arm/calypso/calypso_inth.h` | `rr_start` field |
| `hw/arm/calypso/calypso_trx.c` | DSP force-IDLE, budget, TPU lifecycle, poll calls, msgb patch, addr-sub updates |
| `hw/arm/calypso/l1ctl_sock.c` | Poll-based accept/read, EAGAIN fix, removed fd_handler |
| `hw/char/calypso_uart.c` | Staging buffer, baud-rate drip, ring buffer fix, poll from rx_timer |
| `include/hw/arm/calypso/calypso_uart.h` | Staging buffer fields, `drip_rx` + `l1ctl_sock_poll` decls |
| `l1ctl_pace.py` | Pacing bridge (optional, for future use) |

## Firmware

- Binary: `layer1.highram.elf` (.bak from Docker image `bastienbaranoff/free-bb:getting_closer`)
- Fresh osmocom-bb source at `/opt/GSM/osmocom-bb` (git clone, Makefile.inc patched for `-Wl,-Map,`)
- Toolchain: `/root/gnuarm/install/bin/arm-elf-gcc` 4.5.2, `ln -sf libmpfr.so.6 libmpfr.so.4`
- Note: recompiled firmware has different addresses — use nm to update calypso_trx.c

## L1CTL message flow (verified working)

```
1. RESET_IND  ← firmware (boot)     → mobile
2. RESET_REQ  ← mobile              → firmware → RESET_CONF → mobile  ✓
3. RESET_REQ  ← mobile (redundant)  → firmware (ignored)              ✓
4. PM_REQ     ← mobile              → firmware → d_task_md=1 → PM_CONF ✓
5. RESET_REQ  ← mobile              → firmware → RESET_CONF           ✓
6. FBSB_REQ   ← mobile              → firmware → l1s_reset + l1s_fbsb_req ✓
7. d_task_md=5 (FB detect) fires                                       ✓
8. d_task_md=6 (SB detect) fires                                       ✓
9. d_fb_det=? (DSP correlator)                                         ← PENDING
```

## Next: DSP FB correlator

The DSP receives d_task_md=5 and runs the FB detection routine at PROM0 0x7730-0x7990.
It must find the frequency burst in the DL data (received via BSP DMA into DARAM 0x3fc0)
and write d_fb_det=1. Currently d_fb_det stays at 0.

Possible issues:
- DL burst data doesn't contain a real frequency burst on the right timeslot
- DSP correlator has a bug (unimplemented opcode, wrong NORM, etc.)
- BSP DMA delivers data at wrong DARAM offset
- DSP timing: FB detect must run on the correct TDMA frame
