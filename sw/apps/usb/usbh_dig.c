/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host stack -- digitizers: touch screens and pen tablets. See
 * usbh_dig.h and docs/touch.md.
 */

#include "usbh_dig.h"

#define PAGE_DESKTOP    0x01
#define PAGE_BUTTON     0x09
#define PAGE_DIGITIZER  0x0d

#define U(p, u)         (((uint32_t)(p) << 16) | (u))
#define U_X             U(PAGE_DESKTOP, 0x30)
#define U_Y             U(PAGE_DESKTOP, 0x31)
#define U_MOUSE         U(PAGE_DESKTOP, 0x02)
#define U_POINTER       U(PAGE_DESKTOP, 0x01)
#define U_PEN           U(PAGE_DIGITIZER, 0x02)
#define U_TOUCHSCREEN   U(PAGE_DIGITIZER, 0x04)
#define U_TOUCHPAD      U(PAGE_DIGITIZER, 0x05)
#define U_TIP           U(PAGE_DIGITIZER, 0x42)
#define U_BARREL        U(PAGE_DIGITIZER, 0x44)
#define U_INRANGE       U(PAGE_DIGITIZER, 0x32)
#define U_CONTACTS      U(PAGE_DIGITIZER, 0x54)

static int32_t sext(uint32_t v, int sz)
{
    if (sz == 1) return (int8_t)v;
    if (sz == 2) return (int16_t)v;
    return (int32_t)v;
}

static void lay_clear(z_usbh_dlay_t *l, uint8_t kind)
{
    l->kind = kind;
    l->id = 0;
    l->x_bit = l->y_bit = l->tip_bit = l->btn2_bit = l->range_bit = l->cnt_bit = -1;
    l->x_sz = l->y_sz = l->cnt_sz = 0;
    l->x_min = l->y_min = 0;
    l->x_max = l->y_max = 0;
    l->bytes = 0;
}

static int lay_ok(const z_usbh_dlay_t *l)
{
    return l->kind != DIG_NONE && l->x_bit >= 0 && l->y_bit >= 0 &&
           l->x_sz && l->x_sz <= 32 && l->y_sz && l->y_sz <= 32 &&
           l->x_max > l->x_min && l->y_max > l->y_min &&
           (l->tip_bit >= 0 || l->kind == DIG_PEN);
}

