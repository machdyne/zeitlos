/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zusbc.h, and zusbproto.h for the wire format.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "zeitlos.h"
#include "zsoc.h"		// Z_TICK_HZ
#include "zusbc.h"
#ifndef ZUSBC_KERNEL
#include "zusbnet.h"
#include "zusbcdc.h"
#endif

// Ticks to wait for a reply. Storage waits longest: a drive's start
// asks it up to twenty times whether it is ready, and a flash write
// can NAK for a long while. A command prints, at 1 Mbaud.
#define ZUSB_TIMEOUT		(Z_TICK_HZ * 5)
#define ZUSB_MSC_TIMEOUT	(Z_TICK_HZ * 30)

static uint32_t usb_pid;
static bool usb_known;
static uint32_t seq;

static bool find_usb(bool again) {
	if (usb_known && !again) return true;
	usb_known = z_pid_lookup(Z_USB_SERVICE, &usb_pid);
	return usb_known;
}

// Replies that may still arrive after their caller gave up on them (a
// timeout). z_msg_recv_match() leaves everything else queued, so such a
// reply would sit in the mailbox for good, and enough of them fill it.
// Each is looked for, without waiting, at the start of the next calls,
// and forgotten once found or after STALE_LOOKS tries.
#define STALE_MAX	4
#define STALE_LOOKS	32
static uint32_t stale_tag[STALE_MAX];		// 0: none
static uint8_t stale_looks[STALE_MAX];
static uint8_t stale_n;

static void stale_note(uint32_t tag) {
	int i, k = 0;
	for (i = 0; i < STALE_MAX; i++) {
		if (!stale_tag[i]) { k = i; break; }
		if (stale_looks[i] > stale_looks[k]) k = i;
	}
	if (!stale_tag[k]) stale_n++;
	stale_tag[k] = tag;
	stale_looks[k] = 0;
}

static void stale_drain(void) {
	z_msg_t m;
	int i;
	for (i = 0; i < STALE_MAX; i++) {
		if (!stale_tag[i]) continue;
		if (z_msg_take_match(&m, Z_USB_REPLY, stale_tag[i]) == Z_OK ||
			++stale_looks[i] >= STALE_LOOKS) {
			stale_tag[i] = 0;
			stale_n--;
		}
	}
}

// One request and its reply, which is left borrowed in *rep: `usb`'s
// memory, valid until this process sends it something else.
static int32_t call_raw(z_usbreq_t *rq, const void *data, uint32_t dlen,
	uint32_t timeout, const z_usbrep_t **rep) {

	z_blob_t b[2];
	z_obj_t items[2];
	z_obj_table_t t;
	z_obj_t o;
	z_msg_t m;
	uint32_t tag;

	*rep = NULL;
	if (!find_usb(false)) return -Z_ENODEV;

	b[0].len = sizeof(*rq);
	b[0].data = (uint8_t *)rq;
	b[1].len = data ? dlen : 0;
	b[1].data = (uint8_t *)data;
	items[0].type = Z_BLOB; items[0].val.ptr = &b[0];
	items[1].type = Z_BLOB; items[1].val.ptr = &b[1];
	t.len = 2; t.a = items; t.b = NULL;
	o.type = Z_LIST; o.val.ptr = &t;

	if (stale_n) stale_drain();
	tag = ++seq;
	if (!tag) tag = ++seq;
	if (z_msg_new_send(usb_pid, Z_USB_REQ, tag, o) != Z_OK) {
		// A cached pid that has gone stale: `usb` restarted, say.
		if (!find_usb(true) ||
			z_msg_new_send(usb_pid, Z_USB_REQ, tag, o) != Z_OK)
			return -Z_ENODEV;
	}
	if (z_msg_recv_match(&m, Z_USB_REPLY, tag, timeout) != Z_OK) {
		stale_note(tag);
		usb_known = false;		// look it up again: it may have restarted
		return -Z_ETIMEDOUT;
	}
	if (m.from != usb_pid) return -Z_EIO;

	const z_usbrep_t *r = (const z_usbrep_t *)z_blob_data(&m.obj);
	uint32_t rl = z_blob_len(&m.obj);
	if (!r || rl < sizeof(*r) || ((uintptr_t)r & 3u) ||
		r->len > rl - sizeof(*r))
		return -Z_EIO;
	*rep = r;
	return r->result;
}

// The same, with any data the reply carries copied to `out`.
static int32_t call(z_usbreq_t *rq, const void *data, uint32_t dlen,
	void *out, uint32_t outcap, uint32_t *outlen, uint32_t timeout) {
	const z_usbrep_t *rep;
	int32_t r = call_raw(rq, data, dlen, timeout, &rep);
	uint32_t n = 0;
	if (rep && out) {
		n = rep->len < outcap ? rep->len : outcap;
		if (n) memcpy(out, rep + 1, n);
	}
	if (outlen) *outlen = n;
	return r;
}

static void req_init(z_usbreq_t *rq, uint32_t op) {
	memset(rq, 0, sizeof(*rq));
	rq->op = op;
}

int32_t zusb_cmd(const char *line) {
	z_usbreq_t rq;
	req_init(&rq, Z_USBOP_CMD);
	return call(&rq, line, (uint32_t)strlen(line) + 1, NULL, 0, NULL,
		ZUSB_TIMEOUT);
}

