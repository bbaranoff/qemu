/*
 * calypso_uart.c — Calypso UART
 *
 * Pragmatic emulation for the Compal/Calypso loader path:
 *  - strict 8-bit MMIO accesses
 *  - banked registers via LCR[7] / LCR==0xBF
 *  - SCR / SSR implemented
 *  - RX FIFO with verbose debug
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "chardev/char-fe.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/arm/calypso/calypso_uart.h"
#include "hw/arm/calypso/calypso_trx.h"

/* Register offsets */
#define REG_RBR_THR   0x00
#define REG_IER       0x01
#define REG_IIR_FCR   0x02
#define REG_LCR       0x03
#define REG_MCR       0x04
#define REG_LSR       0x05
#define REG_MSR       0x06
#define REG_SPR       0x07
#define REG_MDR1      0x08
#define REG_SCR       0x10
#define REG_SSR       0x11

/* IER bits */
#define IER_RX_DATA   (1 << 0)
#define IER_TX_EMPTY  (1 << 1)
#define IER_RX_LINE   (1 << 2)

/* IIR values */
#define IIR_NO_INT    0x01
#define IIR_RX_LINE   0x06
#define IIR_RX_DATA   0x04
#define IIR_TX_EMPTY  0x02

/* LCR bits */
#define LCR_DLAB      (1 << 7)
#define LCR_CONF_BF   0xBF

/* LSR bits */
#define LSR_DR        (1 << 0)
#define LSR_OE        (1 << 1)
#define LSR_THRE      (1 << 5)
#define LSR_TEMT      (1 << 6)

/* MSR bits */
#define MSR_CTS       (1 << 4)
#define MSR_DSR       (1 << 5)
#define MSR_DCD       (1 << 7)

/* FCR bits */
#define FCR_FIFO_EN   (1 << 0)
#define FCR_RX_RESET  (1 << 1)
#define FCR_TX_RESET  (1 << 2)

/* SSR bits */
#define SSR_TX_FIFO_FULL  (1 << 0)

static void uart_log_raw(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fwrite(buf, 1, len, f);
    fclose(f);
}

/* ---- FIFO helpers ---- */

static void fifo_reset(CalypsoUARTState *s)
{
    s->rx_head = 0;
    s->rx_tail = 0;
    s->rx_count = 0;
}

static void fifo_push(CalypsoUARTState *s, uint8_t data)
{
    if (s->rx_count >= CALYPSO_UART_RX_FIFO_SIZE) {
        s->lsr |= LSR_OE;
        return;
    }
    s->rx_fifo[s->rx_head] = data;
    s->rx_head = (s->rx_head + 1) % CALYPSO_UART_RX_FIFO_SIZE;
    s->rx_count++;
}

static uint8_t fifo_pop(CalypsoUARTState *s)
{
    if (s->rx_count == 0) return 0;
    uint8_t data = s->rx_fifo[s->rx_tail];
    s->rx_tail = (s->rx_tail + 1) % CALYPSO_UART_RX_FIFO_SIZE;
    s->rx_count--;
    return data;
}

/* ---- IRQ ---- */

static void calypso_uart_update_irq(CalypsoUARTState *s)
{
    uint8_t iir = IIR_NO_INT;
    bool want = false;

    if ((s->ier & IER_RX_LINE) && (s->lsr & LSR_OE)) {
        iir = IIR_RX_LINE; want = true;
    } else if ((s->ier & IER_RX_DATA) && (s->lsr & LSR_DR)) {
        iir = IIR_RX_DATA; want = true;
    } else if ((s->ier & IER_TX_EMPTY) && s->thr_empty_pending) {
        iir = IIR_TX_EMPTY; want = true;
    }

    s->iir = iir;
    if (want) qemu_irq_raise(s->irq);
    else      qemu_irq_lower(s->irq);
}

void calypso_uart_kick_rx(CalypsoUARTState *s)
{
    if (s->rx_count > 0 && (s->lsr & LSR_DR)) {
        qemu_irq_lower(s->irq);
        calypso_uart_update_irq(s);
    }
}

void calypso_uart_poll_backend(CalypsoUARTState *s)
{
    qemu_chr_fe_accept_input(&s->chr);
}

void calypso_uart_kick_tx(CalypsoUARTState *s) { (void)s; }

void calypso_uart_force_init(CalypsoUARTState *s)
{
    if (s->mdr1 != 0x00) s->mdr1 = 0x00;
    s->ier = 0x03;
    calypso_uart_update_irq(s);
}

/* ---- RX poll timer ---- */

#define UART_RX_POLL_NS  (10 * 1000 * 1000)

static void calypso_uart_rx_poll(void *opaque)
{
    CalypsoUARTState *s = (CalypsoUARTState *)opaque;
    qemu_chr_fe_accept_input(&s->chr);
    main_loop_wait(false);
    timer_mod(s->rx_poll_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 50);
}

/* ---- Char backend callbacks ---- */