// A device often has more than one application collection -- a touch
// monitor its "mouse mode" beside its touch screen, a tablet a mouse
// beside its pen. Each is read into `c`; at its end the best usable one
// so far is kept: a touch screen, then a pen, then an absolute mouse.
int z_usbh_dig_parse(const uint8_t *rd, int n, z_usbh_dlay_t *l)
{
    uint32_t page = 0, rsize = 0, rcount = 0, id = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16];
    int nusage = 0, i = 0, depth = 0;
    int32_t umin = -1, umax = -1;
    int bit = 0, sp = 0;
    z_usbh_dlay_t c;
    // Push and Pop (an XP-Pen's pen interface uses them around X and Y)
    struct { uint32_t page, rsize, rcount, id; int32_t lmin, lmax; } stack[4];

    lay_clear(l, DIG_NONE);
    lay_clear(&c, DIG_NONE);

    while (i < n) {
        uint8_t p = rd[i];
        int sz = p & 3, type = (p >> 2) & 3, tag = p >> 4, k;
        uint32_t v = 0;
        if (p == 0xfe) break;               // long item: nothing here uses one
        if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        for (k = 0; k < sz; k++) v |= (uint32_t)rd[i + 1 + k] << (8 * k);
        i += 1 + sz;

        if (type == 1) {                    // global
            if (tag == 0) page = v;
            else if (tag == 1) lmin = sext(v, sz);
            else if (tag == 2) {
                // signed by the HID spec; a maximum written as 0xFF in
                // one byte over a minimum of 0 means 255, as devices mean it
                lmax = sext(v, sz);
                if (lmax < 0 && lmin >= 0) lmax = (int32_t)v;
            }
            else if (tag == 7) rsize = v;
            else if (tag == 8) {            // a report of its own: its own bits
                if (c.x_bit < 0) { lay_clear(&c, c.kind); bit = 0; }
                id = v;
            }
            else if (tag == 9) rcount = v;
            else if (tag == 10) {           // push
                if (sp >= 4) break;
                stack[sp].page = page; stack[sp].rsize = rsize; stack[sp].rcount = rcount;
                stack[sp].id = id; stack[sp].lmin = lmin; stack[sp].lmax = lmax;
                sp++;
            } else if (tag == 11) {         // pop
                if (sp <= 0) break;
                sp--;
                page = stack[sp].page; rsize = stack[sp].rsize; rcount = stack[sp].rcount;
                id = stack[sp].id; lmin = stack[sp].lmin; lmax = stack[sp].lmax;
            }
        } else if (type == 2) {             // local
            uint32_t u = (sz == 4) ? v : ((page << 16) | v);
            if (tag == 0 && nusage < 16) usages[nusage++] = u;
            else if (tag == 1) umin = (int32_t)u;
            else if (tag == 2) umax = (int32_t)u;
        } else if (type == 0) {             // main
            if (tag == 10) {                // collection
                // An application collection says what the device is.
                if (v == 1 && depth == 0) {
                    uint32_t u = nusage ? usages[0] : 0;
                    lay_clear(&c, (u == U_TOUCHSCREEN || u == U_TOUCHPAD) ? DIG_TOUCH :
                                  u == U_PEN ? DIG_PEN :
                                  (u == U_MOUSE || u == U_POINTER) ? DIG_ABS : DIG_NONE);
                    c.id = (uint8_t)id;
                    bit = 0;
                }
                depth++;
            } else if (tag == 12) {         // end collection
                if (depth > 0) depth--;
                if (depth == 0) {
                    if (lay_ok(&c) && (l->kind == DIG_NONE || c.kind < l->kind)) *l = c;
                    lay_clear(&c, DIG_NONE);
                }
            } else if (tag == 8 && c.kind != DIG_NONE) {     // input
                uint32_t f;
                for (f = 0; f < rcount && f < 64; f++) {
                    uint32_t u = 0;
                    if (nusage > 0)
                        u = usages[f < (uint32_t)nusage ? f : (uint32_t)nusage - 1];
                    else if (umin >= 0) {
                        u = (uint32_t)umin + f;
                        if (umax >= 0 && u > (uint32_t)umax) u = (uint32_t)umax;
                    }
                    if (!(v & 1) && (c.x_bit < 0 || c.id == (uint8_t)id)) {     // data, in its report
                        c.id = (uint8_t)id;
                        if (u == U_X && c.x_bit < 0 && !(v & 4)) {
                            c.x_bit = (int16_t)bit; c.x_sz = (uint8_t)rsize;
                            c.x_min = lmin; c.x_max = lmax;
                        } else if (u == U_Y && c.y_bit < 0 && !(v & 4)) {
                            c.y_bit = (int16_t)bit; c.y_sz = (uint8_t)rsize;
                            c.y_min = lmin; c.y_max = lmax;
                        } else if ((u == U_TIP || u == U(PAGE_BUTTON, 1)) && c.tip_bit < 0) {
                            c.tip_bit = (int16_t)bit;
                        } else if ((u == U_BARREL || u == U(PAGE_BUTTON, 2)) && c.btn2_bit < 0) {
                            c.btn2_bit = (int16_t)bit;
                        } else if (u == U_INRANGE && c.range_bit < 0) {
                            c.range_bit = (int16_t)bit;
                        } else if (u == U_CONTACTS && c.cnt_bit < 0) {
                            c.cnt_bit = (int16_t)bit; c.cnt_sz = (uint8_t)rsize;
                        }
                    }
                    bit += (int)rsize;
                }
                if (c.id == (uint8_t)id)
                    c.bytes = (uint16_t)((bit + 7) / 8 + (id ? 1 : 0));
            }
            nusage = 0; umin = umax = -1;   // locals end at a main item
        }
    }

    return lay_ok(l);
}

static uint32_t bits(const uint8_t *r, int n, int at, int sz)
{
    uint32_t v = 0;
    int k;
    for (k = 0; k < sz; k++) {
        int b = at + k;
        if ((b >> 3) >= n) break;
        if (r[b >> 3] & (1u << (b & 7))) v |= 1u << k;
    }
    return v;
}