#ifndef ZUSBC_KERNEL

bool zusb_msc_info(z_usb_msc_info_t *mi) {
	z_usbreq_t rq;
	uint32_t got = 0;
	req_init(&rq, Z_USBOP_MSC_INFO);
	memset(mi, 0, sizeof(*mi));
	return call(&rq, NULL, 0, mi, sizeof(*mi), &got, ZUSB_TIMEOUT) == 0 &&
		got == sizeof(*mi);
}

int32_t zusb_msc_start(void) {
	z_usbreq_t rq;
	req_init(&rq, Z_USBOP_MSC_START);
	return call(&rq, NULL, 0, NULL, 0, NULL, ZUSB_MSC_TIMEOUT);
}

int32_t zusb_msc_read(uint32_t lba, uint32_t count, void *buf) {
	z_usbreq_t rq;
	uint8_t *p = (uint8_t *)buf;
	while (count) {
		uint32_t n = count < Z_USB_MSC_MAX ? count : Z_USB_MSC_MAX, got = 0;
		req_init(&rq, Z_USBOP_MSC_READ);
		rq.lba = lba;
		rq.count = n;
		int32_t r = call(&rq, NULL, 0, p, n * Z_USB_SECTOR, &got,
			ZUSB_MSC_TIMEOUT);
		if (r < 0) return r;
		if (got != n * Z_USB_SECTOR) return -Z_EIO;
		lba += n;
		count -= n;
		p += n * Z_USB_SECTOR;
	}
	return 0;
}

int32_t zusb_msc_write(uint32_t lba, uint32_t count, const void *buf) {
	z_usbreq_t rq;
	const uint8_t *p = (const uint8_t *)buf;
	while (count) {
		uint32_t n = count < Z_USB_MSC_MAX ? count : Z_USB_MSC_MAX;
		req_init(&rq, Z_USBOP_MSC_WRITE);
		rq.lba = lba;
		rq.count = n;
		int32_t r = call(&rq, p, n * Z_USB_SECTOR, NULL, 0, NULL,
			ZUSB_MSC_TIMEOUT);
		if (r < 0) return r;
		lba += n;
		count -= n;
		p += n * Z_USB_SECTOR;
	}
	return 0;
}

// -- ethernet (zusbnet.h) --

static uint32_t net_usb_pid;

bool z_usbnet_info(z_usbnet_info_t *info) {
	z_usbreq_t rq;
	uint32_t got = 0;
	req_init(&rq, Z_USBOP_NET_INFO);
	memset(info, 0, sizeof(*info));
	return call(&rq, NULL, 0, info, sizeof(*info), &got, ZUSB_TIMEOUT) == 0 &&
		got == sizeof(*info);
}

const volatile z_usbnet_srv_t *z_usbnet_open(const uint8_t mac[6],
	z_usbnet_cli_t *cli) {
	z_usbreq_t rq;
	const z_usbrep_t *rep;
	req_init(&rq, Z_USBOP_NET_OPEN);
	memcpy(rq.mac, mac, 6);
	if (call_raw(&rq, cli, sizeof(*cli), ZUSB_TIMEOUT, &rep) != 0 || !rep ||
		rep->len != sizeof(z_usbnet_srv_t) - sizeof(z_usbrep_t))
		return NULL;
	net_usb_pid = usb_pid;
	// Not borrowed: `usb`'s ring itself, there for as long as it runs.
	return (const volatile z_usbnet_srv_t *)rep;
}

void z_usbnet_kick(void) {
	z_obj_t o;
	o.type = Z_NONE;
	o.val.uint32 = 0;
	if (net_usb_pid) z_msg_new_send(net_usb_pid, Z_USB_KICK, 0, o);
}

bool z_usbnet_alive(void) {
	uint32_t pid;
	return net_usb_pid && z_pid_lookup(Z_USB_SERVICE, &pid) &&
		pid == net_usb_pid;
}

// -- serial (zusbcdc.h) --

bool z_usbcdc_present(void) {
	z_usbreq_t rq;
	req_init(&rq, Z_USBOP_CDC_INFO);
	int32_t r = call(&rq, NULL, 0, NULL, 0, NULL, ZUSB_TIMEOUT);
	return r > 0 && (r & 1);
}

int32_t z_usbcdc_read(uint8_t *buf, uint32_t max) {
	z_usbreq_t rq;
	uint32_t got = 0;
	req_init(&rq, Z_USBOP_CDC_READ);
	rq.len = max < Z_USB_CDC_MAX ? max : Z_USB_CDC_MAX;
	int32_t r = call(&rq, NULL, 0, buf, rq.len, &got, ZUSB_TIMEOUT);
	return r < 0 ? -1 : (int32_t)got;
}

int32_t z_usbcdc_write(const uint8_t *buf, uint32_t len) {
	uint32_t done = 0;
	while (done < len) {
		z_usbreq_t rq;
		uint32_t n = len - done < Z_USB_CDC_MAX ? len - done : Z_USB_CDC_MAX;
		req_init(&rq, Z_USBOP_CDC_WRITE);
		int32_t r = call(&rq, buf + done, n, NULL, 0, NULL, ZUSB_TIMEOUT);
		if (r <= 0) return done ? (int32_t)done : -1;
		done += (uint32_t)r;
		if ((uint32_t)r < n) break;
	}
	return (int32_t)done;
}

#endif
