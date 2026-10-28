/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * `usb`: the USB host stack as a driver process. Phase 3 of
 * docs/userspace_drivers.md; docs/usb_driver.md.
 *
 * The stack itself (usbh*.c) is what the kernel ran until now, nearly
 * unchanged: a state machine stepped by z_usbh_poll(), class drivers
 * for hubs, HID, mass storage, ethernet and serial. What this file adds
 * is the process around it:
 *
 *   - IRQ 9 wakes it (z_irq_bind()). The kernel silences the line by
 *     clearing the controller's IRQEN when it fires, so a level cannot
 *     hold the CPU while this process waits its turn; a poll clears
 *     IRQSTAT and turns IRQEN back on.
 *   - A timeout wakes it when the stack waits on time (debounce, a
 *     hub's status endpoint, the ethernet adapter's receive endpoint)
 *     and never otherwise: settled with no hub, no adapter in use and
 *     no serial reader, it sleeps until the next interrupt or request.
 *   - Requests from its clients (sw/common/zusbproto.h): `fs` for the
 *     drive, `net` for the adapter, `serial` for a serial device, the
 *     kernel shell for `lsusb`.
 *
 * HID needs none of this once a device is set up: the controller polls
 * keyboards and mice itself and feeds the kernel's HID registers, with
 * no CPU in the path (usbh_hid.c). So the keyboard keeps working if
 * this process dies; only new devices stop being set up.
 *
 * One thread. A storage command runs to its end before the next poll
 * or request, as before; but now only this process waits for it, not
 * the whole system.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zusbproto.h"
#define ZUSBNET_STRUCTS_ONLY
#include "../../common/zusbnet.h"

#include "usbh.h"
#include "usbh_hw.h"
#include "usbh_msc.h"
#include "usbh_ecm.h"
#include "usbh_cdc.h"

// -- the console --
//
// printf and the rest are these (the Makefile renames them): format
// into a line, write it out. The stdio behind ZFMT's printf would
// cost 12KB of flash for what the stack prints.
int usb_printf(const char *fmt, ...) {
	char b[160];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n < 0) return n;
	if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;
	write(1, b, (size_t)n);
	return n;
}

int usb_puts(const char *s) {
	write(1, s, strlen(s));
	write(1, "\n", 1);
	return 1;
}

int usb_putchar(int c) {
	char ch = (char)c;
	write(1, &ch, 1);
	return c;
}

// -- clients, and telling them --

// Each kind of device has at most one client: whoever asked about it
// last (zusbproto.h, "Events"). pid + 1; 0 none.
static uint32_t msc_client, net_client, cdc_client;

static uint8_t notes;		// Z_USBH_NOTE_* since they were last sent

// Added to the drive generation clients see. Set from the clock at
// start, so that a restarted `usb` does not hand `fs` the same number
// for the same drive and leave a stale mount in place: fs remounts.
static uint32_t gen_base;

// From the class drivers, inside a poll (usbh.h). Sent after it.
void usbh_notify(int what) { notes |= (uint8_t)what; }

static void event(uint32_t client, uint32_t bits) {
	z_obj_t o;
	if (!client) return;
	o.type = Z_UINT32;
	o.val.uint32 = bits;
	// A full mailbox loses it, which is harmless: the client asks
	// again on its own schedule.
	z_msg_new_send(client - 1, Z_USB_EVENT, 0, o);
}

static bool gone(uint32_t pid) {
	uint32_t st = Z_PROC_STATE_UNKNOWN;
	z_proc_status(pid, &st, NULL);
	return st != Z_PROC_STATE_RUNNING;
}

static uint32_t now(void) { return usbh_ticks(); }