int calypso_uart_can_receive(void *opaque)
{
    CalypsoUARTState *s = (CalypsoUARTState *)opaque;
    return CALYPSO_UART_RX_FIFO_SIZE - s->rx_count;
}

void calypso_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    CalypsoUARTState *s = (CalypsoUARTState *)opaque;

    if (s->label && !strcmp(s->label, "modem"))
        uart_log_raw("/tmp/qemu-modem-rx.raw", buf, size);
    else if (s->label && !strcmp(s->label, "irda"))
        uart_log_raw("/tmp/qemu-irda-rx.raw", buf, size);

    /* Sercomm DLCI routing on modem UART RX:
     * DLCI 4 (burst data) -> calypso_trx DSP RAM injection
     * Everything else -> FIFO for firmware
     *
     * FIX P2: parser state is now per-instance (s->sc_*), not static.
     */
    if (s->label && !strcmp(s->label, "modem")) {
        for (int i = 0; i < size; i++) {
            uint8_t b = buf[i];
            if (s->sc_state == 0) {
                if (b == 0x7E) { s->sc_state = 1; s->sc_len = 0; }
                else { fifo_push(s, b); }
            } else if (s->sc_state == 2) {
                if (s->sc_len < CALYPSO_SC_BUF_SIZE)
                    s->sc_buf[s->sc_len++] = b ^ 0x20;
                s->sc_state = 1;
            } else { /* sc_state == 1, in frame */
                if (b == 0x7E) {
                    if (s->sc_len >= 2 && s->sc_buf[0] == 4) {
                        /* DLCI 4: burst data → DSP */
                        calypso_trx_rx_burst(&s->sc_buf[2], s->sc_len - 2);
                    } else if (s->sc_len > 0) {
                        /* Other DLCI: reconstruct into FIFO */
                        fifo_push(s, 0x7E);
                        for (int j = 0; j < s->sc_len; j++)
                            fifo_push(s, s->sc_buf[j]);
                        fifo_push(s, 0x7E);
                    }
                    s->sc_len = 0;
                } else if (b == 0x7D) {
                    s->sc_state = 2;
                } else {
                    if (s->sc_len < CALYPSO_SC_BUF_SIZE)
                        s->sc_buf[s->sc_len++] = b;
                }
            }
        }
    } else {
        for (int i = 0; i < size; i++)
            fifo_push(s, buf[i]);
    }

    if (s->rx_count > 0) s->lsr |= LSR_DR;
    calypso_uart_update_irq(s);
}

void calypso_uart_inject_raw(CalypsoUARTState *s, const uint8_t *buf, int size)
{
    for (int i = 0; i < size; i++)
        fifo_push(s, buf[i]);
    if (s->rx_count > 0) s->lsr |= LSR_DR;
    calypso_uart_update_irq(s);
}

/* ---- MMIO ---- */

static uint64_t calypso_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    CalypsoUARTState *s = CALYPSO_UART(opaque);
    uint64_t val = 0;

    switch (offset) {
    case REG_RBR_THR:
        if (s->lcr & LCR_DLAB) {
            val = s->dll;
        } else {
            val = fifo_pop(s);
            if (s->rx_count > 0) s->lsr |= LSR_DR;
            else s->lsr &= ~LSR_DR;
            calypso_uart_update_irq(s);
        }
        break;
    case REG_IER:
        val = (s->lcr & LCR_DLAB) ? s->dlh : s->ier;
        break;
    case REG_IIR_FCR:
        if (s->lcr == LCR_CONF_BF) {
            val = s->efr;
        } else {
            val = s->iir;
            if ((s->iir & 0x0F) == IIR_TX_EMPTY) {
                s->tx_empty_reads++;
                if (s->tx_empty_reads >= 2) {
                    s->thr_empty_pending = false;
                    s->tx_empty_reads = 0;
                    calypso_uart_update_irq(s);
                }
            }
        }
        break;
    case REG_LCR: val = s->lcr; break;
    case REG_MCR: val = (s->lcr == LCR_CONF_BF) ? s->xon1 : s->mcr; break;
    case REG_LSR:
        if (s->lcr == LCR_CONF_BF) { val = s->xon2; }
        else { val = s->lsr; s->lsr &= ~LSR_OE; }
        break;
    case REG_MSR:
        val = (s->lcr == LCR_CONF_BF) ? s->xoff1 : (MSR_CTS | MSR_DSR | MSR_DCD);
        break;
    case REG_SPR: val = (s->lcr == LCR_CONF_BF) ? s->xoff2 : s->spr; break;
    case REG_MDR1: val = s->mdr1; break;
    case REG_SCR: val = s->scr; break;
    case REG_SSR: val = s->ssr & ~SSR_TX_FIFO_FULL; break;
    default: break;
    }
    return val;
}

