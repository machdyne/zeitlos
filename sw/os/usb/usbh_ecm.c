/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host: CDC-ECM (USB ethernet). See usbh_ecm.h and
 * docs/usb_ethernet.md.
 *
 * -- Framing --
 *
 * ECM carries one raw Ethernet frame (no FCS) per bulk transfer, ended
 * by a short packet: a frame whose length is an exact multiple of the
 * endpoint's packet size is followed by a zero-length packet. Nothing
 * else -- no header, no length field -- so the short packet is the only
 * frame boundary there is.
 *
 * -- The packet buffer --
 *
 * A frame is up to 1514 bytes and the 2 KB packet buffer has no free
 * region that size. So both directions move a frame in 512-byte
 * auto-continue chunks through the mass storage sector area (0x000),
 * copying each chunk out or in before the next. That is safe because
 * both drivers only touch that area in process context with the
 * scheduler held -- MSC inside FatFs, this inside Z_SYS_USBNET (see
 * kernel.c, k_syscall_touches_fs) -- so neither can be switched out
 * with the other's data half-copied, and nothing in the ISR uses it.
 * The notification endpoint lands in the free 16 bytes at 0x270.
 */

#include <stdio.h>
#include <string.h>

#include "usbh.h"
#include "usbh_hw.h"
#include "usbh_int.h"
#include "usbh_ecm.h"

#define ECM_OFF         0x000u      // 512, MSC's sector area; see above
#define ECM_OFF_INT     0x270u      // 16, notifications
#define ECM_CHUNK       512u
// Longest frame accepted. A frame that runs past this is drained to
// its short packet and dropped rather than taken as two frames.
#define ECM_MAX         1536
// NAKs tolerated in the middle of a frame (IN) or before the adapter
// takes one (OUT), paced by a short spin. An adapter that stops half
// way through a frame for this long is not going to finish it.
#define ECM_NAKS        2000
#define ECM_STRIKES     3           // bad CRC / no answer, per USB 2.0 8.5.2
// How often an idle receive also reads the notification endpoint, in
// kernel ticks: about four times a second.
#define ECM_LINK_TICKS  180u

#define PKT_DIRECTED    0x04        // CDC ECM 1.2, 6.2.4
#define PKT_BROADCAST   0x08
#define PKT_PROMISC     0x01

typedef struct {
    int8_t comm;
    uint8_t data, alt, imac;
    uint8_t ep_in, ep_out, ep_int;
    uint8_t mps_in, mps_out, mps_int;
    uint16_t maxseg;
} ecm_desc_t;

static struct {
    ecm_desc_t d;                   // from the probe
    uint8_t addr, xa, port, dev;
    uint8_t tgl_in, tgl_out, tgl_int;
    uint8_t mac[6], mac_ok, promisc, link;
    volatile uint8_t ready;
    uint32_t link_t;
    uint32_t rx, tx, rx_err, tx_err, rx_drop;
} ecm;

// The device mid-enumeration whose configuration is in ecm.d, or -1.
static int8_t pend = -1;
static volatile uint32_t ecm_gen;
static uint8_t want[6], want_ok;

// ---------------------------------------------------------------
// descriptors
// ---------------------------------------------------------------

// Walk the configuration. The data interface's alternate settings are
// separate interface descriptors -- alt 0 with no endpoints, then the
// one with the bulk pair -- so a data interface's endpoints are taken
// when the NEXT interface descriptor (or the end) closes it.
static int parse(const uint8_t *c, int n, ecm_desc_t *e)
{
    int i, cls = -1, num = 0, alt = 0, end;
    uint8_t in = 0, out = 0, mi = 0, mo = 0;
    const uint8_t *p;

    memset(e, 0, sizeof(*e));
    e->comm = -1;
    e->data = 0xff;
    for (i = 0; ; i += p[0]) {
        p = c + i;
        end = i + 2 > n || p[0] < 2 || i + p[0] > n;
        if (end || p[1] == 0x04) {
            if (cls == 0x0a && in && out && !e->ep_in &&
                (e->data == 0xff || e->data == num)) {
                e->data = (uint8_t)num;
                e->alt = (uint8_t)alt;
                e->ep_in = in; e->ep_out = out;
                e->mps_in = mi; e->mps_out = mo;
            }
            if (end) break;
            if (p[0] < 9) continue;
            num = p[2]; alt = p[3]; cls = p[5];
            in = out = 0;
            if (cls == 0x02 && p[6] == 0x06 && e->comm < 0)
                e->comm = (int8_t)num;
        } else if (p[1] == 0x24 && num == e->comm) {
            // class-specific: union (6), Ethernet networking (0x0f)
            if (p[2] == 0x06) e->data = p[4];
            if (p[2] == 0x0f) {
                e->imac = p[3];
                e->maxseg = (uint16_t)(p[8] | (p[9] << 8));
            }
        } else if (p[1] == 0x05) {
            if (num == e->comm && (p[3] & 3) == 3 && (p[2] & 0x80)) {
                e->ep_int = p[2] & 0x0f;
                e->mps_int = p[4];
            } else if (cls == 0x0a && (p[3] & 3) == 2) {
                if (p[2] & 0x80) { in = p[2] & 0x0f; mi = p[4]; }
                else { out = p[2] & 0x0f; mo = p[4]; }
            }
        }
    }
    return (e->comm >= 0 && e->ep_in) ? e->comm : -1;
}

