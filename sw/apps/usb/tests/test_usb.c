/*
 * Zeitlos -- host test for the `usb` process around the stack
 * (sw/apps/usb/usb.c) and its client (sw/common/zusbc.c).
 *
 * The stack itself is not here: rtl/tb's co-simulation runs it against
 * the real gateware (`make test_usb_cosim` and the others at the top
 * level). What is here is everything Phase 3 added around it: the
 * requests and their replies, the ethernet rings, the serial buffer,
 * the events, the slots that keep a borrowed reply from being
 * overwritten. The class drivers are fakes with a drive, an adapter
 * and a serial device behind them.
 *
 * Messages go straight from the client into usb_test_serve() and back:
 * one process each for `usb`, `fs`, `net` and `serial`, told apart by
 * `client`, the pid a send comes from.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zproc.h"
#include "zusbc.h"
#include "zusbnet.h"
#include "zusbcdc.h"
#include "../usbh.h"
#include "../usbh_msc.h"
#include "../usbh_hw.h"

void usb_test_serve(z_msg_t *m);
void usb_test_service(void);
void usb_test_sweep(void);

#define USB_PID		3
#define FS_PID		1
#define NET_PID		4
#define SERIAL_PID	7

static uint32_t client = FS_PID;	// who is sending
static bool alive[64];
static uint32_t ticks = 1000;

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// -- each client's mailbox: the one reply it waits for, and events --

typedef struct {
	bool have_reply;
	z_msg_t reply;
	uint32_t events;		// Z_USBEV_* bits received
	uint32_t n_events;
} box_t;
static box_t boxes[64];
static uint32_t kicks;

// -- the kernel, as far as usb.c and zusbc.c need it --

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	z_msg_t m;
	memset(&m, 0, sizeof m);
	m.to = to; m.subject = subject; m.tag = tag; m.obj = obj;
	if (to == USB_PID) {
		m.from = client;
		if (subject == Z_USB_KICK) { kicks++; return Z_OK; }
		CHECK(subject == Z_USB_REQ);
		uint32_t me = client;
		usb_test_serve(&m);
		client = me;
		return Z_OK;
	}
	CHECK(to < 64);
	if (!alive[to]) return Z_FAIL;
	m.from = USB_PID;
	if (subject == Z_USB_EVENT) {
		CHECK(obj.type == Z_UINT32);
		boxes[to].events |= obj.val.uint32;
		boxes[to].n_events++;
		return Z_OK;
	}
	CHECK(subject == Z_USB_REPLY);
	CHECK(!boxes[to].have_reply);		// one request in flight per client
	CHECK(obj.type == Z_BLOB && ((uintptr_t)((z_blob_t *)obj.val.ptr)->data & 3u) == 0);
	boxes[to].reply = m;
	boxes[to].have_reply = true;
	return Z_OK;
}

z_rv z_msg_recv_match(z_msg_t *msg, uint32_t subject, uint32_t tag, uint32_t t) {
	box_t *b = &boxes[client];
	(void)t;
	if (!b->have_reply || b->reply.subject != subject || b->reply.tag != tag)
		return Z_FAIL;
	*msg = b->reply;
	b->have_reply = false;
	return Z_OK;
}

z_rv z_msg_take_match(z_msg_t *msg, uint32_t subject, uint32_t tag) {
	return z_msg_recv_match(msg, subject, tag, 0);
}

static bool usb_up = true;
bool z_pid_lookup(const char *name, uint32_t *pid) {
	if (strcmp(name, Z_USB_SERVICE) || !usb_up) return false;
	*pid = USB_PID;
	return true;
}

z_rv z_proc_status(uint32_t pid, uint32_t *state, int32_t *status) {
	(void)status;
	if (state) *state = (pid < 64 && alive[pid]) ? Z_PROC_STATE_RUNNING : Z_PROC_STATE_EXITED;
	return Z_OK;
}

// -- the controller's registers: only IRQEN and IRQSTAT are touched --

static uint32_t irqen = 0x17, irqstat;
uint32_t z_usbh_rd(uint32_t a) {
	if (a == Z_USBH_IRQEN) return irqen;
	if (a == Z_USBH_IRQSTAT) return irqstat;
	return 0;
}
void z_usbh_wr(uint32_t a, uint32_t v) {
	if (a == Z_USBH_IRQEN) irqen = v;
	if (a == Z_USBH_IRQSTAT) irqstat &= ~v;
}
uint8_t z_usbh_rb(uint32_t a) { (void)a; return 0; }
void z_usbh_wb(uint32_t a, uint8_t v) { (void)a; (void)v; }

// -- the stack, faked --

uint32_t usbh_ticks(void) { return ticks; }
// 100us a look, so that a back-to-back burst (NET_BURST_US) ends
static uint32_t cyc_extra;
uint32_t usb_test_cycles(void) { return ticks * 65536u + (cyc_extra += 4800u); }
void z_usbh_init(void) {}
int z_usbh_present(void) { return 1; }
void z_usbh_poll(void) {}
uint32_t z_usbh_wait_ticks(void) { return 0; }
static bool irq_on = true;
void z_usbh_irq_enable(int on) { irq_on = on; irqen = on ? 0x17 : 0; }
void z_usbh_kbd_leds(uint8_t l) { (void)l; }
static int dumps, rescans;
void z_usbh_dump(void) { dumps++; }
void z_usbh_rescan(void) { rescans++; }

// A drive.
#define DRIVE_SECTORS 64
static uint8_t drive[DRIVE_SECTORS * 512];
static int msc_bound, msc_started;
static uint32_t msc_gen = 1, msc_reads, msc_max_count;
static bool msc_irq_was_on;
int z_usbh_msc_present(void) { return msc_bound; }
int z_usbh_msc_ready(void) { return msc_bound && msc_started; }
uint32_t z_usbh_msc_sectors(void) { return DRIVE_SECTORS; }
uint32_t z_usbh_msc_generation(void) { return msc_gen; }
int z_usbh_msc_start(void) { if (!msc_bound) return Z_USBH_MSC_ERR; msc_started = 1; return Z_USBH_MSC_OK; }
static int msc_busy_steps, msc_steps;		// a drive slow to be ready
int z_usbh_msc_start_step(void) {
	msc_steps++;
	if (!msc_bound) return Z_USBH_MSC_ERR;
	if (msc_busy_steps > 0) { msc_busy_steps--; return Z_USBH_MSC_BUSY; }
	msc_started = 1;
	return Z_USBH_MSC_OK;
}
int z_usbh_msc_read(uint32_t lba, uint8_t *dst, uint32_t count) {
	msc_reads++;
	if (irq_on) msc_irq_was_on = true;	// a transfer outside a poll is quiet
	if (count > msc_max_count) msc_max_count = count;
	if (lba + count > DRIVE_SECTORS) return Z_USBH_MSC_ERR;
	memcpy(dst, drive + lba * 512, count * 512);
	irqstat |= 1;				// XACT_DONE, as the engine leaves it
	return Z_USBH_MSC_OK;
}
int z_usbh_msc_write(uint32_t lba, const uint8_t *src, uint32_t count) {
	if (lba + count > DRIVE_SECTORS) return Z_USBH_MSC_ERR;
	memcpy(drive + lba * 512, src, count * 512);
	return Z_USBH_MSC_OK;
}

// An adapter: frames waiting on its receive side, and what was sent.
static uint8_t frames[16][Z_USBNET_FRAME];
static int frame_len[16], n_frames, next_frame, ecm_recvs;
static uint8_t sent[16][Z_USBNET_FRAME];
static int sent_len[16], n_sent;
static int ecm_present = 1;
static uint8_t want_mac[6];
void z_usbh_ecm_info(z_usbnet_info_t *in) {
	memset(in, 0, sizeof(*in));
	in->present = (uint8_t)ecm_present;
	in->mac_ok = 1;
	memcpy(in->mac, "\x02\x00\x00\x00\x00\x01", 6);
	in->gen = 7;
}
void z_usbh_ecm_timing(uint32_t out[8], int reset) { (void)reset; memset(out, 0, 8 * sizeof(uint32_t)); }
static int frame_after;		// no frame until ecm_recvs reaches this
int z_usbh_ecm_recv(uint8_t *buf, int max) {
	ecm_recvs++;
	if (!ecm_present) return -1;
	if (next_frame >= n_frames || ecm_recvs < frame_after) return 0;
	int n = frame_len[next_frame];
	if (n > max) return 0;
	memcpy(buf, frames[next_frame++], (size_t)n);
	return n;
}
int z_usbh_ecm_send(const uint8_t *buf, int len) {
	if (!ecm_present || n_sent >= 16) return -1;
	memcpy(sent[n_sent], buf, (size_t)len);
	sent_len[n_sent++] = len;
	return 0;
}
void z_usbh_ecm_want(const uint8_t mac[6]) { memcpy(want_mac, mac, 6); }

// A serial device that has bytes to give.
static uint8_t cdc_in[2048];
static int cdc_in_len, cdc_in_at, cdc_reads;
static uint8_t cdc_out[2048];
static int cdc_out_len;
static int cdc_bound = 1;
int z_usbh_cdc_present(void) { return cdc_bound; }
int z_usbh_cdc_kind(void) { return 1; }
int z_usbh_cdc_read(uint8_t *buf, int max) {
	cdc_reads++;
	if (!cdc_bound) return -1;
	int n = cdc_in_len - cdc_in_at;
	if (n > 64) n = 64;			// a packet at a time
	if (n > max) n = max;
	memcpy(buf, cdc_in + cdc_in_at, (size_t)n);
	cdc_in_at += n;
	return n;
}
int z_usbh_cdc_write(const uint8_t *buf, int len) {
	memcpy(cdc_out + cdc_out_len, buf, (size_t)len);
	cdc_out_len += len;
	return len;
}

// -- the tests --

static void test_msc(void) {
	z_usb_msc_info_t mi;
	uint8_t buf[9 * 512], back[9 * 512];

	client = FS_PID;
	CHECK(zusb_msc_info(&mi) && !mi.present && !mi.ready);

	// A drive binds during a poll: its driver notes it, and the drive's
	// client -- fs, which asked about it -- is told after the poll.
	// usb brings it to ready itself, one step every few ticks, and
	// tells fs only then.
	msc_bound = 1;
	msc_gen++;
	msc_busy_steps = 2;
	boxes[FS_PID].events = 0;
	usbh_notify(Z_USBH_NOTE_MSC);
	usb_test_service();
	CHECK(msc_steps == 1 && !(boxes[FS_PID].events & Z_USBEV_MSC));
	CHECK(zusb_msc_info(&mi) && mi.present && !mi.ready && mi.gen == msc_gen);
	CHECK(zusb_msc_read(0, 1, buf) == -Z_ENODEV);	// not ready yet
	usb_test_service();				// not due yet
	CHECK(msc_steps == 1);
	ticks += 8; usb_test_service();
	CHECK(msc_steps == 2 && !(boxes[FS_PID].events & Z_USBEV_MSC));
	ticks += 8; usb_test_service();
	CHECK(msc_steps == 3 && (boxes[FS_PID].events & Z_USBEV_MSC));
	CHECK(zusb_msc_info(&mi) && mi.ready && mi.sectors == DRIVE_SECTORS);
	CHECK(zusb_msc_start() == 0);			// already: nothing to wait for

	for (int i = 0; i < (int)sizeof(drive); i++) drive[i] = (uint8_t)(i * 7 + (i >> 9));
	msc_reads = 0;
	irqstat = 0;
	CHECK(zusb_msc_read(3, 9, buf) == 0);
	CHECK(!memcmp(buf, drive + 3 * 512, sizeof buf));
	CHECK(msc_reads == 3 && msc_max_count == Z_USB_MSC_MAX);	// 4 + 4 + 1
	CHECK(!msc_irq_was_on);			// quiet around the transfer
	CHECK(irq_on && !(irqstat & 1));	// and its XACT_DONE cleared after

	for (int i = 0; i < (int)sizeof(back); i++) back[i] = (uint8_t)(255 - i);
	CHECK(zusb_msc_write(50, 9, back) == 0);
	CHECK(!memcmp(drive + 50 * 512, back, sizeof back));
	CHECK(zusb_msc_read(60, 9, buf) == -Z_EIO);		// past the end

}

static void put_frame(int len, uint8_t fill, const uint8_t dst[6]) {
	memset(frames[n_frames], fill, (size_t)len);
	memcpy(frames[n_frames], dst, 6);
	frame_len[n_frames++] = len;
}

static void test_net(void) {
	static z_usbnet_cli_t cli;
	const volatile z_usbnet_srv_t *srv;
	z_usbnet_info_t in;
	static const uint8_t mine[6] = { 2, 0, 0, 0, 0, 1 };
	static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };

	client = NET_PID;
	CHECK(z_usbnet_info(&in) && in.present && in.gen == 7);
	srv = z_usbnet_open(mine, &cli);
	CHECK(srv != NULL);
	CHECK(!memcmp(want_mac, mine, 6));
	CHECK(srv->info.present && srv->rx_head == cli.rx_tail);
	CHECK(z_usbnet_alive());
	usb_up = false;				// usb gone (or restarted, a new pid)
	CHECK(!z_usbnet_alive());
	usb_up = true;

	// Frames arrive: into usb's ring, as many as it holds, and net is
	// woken once, when the ring stops being empty.
	for (int i = 0; i < 5; i++) put_frame(60 + i, (uint8_t)i, i == 1 ? mine : bcast);
	boxes[NET_PID].n_events = 0;
	usb_test_service();
	CHECK(srv->rx_head - cli.rx_tail == Z_USBNET_RX_SLOTS);
	CHECK(boxes[NET_PID].n_events == 1 && (boxes[NET_PID].events & Z_USBEV_NET));
	int before = ecm_recvs;
	ticks += 100;
	usb_test_service();			// full: the adapter is not asked
	CHECK(ecm_recvs == before);

	// net takes them, in order, and usb fetches the rest.
	int got = 0;
	for (int round = 0; round < 4 && got < 5; round++) {
		while (srv->rx_head != cli.rx_tail) {
			uint32_t i = cli.rx_tail % Z_USBNET_RX_SLOTS;
			CHECK(srv->rx_len[i] == 60 + got);
			CHECK(srv->rx[i][10] == (uint8_t)got);
			cli.rx_tail++;
			got++;
		}
		ticks += 100;
		usb_test_service();
	}
	CHECK(got == 5);

	// Sending: net's ring, a kick when it was empty, usb drains it.
	uint8_t f[100];
	memset(f, 0x55, sizeof f);
	for (int k = 0; k < Z_USBNET_TX_SLOTS; k++) {
		uint32_t h = cli.tx_head;
		memcpy(cli.tx[h % Z_USBNET_TX_SLOTS], f, sizeof f);
		cli.tx[h % Z_USBNET_TX_SLOTS][0] = (uint8_t)k;
		cli.tx_len[h % Z_USBNET_TX_SLOTS] = sizeof f;
		cli.tx_head = h + 1;
	}
	usb_test_service();
	CHECK(srv->tx_tail == cli.tx_head);
	CHECK(n_sent == Z_USBNET_TX_SLOTS && sent_len[0] == 100 && sent[1][0] == 1);

	// After a send, the adapter is asked back to back (NET_BURST_US),
	// so an answer that lands a few asks later is taken in the same
	// pass, not at the next tick. Not after an ARP reply.
	{
		uint32_t h = cli.tx_head, rx0 = srv->rx_head;
		ticks += 100;
		usb_test_service();		// settle: nothing pending
		frame_after = ecm_recvs + 5;
		put_frame(64, 9, mine);
		memcpy(cli.tx[h % Z_USBNET_TX_SLOTS], f, sizeof f);
		cli.tx[h % Z_USBNET_TX_SLOTS][12] = 0x08;	// IPv4
		cli.tx[h % Z_USBNET_TX_SLOTS][13] = 0x00;
		cli.tx_len[h % Z_USBNET_TX_SLOTS] = sizeof f;
		cli.tx_head = h + 1;
		usb_test_service();
		CHECK(srv->tx_tail == cli.tx_head);
		CHECK(srv->rx_head == rx0 + 1);		// found in the burst
		cli.rx_tail = srv->rx_head;
		frame_after = 0;

		// An ARP reply: one ask, no burst.
		h = cli.tx_head;
		rx0 = srv->rx_head;
		ticks += 100;
		usb_test_service();
		int asks = ecm_recvs;
		frame_after = ecm_recvs + 5;
		put_frame(64, 10, mine);
		memcpy(cli.tx[h % Z_USBNET_TX_SLOTS], f, sizeof f);
		cli.tx[h % Z_USBNET_TX_SLOTS][12] = 0x08;	// ARP
		cli.tx[h % Z_USBNET_TX_SLOTS][13] = 0x06;
		cli.tx_len[h % Z_USBNET_TX_SLOTS] = sizeof f;
		cli.tx_head = h + 1;
		ticks += 100;
		usb_test_service();
		CHECK(srv->rx_head == rx0);		// not yet: no burst
		CHECK(ecm_recvs - asks <= 2);
		frame_after = 0;
		ticks += 100;
		usb_test_service();
		CHECK(srv->rx_head == rx0 + 1);
		cli.rx_tail = srv->rx_head;
	}

	// The adapter goes: usb's ring says so, and net is told.
	ecm_present = 0;
	boxes[NET_PID].events = 0;
	usbh_notify(Z_USBH_NOTE_NET);
	usb_test_service();
	CHECK(!srv->info.present && (boxes[NET_PID].events & Z_USBEV_NET));
	ecm_present = 1;
	usbh_notify(Z_USBH_NOTE_NET);
	usb_test_service();
	CHECK(srv->info.present);

	// net dies: usb forgets its ring and stops reading it.
	alive[NET_PID] = false;
	usb_test_sweep();
	before = ecm_recvs;
	put_frame(60, 9, mine);
	ticks += 100;
	usb_test_service();
	CHECK(ecm_recvs == before);
	alive[NET_PID] = true;
}

static void test_cdc(void) {
	uint8_t b[300];

	client = SERIAL_PID;
	CHECK(z_usbcdc_present());
	CHECK(z_usbcdc_read(b, sizeof b) == 0);		// nothing yet

	// Bytes arrive while serial reads: usb keeps them, and serial is
	// woken once, when there were none kept before.
	for (int i = 0; i < 200; i++) cdc_in[i] = (uint8_t)i;
	cdc_in_len = 200;
	boxes[SERIAL_PID].n_events = 0;
	ticks += 1;
	usb_test_service();
	CHECK(boxes[SERIAL_PID].n_events == 1);
	int32_t n = z_usbcdc_read(b, 50);
	CHECK(n == 50 && b[0] == 0 && b[49] == 49);
	n = z_usbcdc_read(b, sizeof b);
	CHECK(n == 150 && b[0] == 50 && b[149] == 199);

	// Writes, in Z_USB_CDC_MAX pieces.
	uint8_t w[600];
	for (int i = 0; i < 600; i++) w[i] = (uint8_t)(i * 3);
	CHECK(z_usbcdc_write(w, sizeof w) == 600);
	CHECK(cdc_out_len == 600 && !memcmp(cdc_out, w, 600));

	// Not read for a while: usb stops asking the device.
	int before = cdc_reads;
	ticks += 10000;
	usb_test_service();
	CHECK(cdc_reads == before);

	cdc_bound = 0;
	CHECK(!z_usbcdc_present() && z_usbcdc_read(b, 10) == -1);
	cdc_bound = 1;
}

static void test_cmd(void) {
	client = 0;				// the kernel shell
	CHECK(zusb_cmd("lsusb") == 0 && dumps == 1);
	CHECK(zusb_cmd("  usbrescan") == 0 && rescans == 1);
	CHECK(zusb_cmd("usbfrob") == -Z_ENOENT);
	CHECK(zusb_cmd("usbstat") == 0);
}

// A reply is borrowed until its client sends again, so a second
// client's request must not overwrite the first's.
static void test_slots(void) {
	z_usbreq_t rq;
	z_msg_t m1, m2;
	z_blob_t b[2];
	z_obj_t items[2];
	z_obj_table_t t;

	memset(&rq, 0, sizeof rq);
	rq.op = Z_USBOP_MSC_INFO;
	b[0].len = sizeof rq; b[0].data = (uint8_t *)&rq;
	b[1].len = 0; b[1].data = NULL;
	items[0].type = Z_BLOB; items[0].val.ptr = &b[0];
	items[1].type = Z_BLOB; items[1].val.ptr = &b[1];
	t.len = 2; t.a = items; t.b = NULL;
	memset(&m1, 0, sizeof m1);
	m1.subject = Z_USB_REQ; m1.tag = 1; m1.obj.type = Z_LIST; m1.obj.val.ptr = &t;
	m2 = m1; m2.tag = 2;

	m1.from = 10;
	usb_test_serve(&m1);
	const z_usbrep_t *r1 = (const z_usbrep_t *)z_blob_data(&boxes[10].reply.obj);
	z_usbrep_t copy = *r1;
	z_usb_msc_info_t mi1 = *(const z_usb_msc_info_t *)(r1 + 1);
	boxes[10].have_reply = false;

	msc_gen += 100;
	m2.from = 11;
	usb_test_serve(&m2);
	boxes[11].have_reply = false;
	CHECK(r1->result == copy.result && r1->len == copy.len);
	CHECK(((const z_usb_msc_info_t *)(r1 + 1))->gen == mi1.gen);	// untouched
	const z_usbrep_t *r2 = (const z_usbrep_t *)z_blob_data(&boxes[11].reply.obj);
	CHECK(r2 != r1 && ((const z_usb_msc_info_t *)(r2 + 1))->gen == msc_gen);

	// Garbage: refused, still answered.
	rq.op = 999;
	m1.from = 12; m1.tag = 3;
	usb_test_serve(&m1);
	CHECK(((const z_usbrep_t *)z_blob_data(&boxes[12].reply.obj))->result == -Z_EINVAL);
	boxes[12].have_reply = false;
}

int main(void) {
	for (int i = 0; i < 64; i++) alive[i] = true;
	test_msc();
	test_net();
	test_cdc();
	test_cmd();
	test_slots();
	printf("test_usb: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