// -- the transaction engine and the interrupt --
//
// Every transfer this process makes outside a poll -- a sector, a
// frame, a serial packet -- ends with XACT_DONE in IRQSTAT, which would
// raise IRQ 9 and wake this process for a poll with nothing to do. So
// the interrupt is off around them, and their XACT_DONE is cleared
// before it goes back on. A control transfer cannot be among them:
// those start only inside a poll, and z_usbh_bus_reserve() has run any
// under way to its end before the engine is ours.
//
// IRQEN reading 0 outside these is how a wake by IRQ 9 is told from a
// timeout: the kernel cleared it when the line fired (or when it poked
// this process about the keyboard LEDs, sw/os/hid.c). A quiet section
// remembers that before it clears IRQEN itself.
static bool want_poll;
static void quiet_begin(void) {
	if (!z_usbh_rd(Z_USBH_IRQEN)) want_poll = true;
	z_usbh_irq_enable(0);
}
static void quiet_end(void) {
	z_usbh_wr(Z_USBH_IRQSTAT, Z_USBH_IRQ_XACT_DONE);
	z_usbh_irq_enable(1);
}

// -- the ethernet adapter (zusbnet.h) --

static z_usbnet_srv_t srv;
static z_blob_t srv_hdr;
static const volatile z_usbnet_cli_t *net_cli;
static uint8_t net_mac[6];
static uint32_t net_next;	// when to ask the adapter again
static uint32_t net_active;	// last frame to or from us

#define NET_BUSY_HOLD	(Z_TICK_HZ / 2)	// "recently active"
#define NET_GAP_IDLE	16		// ~22 ms: an idle link answers a ping in that
// After sending a frame in a conversation, the answer is usually a
// fraction of a millisecond away -- a TFTP block after our ACK, a TCP
// segment after ours. So the adapter is asked again at once, back to
// back, for up to this long, instead of at the next tick (1.4-2.7ms
// later, which was most of a TFTP round trip). A NAK comes back in
// microseconds; this process stays preemptible meanwhile.
#define NET_BURST_US	2000
static uint32_t net_burst_end;	// cycles; 0 when not asking back to back
static uint32_t net_burst_from;	// cycles when the send that started it ended

static void net_info(void) { z_usbh_ecm_info(&srv.info); }

// What the ethernet path is doing, for `usbstat` (and `lsusb`): how
// often the adapter is asked and has nothing, and how long a frame
// waits on each side of the rings -- in usb's ring until net takes it,
// and in net's ring after its kick until usb sends it. In microseconds,
// from the cycle counter. For finding where the time goes in a slow
// transfer, without a debug build.
static struct {
	uint32_t polls, empty, rx, tx, tx_fail, full, events, kicks;
	uint32_t burst, burst_rx;	// back-to-back asks after a send, and the frames they found
	uint32_t reply_n, reply_sum, reply_max;	// send done -> the answer in the ring, us
	uint32_t ip_short, ip_long;	// IPv4 frames whose length disagrees with the IP header's
	uint32_t rx_wait_n, rx_wait_sum, rx_wait_max;	// usb's ring -> net
	uint32_t tx_wait_n, tx_wait_sum, tx_wait_max;	// net's kick -> sent
} ns;
static uint32_t rx_put[Z_USBNET_RX_SLOTS];	// cycle count when each slot was filled
static uint32_t rx_seen;			// net's rx_tail as last accounted
static uint32_t kick_at;			// cycle count of the last kick, 0 none

#ifdef USB_HOST_TEST
uint32_t usb_test_cycles(void);
static uint32_t cycles(void) { return usb_test_cycles(); }
#else
static uint32_t cycles(void) {
	uint32_t c;
	__asm__ volatile ("rdcycle %0" : "=r"(c));
	return c;
}
#endif

static void wait_note(uint32_t c0, uint32_t *n, uint32_t *sum, uint32_t *max) {
	uint32_t us = (cycles() - c0) / (Z_SYSCLK_HZ / 1000000u);
	(*n)++;
	*sum += us;
	if (us > *max) *max = us;
}

// Frames net has taken since the last look: how long each waited.
static void net_rx_seen(void) {
	uint32_t tail = net_cli->rx_tail;
	while (rx_seen != tail && rx_seen != srv.rx_head) {
		wait_note(rx_put[rx_seen % Z_USBNET_RX_SLOTS], &ns.rx_wait_n,
			&ns.rx_wait_sum, &ns.rx_wait_max);
		rx_seen++;
	}
}