int z_usbh_ecm_probe(int dev, const uint8_t *cfg, int n)
{
    int r;
    if (ecm.ready || (pend >= 0 && pend != dev)) return -1;
    r = parse(cfg, n, &ecm.d);
    if (r >= 0) {
        pend = (int8_t)dev;
        ecm.mac_ok = 0;
    }
    return r;
}

void z_usbh_ecm_ids(uint8_t *imac, uint8_t *data, uint8_t *alt)
{
    *imac = ecm.d.imac;
    *data = ecm.d.data;
    *alt = ecm.d.alt;
}

// iMACAddress: twelve hex digits, UTF-16LE, most significant first
// (CDC ECM 1.2, 5.4). Anything else leaves mac_ok clear, and the
// adapter is then run promiscuous under net's own address.
void z_usbh_ecm_mac(uint32_t addr)
{
    int k, v;
    uint8_t ch, m[6];
    if (z_usbh_rb(addr) < 26) return;
    for (k = 0; k < 12; k++) {
        ch = z_usbh_rb(addr + 2 + (uint32_t)k * 2);
        v = ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10;
        if (v < 0 || v > 15) return;
        m[k >> 1] = (uint8_t)(m[k >> 1] << 4 | v);
    }
    memcpy(ecm.mac, m, 6);
    ecm.mac_ok = 1;
}

// Directed and broadcast is what an address of its own needs. With no
// address read, or when net already sends from a different one -- an
// adapter swapped while net runs keeps net's first address, so its
// DHCP lease and every peer's ARP entry stay good -- the adapter must
// also pass frames addressed to that one, which only promiscuous mode
// does.
uint16_t z_usbh_ecm_filter(void)
{
    ecm.promisc = !ecm.mac_ok ||
                  (want_ok && memcmp(want, ecm.mac, 6) != 0);
    return (uint16_t)(PKT_DIRECTED | PKT_BROADCAST |
                      (ecm.promisc ? PKT_PROMISC : 0));
}

void z_usbh_ecm_bind(int dev, uint8_t addr, uint8_t xa_flags, uint8_t port)
{
    ecm.addr = addr;
    ecm.xa = xa_flags;
    ecm.port = port;
    ecm.dev = (uint8_t)dev;
    // Full speed: bulk is 64 at most, and an interrupt report here is
    // read one packet at a time into 16 bytes.
    if (ecm.d.mps_in == 0 || ecm.d.mps_in > 64) ecm.d.mps_in = 64;
    if (ecm.d.mps_out == 0 || ecm.d.mps_out > 64) ecm.d.mps_out = 64;
    if (ecm.d.mps_int == 0 || ecm.d.mps_int > 16) ecm.d.mps_int = 16;
    // SET_CONFIGURATION and SET_INTERFACE reset every endpoint to DATA0.
    ecm.tgl_in = ecm.tgl_out = ecm.tgl_int = 0;
    ecm.link = Z_USBNET_LINK_UNKNOWN;
    ecm.link_t = usbh_ticks();
    pend = -1;
    ecm_gen++;
    ecm.ready = 1;
    printf("usb ecm: addr %d, ep %d/%d/%d\n", addr,
           ecm.d.ep_in, ecm.d.ep_out, ecm.d.ep_int);
}

void z_usbh_ecm_forget(int dev)
{
    if (pend == dev) pend = -1;
    if (ecm.ready && ecm.dev == dev) {
        ecm.ready = 0;
        ecm_gen++;
        printf("usb ecm: adapter removed\n");
    }
}

void z_usbh_ecm_want(const uint8_t mac[6])
{
    memcpy(want, mac, 6);
    want_ok = 1;
}

void z_usbh_ecm_info(z_usbnet_info_t *in)
{
    memset(in, 0, sizeof(*in));
    in->present = ecm.ready;
    in->gen = ecm_gen;
    in->rx = ecm.rx; in->tx = ecm.tx;
    in->rx_err = ecm.rx_err; in->tx_err = ecm.tx_err;
    in->rx_drop = ecm.rx_drop;
    if (!ecm.ready) return;
    in->mac_ok = ecm.mac_ok;
    in->link = ecm.link;
    in->promisc = ecm.promisc;
    in->maxseg = ecm.d.maxseg;
    memcpy(in->mac, ecm.mac, 6);
}