static void calypso_uart_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    CalypsoUARTState *s = CALYPSO_UART(opaque);

    switch (offset) {
    case REG_RBR_THR:
        if (s->lcr & LCR_DLAB) {
            s->dll = value;
        } else {
            uint8_t ch = (uint8_t)value;
            if (s->label && !strcmp(s->label, "modem"))
                uart_log_raw("/tmp/qemu-modem-tx.raw", &ch, 1);
            else if (s->label && !strcmp(s->label, "irda"))
                uart_log_raw("/tmp/qemu-irda-tx.raw", &ch, 1);
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
            if (s->label && !strcmp(s->label, "modem"))
                l1ctl_sock_uart_tx_byte(ch);
            s->lsr |= LSR_THRE | LSR_TEMT;
            s->thr_empty_pending = true;
            s->tx_empty_reads = 0;
            calypso_uart_update_irq(s);
        }
        break;
    case REG_IER:
        if (s->lcr & LCR_DLAB) {
            s->dlh = value;
        } else {
            uint8_t old = s->ier;
            s->ier = value & 0x0F;
            if (!(old & IER_TX_EMPTY) && (s->ier & IER_TX_EMPTY) && (s->lsr & LSR_THRE))
                s->thr_empty_pending = true;
            calypso_uart_update_irq(s);
        }
        break;
    case REG_IIR_FCR:
        if (s->lcr == LCR_CONF_BF) {
            s->efr = value;
        } else {
            s->fcr = value;
            if (value & FCR_RX_RESET) { fifo_reset(s); s->lsr &= ~LSR_DR; }
            if (value & FCR_TX_RESET) { s->thr_empty_pending = false; s->lsr |= LSR_THRE | LSR_TEMT; }
            calypso_uart_update_irq(s);
        }
        break;
    case REG_LCR: s->lcr = value; break;
    case REG_MCR:
        if (s->lcr == LCR_CONF_BF) s->xon1 = value;
        else s->mcr = value;
        break;
    case REG_LSR:
        if (s->lcr == LCR_CONF_BF) s->xon2 = value;
        break;
    case REG_MSR:
        if (s->lcr == LCR_CONF_BF) s->xoff1 = value;
        break;
    case REG_SPR:
        if (s->lcr == LCR_CONF_BF) s->xoff2 = value;
        else s->spr = value;
        break;
    case REG_MDR1:
        s->mdr1 = value;
        calypso_fw_patch_apply();
        break;
    case REG_SCR: s->scr = value; break;
    case REG_SSR: s->ssr = value; break;
    default: break;
    }
}

static const MemoryRegionOps calypso_uart_ops = {
    .read = calypso_uart_read,
    .write = calypso_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/* ---- QOM ---- */

static void calypso_uart_realize(DeviceState *dev, Error **errp)
{
    CalypsoUARTState *s = CALYPSO_UART(dev);
    bool connected;

    memory_region_init_io(&s->iomem, OBJECT(dev), &calypso_uart_ops, s,
                          "calypso-uart", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    connected = qemu_chr_fe_backend_connected(&s->chr);

    if (connected) {
        qemu_chr_fe_set_handlers(&s->chr,
                                 calypso_uart_can_receive,
                                 calypso_uart_receive,
                                 NULL, NULL, s, NULL, true);
        s->rx_poll_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                        calypso_uart_rx_poll, s);
        timer_mod(s->rx_poll_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 10);
    }
}

static void calypso_uart_reset_state(DeviceState *dev)
{
    CalypsoUARTState *s = CALYPSO_UART(dev);

    s->ier = 0; s->iir = IIR_NO_INT; s->fcr = 0;
    s->lcr = 0; s->mcr = 0;
    s->lsr = LSR_THRE | LSR_TEMT;
    s->msr = MSR_CTS | MSR_DSR | MSR_DCD;
    s->spr = 0; s->dll = 0; s->dlh = 0; s->mdr1 = 0;
    s->efr = 0; s->xon1 = 0; s->xon2 = 0;
    s->xoff1 = 0; s->xoff2 = 0; s->scr = 0; s->ssr = 0;
    s->thr_empty_pending = false;
    s->tx_empty_reads = 0;
    fifo_reset(s);

    /* Reset sercomm parser state (P2 fix) */
    s->sc_len = 0;
    s->sc_state = 0;
}

static Property calypso_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", CalypsoUARTState, chr),
    DEFINE_PROP_STRING("label", CalypsoUARTState, label),
    DEFINE_PROP_END_OF_LIST(),
};

static void calypso_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = calypso_uart_realize;
    device_class_set_legacy_reset(dc, calypso_uart_reset_state);
    dc->desc = "Calypso UART";
    device_class_set_props(dc, calypso_uart_properties);
}

static const TypeInfo calypso_uart_info = {
    .name          = TYPE_CALYPSO_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CalypsoUARTState),
    .class_init    = calypso_uart_class_init,
};

static void calypso_uart_register_types(void)
{
    type_register_static(&calypso_uart_info);
}

type_init(calypso_uart_register_types)