static void net_stats(bool reset) {
	printf("usb: ethernet: asked %lu (empty %lu), in %lu, out %lu (failed %lu), "
		"ring full %lu, wakes to net %lu, kicks %lu\n",
		(unsigned long)ns.polls, (unsigned long)ns.empty, (unsigned long)ns.rx,
		(unsigned long)ns.tx, (unsigned long)ns.tx_fail, (unsigned long)ns.full,
		(unsigned long)ns.events, (unsigned long)ns.kicks);
	printf("usb: ethernet: IPv4 frames shorter than their header says %lu, longer %lu; "
		"asked back to back after %lu sends, found %lu frames that way\n",
		(unsigned long)ns.ip_short, (unsigned long)ns.ip_long,
		(unsigned long)ns.burst, (unsigned long)ns.burst_rx);
	{
		uint32_t t[8];
		uint32_t us = Z_SYSCLK_HZ / 1000000u;
		z_usbh_ecm_timing(t, reset);
		printf("usb: ethernet: reading a frame took avg %lu us (max %lu), %lu NAKs mid-frame; "
			"sending avg %lu us (max %lu), %lu NAKs\n",
			(unsigned long)(t[0] ? t[1] / t[0] / us : 0), (unsigned long)(t[2] / us),
			(unsigned long)t[3],
			(unsigned long)(t[4] ? t[5] / t[4] / us : 0), (unsigned long)(t[6] / us),
			(unsigned long)t[7]);
		printf("usb: ethernet: after a send, the answer was in the ring after avg %lu us "
			"(max %lu), its reading included\n",
			(unsigned long)(ns.reply_n ? ns.reply_sum / ns.reply_n : 0),
			(unsigned long)ns.reply_max);
	}
	printf("usb: ethernet: a frame waited for net avg %lu us, max %lu; "
		"a kick waited for usb avg %lu us, max %lu\n",
		(unsigned long)(ns.rx_wait_n ? ns.rx_wait_sum / ns.rx_wait_n : 0),
		(unsigned long)ns.rx_wait_max,
		(unsigned long)(ns.tx_wait_n ? ns.tx_wait_sum / ns.tx_wait_n : 0),
		(unsigned long)ns.tx_wait_max);
	if (reset) memset(&ns, 0, sizeof(ns));
}

static bool net_open(void) { return net_cli && srv.info.present; }

// Frames from net's ring to the adapter.
static void net_tx(void) {
	while (net_open() && srv.tx_tail != net_cli->tx_head) {
		uint32_t i = srv.tx_tail % Z_USBNET_TX_SLOTS;
		uint32_t len = net_cli->tx_len[i];
		const uint8_t *f = (const uint8_t *)net_cli->tx[i];
		if (kick_at) {
			wait_note(kick_at, &ns.tx_wait_n, &ns.tx_wait_sum, &ns.tx_wait_max);
			kick_at = 0;
		}
		if (len >= 14 && len <= Z_USBNET_FRAME && z_usbh_ecm_send(f, (int)len) == 0) {
			ns.tx++;
			// Sending says a conversation is going on -- except
			// ARP: answering "who has us?" every few hundred ms held
			// the polling busy on an idle desktop (round 7e).
			if (!(f[12] == 0x08 && f[13] == 0x06)) {
				net_active = now();
				net_burst_from = cycles();
				net_burst_end = (net_burst_from + NET_BURST_US * (Z_SYSCLK_HZ / 1000000u)) | 1;
				net_next = now();
				ns.burst++;
			}
		} else {
			ns.tx_fail++;
		}
		srv.tx_tail++;
	}
}