void z_usbh_ecm_dump(const char *pre)
{
    const uint8_t *m = ecm.mac;
    printf("%secm: %02x:%02x:%02x:%02x:%02x:%02x%s link %d, "
           "rx %lu tx %lu err %lu/%lu\n", pre, m[0], m[1], m[2], m[3],
           m[4], m[5], ecm.promisc ? " promisc" : "", ecm.link,
           (unsigned long)ecm.rx, (unsigned long)ecm.tx,
           (unsigned long)ecm.rx_err, (unsigned long)ecm.tx_err);
}

// ---------------------------------------------------------------
// data path: process context, engine held
// ---------------------------------------------------------------

// One request on the engine. Returns XACT_S, or ~0 if the engine never
// finished. The toggle comes back for the next request either way: the
// engine advances it only for packets that were acknowledged.
static uint32_t run(int pid, uint8_t ep, uint8_t mps, uint8_t *tgl,
                    uint32_t off, uint32_t len, int ac, int nak)
{
    uint32_t s = Z_USBH_XS_PENDING, g;

    z_usbh_wr(Z_USBH_XACT_A,
              Z_USBH_XA_ADDR(ecm.addr) | Z_USBH_XA_ENDP(ep) |
              Z_USBH_XA_PID(pid) | Z_USBH_XA_PORT(ecm.port) |
              Z_USBH_XA_MPS(mps) | ((uint32_t)ecm.xa << 13) |
              (*tgl ? Z_USBH_XA_TOGGLE : 0) |
              (ac ? Z_USBH_XA_AUTOCONT : 0));
    z_usbh_wr(Z_USBH_XACT_B, Z_USBH_XB_OFF(off) | Z_USBH_XB_LEN(len) |
                             Z_USBH_XB_NAK(nak) | Z_USBH_XB_START);
    for (g = 0; g < 2000000u; g++) {
        s = z_usbh_rd(Z_USBH_XACT_S);
        if (!(s & Z_USBH_XS_PENDING)) break;
    }
    if (s & Z_USBH_XS_PENDING) return ~0u;
    *tgl = (uint8_t)Z_USBH_XS_TOGGLE(s);
    return s;
}

static void pace(void)
{
    volatile uint32_t g;
    for (g = 0; g < 2000u; g++) { }
}

// Word accesses: a quarter of the bus cycles of byte ones. The buffer
// is little-endian, one byte lane per address (usb_host.v).
static void copy_out(uint8_t *dst, int n)
{
    int i;
    uint32_t w;
    for (i = 0; i < n; i += 4) {
        w = z_usbh_rd(Z_USBH_BUF + ECM_OFF + (uint32_t)i);
        dst[i] = (uint8_t)w;
        if (i + 1 < n) dst[i + 1] = (uint8_t)(w >> 8);
        if (i + 2 < n) dst[i + 2] = (uint8_t)(w >> 16);
        if (i + 3 < n) dst[i + 3] = (uint8_t)(w >> 24);
    }
}

static void copy_in(const uint8_t *src, int n)
{
    int i;
    uint32_t w;
    for (i = 0; i < n; i += 4) {
        w = src[i];
        if (i + 1 < n) w |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < n) w |= (uint32_t)src[i + 2] << 16;
        if (i + 3 < n) w |= (uint32_t)src[i + 3] << 24;
        z_usbh_wr(Z_USBH_BUF + ECM_OFF + (uint32_t)i, w);
    }
}

// NETWORK_CONNECTION (CDC 1.2 PSTN / ECM 6.3.1): 0xa1, 0x00, wValue
// 1 connected, 0 not. Everything else -- CONNECTION_SPEED_CHANGE and
// its data -- is ignored.
static void link_poll(void)
{
    uint32_t s, a = Z_USBH_BUF + ECM_OFF_INT;

    ecm.link_t = usbh_ticks();
    if (!ecm.d.ep_int) return;
    s = run(Z_USBH_PID_IN, ecm.d.ep_int, ecm.d.mps_int, &ecm.tgl_int,
            ECM_OFF_INT, ecm.d.mps_int, 0, 0);
    if (s == ~0u) return;
    if (Z_USBH_XS_STATUS(s) != Z_USBH_ST_OK &&
        Z_USBH_XS_STATUS(s) != Z_USBH_ST_SHORT) return;
    if (Z_USBH_XS_LEN(s) >= 8 && z_usbh_rb(a) == 0xa1 &&
        z_usbh_rb(a + 1) == 0x00)
        ecm.link = z_usbh_rb(a + 2) ? Z_USBNET_LINK_UP
                                    : Z_USBNET_LINK_DOWN;
}