// raw in [lo, hi] onto 0 .. span-1, in 32 bits: the range is shifted
// down until (raw - lo) * span cannot overflow
static int scale(uint32_t raw, int32_t lo, int32_t hi, int span)
{
    int32_t v = (int32_t)raw - lo, w = hi - lo;
    if (v < 0) v = 0;
    if (v > w) v = w;
    while (w > 0x3fffff) { v >>= 1; w >>= 1; }
    return (int)((uint32_t)v * (uint32_t)(span - 1) / (uint32_t)w);
}

int z_usbh_dig_decode(const z_usbh_dlay_t *l, z_usbh_dstate_t *st,
                      const uint8_t *r, int n, uint32_t *word)
{
    int x, y, b = 0, touch = l->kind == DIG_TOUCH, count = 1;

    if (l->id) {
        if (n < 1 || r[0] != l->id) return 0;
        r++; n--;
    }

    x = scale(bits(r, n, l->x_bit, l->x_sz), l->x_min, l->x_max, 640);
    y = scale(bits(r, n, l->y_bit, l->y_sz), l->y_min, l->y_max, 480);
    if (l->tip_bit >= 0 && bits(r, n, l->tip_bit, 1)) b |= 1;
    if (l->btn2_bit >= 0 && bits(r, n, l->btn2_bit, 1)) b |= 2;
    if (l->cnt_bit >= 0) count = (int)bits(r, n, l->cnt_bit, l->cnt_sz);

    // A pen out of range: the pointer goes back to the mouse.
    if (l->kind == DIG_PEN && l->range_bit >= 0 && !bits(r, n, l->range_bit, 1) && !b) {
        st->two = 0;
        *word = st->last = 0;
        return 1;
    }

    // Two fingers: no button, and the wheel by how far they moved --
    // a notch every 16 screen rows, down the screen is toward the user
    if (touch && count >= 2) {
        if (!st->two) { st->two = 1; st->two_y = (int16_t)y; }
        while (y - st->two_y >= 16) { st->wheel = (uint8_t)(st->wheel - 1); st->two_y += 16; }
        while (st->two_y - y >= 16) { st->wheel = (uint8_t)(st->wheel + 1); st->two_y -= 16; }
        b = 0;
        x = (int)(st->last & 0x3ff);
        y = (int)((st->last >> 10) & 0x3ff);
    } else if (count < 2) {
        st->two = 0;
    }

    *word = st->last = (uint32_t)x | ((uint32_t)y << 10) | ((uint32_t)(b & 7) << 20) |
        (1u << 24) | (touch ? 1u << 25 : 0) | ((uint32_t)(st->wheel & 0x3f) << 26);
    return 1;
}

#define DESC_INTERFACE  0x04
#define DESC_ENDPOINT   0x05
#define DESC_HID        0x21
#define DESC_REPORT     0x22

int z_usbh_dig_iface(const uint8_t *cfg, int n, int skip_iface,
                     uint8_t *ep, uint8_t *mps, uint8_t *ival, int *rdlen)
{
    int i = 0, cur = -1, found = -1;

    *rdlen = 0;
    while (i + 1 < n) {
        int len = cfg[i], type = cfg[i + 1];
        if (len < 2) break;
        if (type == DESC_INTERFACE && i + 8 < n) {
            if (found >= 0) break;          // the one before had no endpoint
            cur = -1;
            // HID, and not a boot keyboard: a digitizer, or a mouse that
            // may be an absolute one
            if (cfg[i + 5] == 0x03 && !(cfg[i + 6] == 1 && cfg[i + 7] == 1) &&
                cfg[i + 2] != skip_iface)
                cur = cfg[i + 2];
            *rdlen = 0;
        } else if (type == DESC_HID && cur >= 0 && i + 8 < n && cfg[i + 6] == DESC_REPORT) {
            *rdlen = cfg[i + 7] | (cfg[i + 8] << 8);
        } else if (type == DESC_ENDPOINT && cur >= 0 && i + 6 < n &&
                   (cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3) {
            *ep = cfg[i + 2] & 0x0f;
            *mps = cfg[i + 4];
            *ival = cfg[i + 6] ? cfg[i + 6] : 1;
            if (*rdlen) return cur;
        }
        i += len;
    }
    return -1;
}