// Frames from the adapter into our ring, while there is room.
static void net_rx(void) {
	int k;
	if (!net_open()) return;
	net_rx_seen();
	if ((int32_t)(now() - net_next) < 0) return;
	for (k = 0; k < Z_USBNET_RX_SLOTS; ) {
		uint32_t head = srv.rx_head, tail = net_cli->rx_tail;
		if (head - tail >= Z_USBNET_RX_SLOTS) { ns.full++; break; }	// it waits in the adapter
		uint32_t i = head % Z_USBNET_RX_SLOTS;
		ns.polls++;
		uint8_t *f = srv.rx[i] + Z_USBNET_RX_OFF;
		int n = z_usbh_ecm_recv(f, Z_USBNET_FRAME - Z_USBNET_RX_OFF);
		if (n <= 0) {
			ns.empty++;
			// Asking back to back after a send (NET_BURST_US).
			if (net_burst_end && (int32_t)(cycles() - net_burst_end) < 0) continue;
			net_burst_end = 0;
			break;
		}
		k++;
		if (net_burst_end) {
			ns.burst_rx++;
			net_burst_end = 0;
			wait_note(net_burst_from, &ns.reply_n, &ns.reply_sum, &ns.reply_max);
		}
		ns.rx++;
		// Does the frame hold what its IP header says? A frame short
		// of it lost a packet on the way in (net drops it: a TFTP
		// retry); one longer than it plus padding took a packet too
		// many. Neither is counted by the class driver as an error.
		if (n >= 34 && f[12] == 0x08 && f[13] == 0x00) {
			int want = 14 + ((f[16] << 8) | f[17]);
			if (want > n) ns.ip_short++;
			else if (n > 60 && n > want) ns.ip_long++;
		}
		srv.rx_len[i] = (uint16_t)n;
		rx_put[i] = cycles();
		srv.rx_at[i] = rx_put[i];
		srv.rx_head = head + 1;
		// Only a frame addressed to us says a conversation is going
		// on; broadcasts arrive every few hundred ms on any LAN.
		if (n >= 6 && memcmp(f, net_mac, 6) == 0) net_active = now();
		// Wake net if this is the only frame it has not taken -- by its
		// tail NOW, not the one read before the receive. The receive
		// spins on the bus for a millisecond or so and can be
		// preempted; net may have taken the previous frame meanwhile
		// and gone to sleep with the ring empty. Judged by the old
		// tail, the ring was not empty, nobody was woken, and the
		// frame waited for net's 100ms backstop or its TFTP retry
		// timer (the 0.5s "frame waited for net" in usbstat, round 10).
		if (head == net_cli->rx_tail) { event(net_client, Z_USBEV_NET); ns.events++; }
	}
	net_next = now() + ((now() - net_active) < NET_BUSY_HOLD ? 1 : NET_GAP_IDLE);
}

// -- the serial device (zusbcdc.h) --
//
// Read for the client while it is reading, and kept here, so that its
// read is answered from memory.

#define CDC_BUF		512
#define CDC_HOLD	(Z_TICK_HZ * 2)	// read this long after the client last asked

static uint8_t cdc_buf[CDC_BUF];
static uint32_t cdc_len;
static uint32_t cdc_until;

static bool cdc_reading(void) {
	return cdc_client && z_usbh_cdc_present() && (int32_t)(cdc_until - now()) > 0;
}

static void cdc_fill(bool tell) {
	int k;
	bool was_empty = cdc_len == 0;
	for (k = 0; k < 4 && CDC_BUF - cdc_len >= 64; k++) {
		int n = z_usbh_cdc_read(cdc_buf + cdc_len, 64);
		if (n <= 0) break;
		cdc_len += (uint32_t)n;
	}
	if (tell && was_empty && cdc_len) event(cdc_client, Z_USBEV_CDC);
}

// -- commands (Z_USBOP_CMD: the kernel shell's lsusb and the rest) --