int z_usbh_ecm_recv(uint8_t *buf, int max)
{
    uint32_t s, g0;
    int total = 0, naks = 0, strikes = 0, n, take, r;
    uint8_t st;

    if (!ecm.ready) return -1;
    if (!z_usbh_bus_reserve()) return 0;        // busy: next time
    g0 = ecm_gen;

    for (;;) {
        r = -1;
        if (!ecm.ready || ecm_gen != g0) break;
        // Idle, a NAK comes straight back and the call returns: this
        // is polled. Mid-frame, the adapter has committed to the rest,
        // so a few quick hardware retries.
        s = run(Z_USBH_PID_IN, ecm.d.ep_in, ecm.d.mps_in, &ecm.tgl_in,
                ECM_OFF, ECM_CHUNK, 1, total ? 3 : 0);
        if (s == ~0u) break;
        st = (uint8_t)Z_USBH_XS_STATUS(s);
        n = (int)Z_USBH_XS_LEN(s);

        // What landed is good whatever ended the request: a NAK, a bad
        // CRC and the end-of-frame guard all stop the engine without
        // advancing past the packets it acknowledged. Take them, then
        // ask again for the rest.
        if (n) {
            take = n;
            if (total + take > max) take = max - total;
            if (take > 0) copy_out(buf + total, take);
            total += n;
            naks = 0;
            strikes = 0;
        }

        if (st == Z_USBH_ST_SHORT) { r = total; break; }  // frame ends
        if (st == Z_USBH_ST_OK) {
            // A whole chunk and no short packet yet: the frame goes on.
            // Past ECM_MAX it is no frame of ours; keep reading to its
            // end so the next one starts in the right place.
            if (total > ECM_MAX + 16 * (int)ECM_CHUNK) break;
            continue;
        }
        if (st == Z_USBH_ST_NAK) {
            if (!total) { r = 0; break; }       // nothing waiting
            if (++naks > ECM_NAKS) break;
            pace();
            continue;
        }
        if ((st == Z_USBH_ST_CRCERR || st == Z_USBH_ST_TIMEOUT) &&
            ++strikes <= ECM_STRIKES)
            continue;
        break;                                  // STALL, babble, abort
    }

    if (r == 0 && usbh_ticks() - ecm.link_t > ECM_LINK_TICKS)
        link_poll();
    z_usbh_bus_release();

    if (r < 0) {
        if (!ecm.ready || ecm_gen != g0) return -1;
        ecm.rx_err++;
        return 0;
    }
    if (r > max || r > ECM_MAX) {
        ecm.rx_drop++;
        return 0;
    }
    if (r) ecm.rx++;
    return r;
}

int z_usbh_ecm_send(const uint8_t *buf, int len)
{
    uint32_t s, g0;
    int done = 0, chunk, sent, naks = 0, zlp;
    uint8_t st;

    if (!ecm.ready || len <= 0 || len > ECM_MAX) return -1;
    if (!z_usbh_bus_reserve()) { ecm.tx_err++; return -1; }
    g0 = ecm_gen;
    // An exact multiple of the packet size needs a zero-length packet
    // to end it; otherwise the adapter takes the next frame as more of
    // this one.
    zlp = (len % ecm.d.mps_out) == 0;

    while (done < len || zlp) {
        chunk = len - done;
        if (chunk > (int)ECM_CHUNK) chunk = ECM_CHUNK;
        if (chunk) copy_in(buf + done, chunk);
        else zlp = 0;
        sent = 0;
        for (;;) {
            if (!ecm.ready || ecm_gen != g0) goto fail;
            s = run(Z_USBH_PID_OUT, ecm.d.ep_out, ecm.d.mps_out,
                    &ecm.tgl_out, ECM_OFF + (uint32_t)sent,
                    (uint32_t)(chunk - sent), chunk != 0, 3);
            if (s == ~0u) goto fail;
            st = (uint8_t)Z_USBH_XS_STATUS(s);
            if (Z_USBH_XS_LEN(s)) naks = 0;
            sent += (int)Z_USBH_XS_LEN(s);
            if (st == Z_USBH_ST_OK) break;
            // Its buffer is full: it will take the frame shortly.
            if (st == Z_USBH_ST_NAK && ++naks <= ECM_NAKS) {
                pace();
                continue;
            }
            goto fail;
        }
        done += chunk;
    }
    z_usbh_bus_release();
    ecm.tx++;
    return 0;

fail:
    z_usbh_bus_release();
    ecm.tx_err++;
    return -1;
}
