/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host: CDC-ACM. See usbh_cdc.h and docs/usb_host.md, "CDC".
 *
 * Enumeration (usbh.c) sends SET_LINE_CODING -- 115200 8N1, which a
 * native-USB device mostly ignores but a bridge chip that speaks CDC
 * uses -- and SET_CONTROL_LINE_STATE with DTR and RTS set, without
 * which many devices (the Pico's stdio among them) send nothing. Then
 * it binds here.
 *
 * The data path is bulk IN and OUT, one packet per transaction, run
 * from process context holding the transaction engine for each
 * transaction, as mass storage does. The notification endpoint (serial
 * state) is not polled; nothing here needs it.
 */

#include <stdio.h>
#include <string.h>

#include "usbh.h"
#include "usbh_hw.h"
#include "usbh_cdc.h"

// Packet-buffer areas, in the gap between mass storage (0x000-0x26f)
// and the auto-poll landing area (0x380). See usbh.c's scratch map.
#define CDC_OFF_RX      0x280u      // 64
#define CDC_OFF_TX      0x2c0u      // 64

#define CDC_CLASS_COMM  0x02
#define CDC_SUB_ACM     0x02
#define CDC_CLASS_DATA  0x0a

// Consecutive NAKs on a write before giving up: a device that never
// drains is not coming back. Paced by a short spin between tries.
#define CDC_WRITE_NAKS  20000

static struct {
    uint8_t addr, xa_flags, port;
    uint8_t ep_in, ep_out;
    uint8_t mps_in, mps_out;
    uint8_t tgl_in, tgl_out;
    volatile uint8_t ready;
} cdc;

// Walk the configuration: the ACM communications interface, then the
// data interface's bulk endpoints. Fills ep/mps when want_eps.
static int parse(const uint8_t *cfg, int n, int want_eps)
{
    int i = 0, comm = -1, in_data = 0;
    int got_in = 0, got_out = 0;

    while (i + 1 < n) {
        int len = cfg[i], type = cfg[i + 1];
        if (len < 2) break;
        if (type == 0x04 && i + 7 < n) {
            in_data = 0;
            if (cfg[i + 5] == CDC_CLASS_COMM && cfg[i + 6] == CDC_SUB_ACM &&
                comm < 0)
                comm = cfg[i + 2];
            if (cfg[i + 5] == CDC_CLASS_DATA) in_data = 1;
        }
        if (type == 0x05 && i + 6 < n && in_data &&
            (cfg[i + 3] & 0x03) == 0x02) {
            if (cfg[i + 2] & 0x80) {
                if (!got_in && want_eps) {
                    cdc.ep_in = cfg[i + 2] & 0x0f;
                    cdc.mps_in = cfg[i + 4];
                }
                got_in = 1;
            } else {
                if (!got_out && want_eps) {
                    cdc.ep_out = cfg[i + 2] & 0x0f;
                    cdc.mps_out = cfg[i + 4];
                }
                got_out = 1;
            }
        }
        i += len;
    }
    return (comm >= 0 && got_in && got_out) ? comm : -1;
}

int z_usbh_cdc_probe(const uint8_t *cfg, int n)
{
    return parse(cfg, n, 0);
}

int z_usbh_cdc_bind(uint8_t addr, uint8_t xa_flags, uint8_t port,
                    const uint8_t *cfg, int n)
{
    if (cdc.ready) return 0;            // one at a time
    memset(&cdc, 0, sizeof(cdc));
    if (parse(cfg, n, 1) < 0) return 0;
    if (cdc.mps_in == 0 || cdc.mps_in > 64) cdc.mps_in = 64;
    if (cdc.mps_out == 0 || cdc.mps_out > 64) cdc.mps_out = 64;
    cdc.addr = addr;
    cdc.xa_flags = xa_flags;
    cdc.port = port;
    cdc.ready = 1;
    printf("usb cdc: addr %d ep in %d out %d, mps %d/%d\n", addr,
           cdc.ep_in, cdc.ep_out, cdc.mps_in, cdc.mps_out);
    return 1;
}

void z_usbh_cdc_unbind(void)
{
    if (!cdc.ready) return;
    cdc.ready = 0;
    printf("usb cdc: device removed\n");
}

int z_usbh_cdc_present(void)
{
    return cdc.ready;
}

// One transaction on the engine, which the caller holds. Returns XACT_S,
// or 0 if the engine never finished.
static uint32_t xact(int in, uint32_t off, uint16_t len)
{
    uint32_t s = Z_USBH_XS_PENDING, guard;

    z_usbh_wr(Z_USBH_XACT_A,
              Z_USBH_XA_ADDR(cdc.addr) |
              Z_USBH_XA_ENDP(in ? cdc.ep_in : cdc.ep_out) |
              Z_USBH_XA_PID(in ? Z_USBH_PID_IN : Z_USBH_PID_OUT) |
              Z_USBH_XA_PORT(cdc.port) |
              Z_USBH_XA_MPS(in ? cdc.mps_in : cdc.mps_out) |
              ((uint32_t)cdc.xa_flags << 13) |
              ((in ? cdc.tgl_in : cdc.tgl_out) ? Z_USBH_XA_TOGGLE : 0));
    // NAK budget 0: a NAK comes straight back. A read that finds
    // nothing returns at once, and a write paces its own retries.
    z_usbh_wr(Z_USBH_XACT_B, Z_USBH_XB_OFF(off) | Z_USBH_XB_LEN(len) |
                             Z_USBH_XB_NAK(0) | Z_USBH_XB_START);
    for (guard = 0; guard < 2000000u; guard++) {
        s = z_usbh_rd(Z_USBH_XACT_S);
        if (!(s & Z_USBH_XS_PENDING)) break;
    }
    if (s & Z_USBH_XS_PENDING) return 0;
    // The engine reports the toggle the NEXT transaction must carry,
    // advanced only on success.
    if (in) cdc.tgl_in = (uint8_t)Z_USBH_XS_TOGGLE(s);
    else cdc.tgl_out = (uint8_t)Z_USBH_XS_TOGGLE(s);
    return s;
}

int z_usbh_cdc_read(uint8_t *buf, int max)
{
    uint32_t s;
    uint8_t st;
    int n, i;

    if (!cdc.ready || max <= 0) return -1;
    if (!z_usbh_bus_reserve()) return 0;    // busy: try again later
    if (!cdc.ready) { z_usbh_bus_release(); return -1; }
    s = xact(1, CDC_OFF_RX, cdc.mps_in);
    z_usbh_bus_release();
    if (!s) return -1;

    st = (uint8_t)Z_USBH_XS_STATUS(s);
    if (st == Z_USBH_ST_NAK) return 0;
    if (st != Z_USBH_ST_OK && st != Z_USBH_ST_SHORT) return -1;

    n = (int)Z_USBH_XS_LEN(s);
    if (n > max) n = max;           // the rest of the packet is lost
    for (i = 0; i < n; i++)
        buf[i] = z_usbh_rb(Z_USBH_BUF + CDC_OFF_RX + (uint32_t)i);
    return n;
}

int z_usbh_cdc_write(const uint8_t *buf, int len)
{
    int sent = 0, chunk, i, naks = 0;
    uint32_t s, spin;
    uint8_t st;

    if (!cdc.ready) return -1;
    while (sent < len) {
        chunk = len - sent;
        if (chunk > cdc.mps_out) chunk = cdc.mps_out;
        for (i = 0; i < chunk; i++)
            z_usbh_wb(Z_USBH_BUF + CDC_OFF_TX + (uint32_t)i, buf[sent + i]);

        if (!z_usbh_bus_reserve()) {
            if (++naks > CDC_WRITE_NAKS) return sent ? sent : -1;
            continue;
        }
        if (!cdc.ready) { z_usbh_bus_release(); return -1; }
        s = xact(0, CDC_OFF_TX, (uint16_t)chunk);
        z_usbh_bus_release();
        if (!s) return -1;

        st = (uint8_t)Z_USBH_XS_STATUS(s);
        if (st == Z_USBH_ST_NAK) {
            // Not ready for more yet. Pace, then the same chunk again.
            if (++naks > CDC_WRITE_NAKS) return sent ? sent : -1;
            for (spin = 0; spin < 2000u; spin++) { }
            continue;
        }
        if (st != Z_USBH_ST_OK) return sent ? sent : -1;
        naks = 0;
        sent += chunk;
    }
    return sent;
}