static int32_t run_cmd(const char *line) {
	char w[16];
	const char *p = line;
	int n = 0;
	while (*p == ' ') p++;
	while (*p && *p != ' ' && n < (int)sizeof(w) - 1) w[n++] = *p++;
	w[n] = 0;
	while (*p == ' ') p++;

	if (!strcmp(w, "lsusb")) { z_usbh_dump(); if (net_cli) net_stats(false); }
	else if (!strcmp(w, "usbstat")) net_stats(true);
	else if (!strcmp(w, "usbrescan")) z_usbh_rescan();
#ifdef USBH_DEBUG	// bring-up tools, docs/usb_host.md "Debug build"
	else if (!strcmp(w, "usbcap")) z_usbh_cap_start(0);
	else if (!strcmp(w, "usbcapok")) z_usbh_cap_start(1);
	else if (!strcmp(w, "usbcapd")) z_usbh_cap_dump();
	else if (!strcmp(w, "usbidle")) z_usbh_probe_idle();
	else if (!strcmp(w, "usbbuf")) z_usbh_buftest();
	else if (!strcmp(w, "usbnak")) z_usbh_set_ls_hub_nak(*p ? atoi(p) : 0);
	else if (!strcmp(w, "usbtune")) {
		int v[4] = { 0, 0, 0, 0 }, k = 0;
		while (*p && k < 4) {
			v[k++] = atoi(p);
			while (*p && *p != ' ') p++;
			while (*p == ' ') p++;
		}
		if (k == 0) z_usbh_tune_show();
		else if (k == 4) z_usbh_tune_set(v[0], v[1], v[2], v[3]);
		else printf("usage: usbtune [timeout_us turn_us gap_us pre_bits]\n");
	}
#endif
	else return -Z_ENOENT;
	return 0;
}

// -- requests --

// A reply is borrowed until its client sends again, so each client
// gets its own; the least recently used is reused, as in `fs`. Sector
// data has one buffer of its own: `fs` is the only reader of sectors.
#define SLOTS	4

typedef struct {
	uint32_t	pid;		// + 1; 0 free
	uint32_t	last;
	z_blob_t	hdr;
	z_usbrep_t	h;
	uint8_t		data[Z_USB_CDC_MAX];
} slot_t;

static slot_t slots[SLOTS];
static uint32_t clock_n;

static struct {
	z_usbrep_t	h;
	uint8_t		data[Z_USB_MSC_MAX * Z_USB_SECTOR];
} msc_rep;
static z_blob_t msc_hdr;

static slot_t *slot_of(uint32_t pid) {
	slot_t *s = NULL, *old = &slots[0];
	int i;
	for (i = 0; i < SLOTS; i++) {
		if (slots[i].pid == pid + 1) { s = &slots[i]; break; }
		if (slots[i].last < old->last) old = &slots[i];
	}
	if (!s) { s = old; s->pid = pid + 1; }
	s->last = ++clock_n;
	return s;
}

static void serve(z_msg_t *m) {
	z_obj_t *o0 = z_list_get(&m->obj, 0), *o1 = z_list_get(&m->obj, 1);
	const z_usbreq_t *rq = o0 ? (const z_usbreq_t *)z_blob_data(o0) : NULL;
	const uint8_t *data = o1 ? (const uint8_t *)z_blob_data(o1) : NULL;
	uint32_t dlen = o1 ? z_blob_len(o1) : 0;
	slot_t *s = slot_of(m->from);
	z_blob_t *hdr = &s->hdr;
	z_usbrep_t *h = &s->h;
	int32_t r = -Z_EINVAL;
	z_obj_t reply;

	h->len = 0;
	if (!data) dlen = 0;
	if (rq && z_blob_len(o0) >= sizeof(*rq)) switch (rq->op) {

	case Z_USBOP_MSC_INFO: {
		z_usb_msc_info_t mi;
		msc_client = m->from + 1;
		memset(&mi, 0, sizeof(mi));
		mi.present = (uint8_t)z_usbh_msc_present();
		mi.ready = (uint8_t)z_usbh_msc_ready();
		mi.sectors = mi.ready ? z_usbh_msc_sectors() : 0;
		mi.gen = gen_base + z_usbh_msc_generation();
		memcpy(s->data, &mi, sizeof(mi));
		h->len = sizeof(mi);
		r = 0;
		break;
	}
	case Z_USBOP_MSC_START:
		r = -Z_ENODEV;
		if (!z_usbh_msc_present()) break;
		if (z_usbh_msc_ready()) { r = 0; break; }
		quiet_begin();
		r = z_usbh_msc_start() == Z_USBH_MSC_OK ? 0 : -Z_EIO;
		quiet_end();
		break;
	case Z_USBOP_MSC_READ:
		if (!rq->count || rq->count > Z_USB_MSC_MAX) break;
		r = -Z_ENODEV;
		if (!z_usbh_msc_ready()) break;
		quiet_begin();
		r = z_usbh_msc_read(rq->lba, msc_rep.data, rq->count) ==
			Z_USBH_MSC_OK ? 0 : -Z_EIO;
		quiet_end();
		if (r == 0) {
			h = &msc_rep.h;
			hdr = &msc_hdr;
			h->len = rq->count * Z_USB_SECTOR;
		}
		break;
	case Z_USBOP_MSC_WRITE:
		if (!rq->count || rq->count > Z_USB_MSC_MAX ||
			dlen < rq->count * Z_USB_SECTOR) break;
		r = -Z_ENODEV;
		if (!z_usbh_msc_ready()) break;
		quiet_begin();
		r = z_usbh_msc_write(rq->lba, data, rq->count) ==
			Z_USBH_MSC_OK ? 0 : -Z_EIO;
		quiet_end();
		break;

	case Z_USBOP_NET_INFO:
		net_info();
		memcpy(s->data, &srv.info, sizeof(srv.info));
		h->len = sizeof(srv.info);
		r = 0;
		break;
	case Z_USBOP_NET_OPEN:
		if (dlen < sizeof(z_usbnet_cli_t) || ((uintptr_t)data & 3u)) break;
		net_cli = (const volatile z_usbnet_cli_t *)data;
		net_client = m->from + 1;
		memcpy(net_mac, rq->mac, 6);
		z_usbh_ecm_want(rq->mac);
		// Both rings start empty, from wherever net's indices are.
		srv.rx_head = net_cli->rx_tail;
		srv.tx_tail = net_cli->tx_head;
		rx_seen = srv.rx_head;
		net_info();
		net_next = now();
		net_active = now();
		// The reply is the ring itself (zusbnet.h).
		srv.result = 0;
		srv.len = sizeof(srv) - sizeof(z_usbrep_t);
		srv_hdr.data = (uint8_t *)&srv;
		srv_hdr.len = sizeof(srv);
		hdr = &srv_hdr;
		h = NULL;
		r = 0;
		break;
	case Z_USBOP_NET_CLOSE:
		if (net_client == m->from + 1) { net_cli = NULL; net_client = 0; }
		r = 0;
		break;

	case Z_USBOP_CDC_INFO:
		cdc_client = m->from + 1;
		r = z_usbh_cdc_present() ? (1 | (z_usbh_cdc_kind() << 1)) : 0;
		break;
	case Z_USBOP_CDC_READ: {
		uint32_t n;
		cdc_client = m->from + 1;
		cdc_until = now() + CDC_HOLD;
		r = -Z_ENODEV;
		if (!z_usbh_cdc_present()) { cdc_len = 0; break; }
		if (!cdc_len) {		// nothing kept: ask the device now
			quiet_begin();
			cdc_fill(false);	// for this very reader: no event
			quiet_end();
		}
		n = rq->len < cdc_len ? rq->len : cdc_len;
		if (n > Z_USB_CDC_MAX) n = Z_USB_CDC_MAX;
		memcpy(s->data, cdc_buf, n);
		memmove(cdc_buf, cdc_buf + n, cdc_len - n);
		cdc_len -= n;
		h->len = n;
		r = (int32_t)n;
		break;
	}
	case Z_USBOP_CDC_WRITE:
		if (!dlen) { r = 0; break; }
		quiet_begin();
		r = z_usbh_cdc_write(data, (int)dlen);
		quiet_end();
		if (r < 0) r = -Z_EIO;
		break;

	case Z_USBOP_CMD:
		if (!dlen || !memchr(data, 0, dlen)) break;
		quiet_begin();
		r = run_cmd((const char *)data);
		quiet_end();
		break;
	}

	if (h) {
		h->result = r;
		hdr->data = (uint8_t *)h;
		hdr->len = (uint32_t)sizeof(*h) + h->len;
	}
	reply.type = Z_BLOB;
	reply.val.ptr = hdr;
	z_msg_new_send(m->from, Z_USB_REPLY, m->tag, reply);
}

#ifndef USB_HOST_TEST
// Between the steps of `usbmount`'s bring-up (z_usbh_msc_start(), run
// inside a request): asleep rather than asking the drive back to back,
// which is what used to make a slow stick fail it. The wait ends early
// when mail comes, so it is a loop on the clock; with mail waiting it
// turns without sleeping, still pre-emptible, for at most the
// driver's ~15 s.
void usbh_msc_pause(uint32_t ticks) {
	uint32_t until = now() + ticks;
	int32_t left;
	while ((left = (int32_t)(until - now())) > 0) z_proc_wait((uint32_t)left);
}

// -- the keyboards' lock LEDs --
//
// The kernel keeps Num/Caps/Scroll Lock (sw/os/hid.c) and wakes this
// process when one changes; this asks which are on, and the stack sends
// them to every keyboard.
static int32_t kbd_locks(void) {
	z_obj_t obj;
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	obj.type = Z_INT32;
	obj.val.int32 = -2;		// sw/os/hid.c, k_kbd_layout(): the locks
	k(Z_SYS_KBD_LAYOUT, (uint32_t *)&obj, 0);
	return obj.val.int32;
}

// -- the loop --

static uint32_t min_wait(uint32_t a, uint32_t b) {
	if (!a) return b;
	if (!b) return a;
	return a < b ? a : b;
}
#endif

// -- a new drive, brought up in steps --
//
// A drive that has just bound may take seconds to report ready (a card
// reader with no card, a disk spinning up). Asked one TEST UNIT READY
// at a time here, further apart each time it is not ready (8 ticks,
// then 16, 32 ... at most 256: z_usbh_msc_start_pace()), it holds up
// nobody; `fs` hears about the drive once it is ready, and mounts it
// then. The driver gives up after ~15 s, however often it was asked
// (usbh_msc.c, "bringing a new drive up"). Until Phase 3 polish, `fs`
// asked for the whole wait itself (MSC_START), and answered nobody for
// its length.
//
// A drive that never became ready gets one more bring-up, 5 s later
// (MSC_AGAIN_TICKS): some sticks need a pause, and with nothing more
// the drive stayed unmounted until someone typed `usbmount`. Here
// rather than in `fs`: fs's only way to ask is MSC_START, which would
// hold up every one of its clients for the whole bring-up.
#define MSC_AGAIN_TICKS	(5u * 732u)

static bool msc_bring;		// a bound drive not yet ready
static uint32_t msc_due;
static uint8_t msc_again;	// bring-ups left after this one

static void msc_service(void) {
	if (!msc_bring || (int32_t)(now() - msc_due) < 0) return;
	quiet_begin();
	int r = z_usbh_msc_start_step();
	quiet_end();
	if (r == Z_USBH_MSC_BUSY) { msc_due = now() + z_usbh_msc_start_pace(); return; }
	if (r != Z_USBH_MSC_OK && msc_again && z_usbh_msc_present()) {
		msc_again--;
		msc_due = now() + MSC_AGAIN_TICKS;
		return;
	}
	msc_bring = false;
	// Ready, or given up on (`usbmount` tries again in full): either
	// way the drive's client looks now.
	event(msc_client, Z_USBEV_MSC);
}

// The data paths between polls: the adapter both ways, the serial
// device for a reading client.
static void service(void) {
	if (net_open() || cdc_reading()) {
		quiet_begin();
		net_tx();
		net_rx();
		if (cdc_reading() && cdc_len < CDC_BUF) cdc_fill(true);
		quiet_end();
	}
}

// What the class drivers noted during a poll, to their clients.
static void send_notes(void) {
	uint8_t n = notes;
	notes = 0;
	if (n & Z_USBH_NOTE_MSC) {
		if (z_usbh_msc_present() && !z_usbh_msc_ready()) {
			msc_bring = true;		// fs hears once it is ready
			msc_due = now();
			msc_again = 1;
		} else {
			msc_bring = false;
			event(msc_client, Z_USBEV_MSC);
		}
	}
	if (n & Z_USBH_NOTE_NET) { net_info(); event(net_client, Z_USBEV_NET); }
	if (n & Z_USBH_NOTE_CDC) { cdc_len = 0; event(cdc_client, Z_USBEV_CDC); }
}

// Clients that died are forgotten: net's ring above all, which is
// memory this process would otherwise go on reading.
static void sweep(void) {
	if (net_open()) net_info();
	if (net_client && gone(net_client - 1)) { net_cli = NULL; net_client = 0; }
	if (cdc_client && gone(cdc_client - 1)) { cdc_client = 0; cdc_len = 0; }
	if (msc_client && gone(msc_client - 1)) msc_client = 0;
}

#ifndef USB_HOST_TEST
int main(void) {
	char name[16];
	uint32_t last_poll, last_sweep;
	int32_t leds = -1;

	z_usbh_init();
	if (!z_usbh_present()) {
		printf("usb: no USB host controller, nothing to do\n");
		return 0;
	}
	if (!z_pid_register("usb", name, sizeof name)) {
		printf("usb: cannot register\n");
		return 1;
	}
	// The kernel clears IRQEN when the line fires, so that a level
	// cannot hold the CPU in its handler until this process runs.
	if (z_irq_bind(Z_IRQ_USB, Z_USBH_IRQEN, 0) != Z_OK)
		printf("usb: cannot take IRQ 9; polling only\n");
	printf("usb: %s ready\n", name);

	gen_base = cycles() << 8;

	// Drives are `fs`'s until someone else asks. At boot it asks
	// soon anyway; after a restart of this process (kernel.c,
	// "restarting a driver") it has no reason to, and a drive still
	// plugged in would never be mounted again.
	{
		uint32_t fs;
		if (z_pid_lookup(Z_FS_SERVICE, &fs)) msc_client = fs + 1;
	}

	last_poll = last_sweep = now();
	for (;;) {
		z_msg_t m;
		uint32_t t, wt = z_usbh_wait_ticks();

		// How long to sleep: the soonest of the stack's own next
		// timed step, the adapter's next ask, a reading serial
		// client, the once-a-second look for clients that died.
		t = wt;
		if (net_open())
			t = min_wait(t, (int32_t)(net_next - now()) > 0 ? net_next - now() : 1);
		if (cdc_reading()) t = min_wait(t, 1);
		if (msc_bring)
			t = min_wait(t, (int32_t)(msc_due - now()) > 0 ? msc_due - now() : 1);
		if (net_client || cdc_client || msc_client) t = min_wait(t, Z_TICK_HZ);

		// A request, or else an interrupt, a poke or the timeout.
		// Z_USB_KICK needs nothing more than the wake: net_tx() below.
		if (z_msg_recv(&m, t) == Z_OK) {
			if (m.subject == Z_USB_REQ) serve(&m);
			else if (m.subject == Z_USB_KICK) {
				ns.kicks++;
				if (!kick_at) kick_at = cycles() | 1;
			}
		}

		// A poll only when the stack wants one: IRQ 9 fired, or a
		// timed step is due. Not on every wake -- the adapter's
		// receive timer wakes this every tick while a conversation is
		// going on, and a poll steps every device.
		if (want_poll || !z_usbh_rd(Z_USBH_IRQEN) ||
			(wt && now() - last_poll >= wt)) {
			want_poll = false;
			z_usbh_poll();
			last_poll = now();
			int32_t l = kbd_locks();
			if (l >= 0 && l != leds) { leds = l; z_usbh_kbd_leds((uint8_t)l); }
			z_usbh_irq_enable(1);
		}

		service();
		if (notes) send_notes();
		msc_service();
		if (now() - last_sweep >= Z_TICK_HZ) {
			last_sweep = now();
			sweep();
		}
	}
}
#else
// For tests/test_usb.c: the server without its loop or its hardware.
void usb_test_serve(z_msg_t *m) { serve(m); }
void usb_test_service(void) { service(); if (notes) send_notes(); msc_service(); }
void usb_test_sweep(void) { sweep(); }
#endif
