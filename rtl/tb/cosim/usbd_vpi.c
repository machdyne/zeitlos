/*
 * Zeitlos -- simulation only.
 *
 * The USB DEVICE's half of tb_usbd_cosim.v: the real sw/apps/usbd
 * (usbd.c) and net's real driver for the frame endpoint (usbd_eth.c),
 * against the real rtl/usbd_ep.v -- whose other end, on the wire, is
 * the real rtl/usb host gateware run by the real sw/apps/usb driver
 * (usbh_vpi.c, the other VPI module). Two implementations written
 * separately, from the specifications, meeting for the first time at
 * D+ and D-.
 *
 * The mechanism is usbh_vpi.c's: the device code runs in its own
 * thread, and each register access it makes (zu_rd/zu_wr, ZU_COSIM in
 * sw/common/zusbd.h) is a real Wishbone cycle on the device's bus,
 * run by the Verilog side between $usbd_step and $usbd_done. Only one
 * thread -- host, device or simulator -- runs at a time.
 *
 * The kernel is stood in for here: its console log (what usbd sends to
 * the terminal) is a buffer the bench fills with ser_pat(), the same
 * pattern usbh_vpi.c's SER_OP_READ checks; its receive ring (what usbd
 * puts typing into) checks what arrives against ser_pat() too, which is
 * what SER_OP_WRITE sends. Frames are ecm_pat(), as the host's
 * ECM_OP_SEND and ECM_OP_RECV use.
 *
 * Built with USBD_MSC (usbd_cosim_msc), the device is `share`'s: the
 * card reader as well, over a card here that holds usbh_vpi.c's medium
 * (msc_pattern()), so the host's MSC_OP_READ checks what it reads.
 */

#include <vpi_user.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "zusbd.h"

#define ST_COMPUTING 0
#define ST_REQUEST   1
#define ST_IDLE      2

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t to_driver = PTHREAD_COND_INITIALIZER;
static pthread_cond_t to_sim = PTHREAD_COND_INITIALIZER;
static int state = ST_COMPUTING;	/* until the thread first waits */
static int go;
static uint32_t req_adr, req_wdat, req_rdat;
static int req_we;
static pthread_t thr;
static int started;

/* -- the code under test -- */

uint32_t usbd_poll(void);
int usbd_state(void);
void usbd_detach(void);
void usbd_attach(uint32_t kp);
#ifdef USBD_MSC
void usbd_msc_medium(int on);
int usbd_msc_ejected(void);
#endif
bool usbd_eth_init(const uint8_t mac[6]);
uint16_t usbd_eth_recv(uint8_t *buf, uint16_t maxlen);
bool usbd_eth_send(const uint8_t *buf, uint16_t len);
int usbd_eth_tx_room(void);

/* -- what the kernel would be -- */

static uint8_t ser_pat(int pos)		/* usbh_vpi.c's, byte for byte */
{
	return (uint8_t)((pos * 13 + (pos >> 8) * 7 + 0x21) & 0xff);
}

static uint8_t ecm_pat(uint8_t seed, int pos)	/* usbh_vpi.c's */
{
	if (pos == 0) return seed;
	return (uint8_t)((seed * 29 + pos * 7 + (pos >> 8) * 3) & 0xff);
}

#define KLOG_SIZE 4096			/* sw/os/uart.c's K_KLOG_SIZE */
static uint32_t klog_end;		/* bytes "printed" since boot */

/* sw/os/uart.c's k_klog_read(), against a log of ser_pat() */
uint32_t cosim_klog_read(uint32_t *pos, uint8_t *buf, uint32_t len, uint32_t *lost)
{
	uint32_t start = *pos, avail = klog_end - start, n, i;
	if (lost) *lost = 0;
	if (avail > KLOG_SIZE) {
		if ((int32_t)avail < 0) { start = klog_end; avail = 0; }
		else {
			if (lost) *lost = avail - KLOG_SIZE;
			start = klog_end - KLOG_SIZE;
			avail = KLOG_SIZE;
		}
	}
	n = avail;
	if (n > len) n = len;
	if (n > 128) n = 128;
	for (i = 0; i < n; i++) buf[i] = ser_pat((int)(start + i));
	*pos = start + n;
	return n;
}

static uint32_t con_in, con_bad, con_calls, ring_cap;

/* k_console_input() as a driver's "physical" input: takes what fits */
uint32_t cosim_console_input(const uint8_t *buf, uint32_t len)
{
	uint32_t n = len, i;
	if (ring_cap && n > ring_cap) n = ring_cap;
	for (i = 0; i < n; i++)
		if (buf[i] != ser_pat((int)con_in++)) con_bad++;
	con_calls++;
	return n;
}

static uint32_t ticks;
uint32_t z_uptime_ticks(void) { return ticks; }

#ifdef USBD_MSC
/* -- the card (share.c's usbd_card_*(), over sdmm.c) -- */

#define CARD_SECTORS	4096		/* 2 MB */
static uint8_t card[CARD_SECTORS * 512];
static uint32_t card_reads, card_writes, card_cmds;
static int card_filled;

/* usbh_vpi.c's msc_pattern() for a sector it has not written */
static uint8_t card_pattern(int lba, int i)
{
	int g = lba * 512 + i;
	if (lba == 0) {
		if (i == 0) return 0xeb;
		if (i == 1) return 0x3c;
		if (i == 2) return 0x90;
		if (i == 3) return 0x6d;
		if (i == 510) return 0x55;
		if (i == 511) return 0xaa;
	}
	return (uint8_t)((g * 7 + (g >> 9) * 13) & 0xff);
}

static void card_fill(void)
{
	int l, i;
	for (l = 0; l < CARD_SECTORS; l++)
		for (i = 0; i < 512; i++)
			card[l * 512 + i] = card_pattern(l, i);
}

uint32_t usbd_card_sectors(void) { return CARD_SECTORS; }

int usbd_card_read(uint32_t lba, uint8_t *buf, uint32_t n)
{
	if (lba + n > CARD_SECTORS) return 1;
	memcpy(buf, card + lba * 512, n * 512);
	card_reads += n;
	card_cmds++;
	return 0;
}

int usbd_card_write(uint32_t lba, const uint8_t *buf, uint32_t n)
{
	if (lba + n > CARD_SECTORS) return 1;
	memcpy(card + lba * 512, buf, n * 512);
	card_writes += n;
	card_cmds++;
	return 0;
}
#endif

/* -- device ops ($usbd_arm / $usbd_result) -- */

#define D_START     0	/* usbd's start: off the bus, events cleared, attach */
#define D_STATE     1	/* rc = usbd_state(), bad = NSTAT */
#define D_CON_PUSH  2	/* the kernel prints arg more bytes of ser_pat */
#define D_CON_STATS 3	/* rc = bytes typed, bad = wrong, first_bad = calls */
#define D_NET_UP    4	/* net starts: usbd_eth_init() */
#define D_NET_DOWN  5	/* net's side down: NCTL 0 */
#define D_NET_SEND  6	/* arg = len | seed << 16: usbd_eth_send(); rc = its return */
#define D_NET_STATS 7	/* rc = frames received, bad = bytes wrong, first_bad = last length */
#define D_DETACH    8	/* off the bus (CTRL 0) */
#define D_RING      9	/* the receive ring takes at most arg bytes a call (0: all) */
#define D_NET_HOLD  10	/* arg 1: net stops reading frames; 0: reads again */
#define D_TX_ROOM   11	/* rc = usbd_eth_tx_room() */
#define D_MSC_ON    12	/* the card is the host's (share has it) */
#define D_MSC_OFF   13	/* Eject pressed here: the medium goes */
#define D_MSC_STATS 14	/* rc = sectors read, bad = written, first_bad = ejected */
#define D_MSC_CHECK 15	/* bad = bytes of sector arg not as the host's WRITE leaves them */

static int op_pending, op, op_arg, op_rc, op_bad, op_first;
static uint32_t net_frames, net_bad, net_last;
static int net_on, net_hold;

static void net_poll(void)
{
	static uint8_t buf[2048];
	uint16_t n;
	int i;
	if (!net_on || net_hold) return;
	while ((n = usbd_eth_recv(buf, sizeof(buf))) != 0) {
		for (i = 1; i < n; i++)
			if (buf[i] != ecm_pat(buf[0], i)) net_bad++;
		net_frames++;
		net_last = n;
	}
}

static void run_op(void)
{
	static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
	static uint8_t buf[2048];
	int i, len;

	op_rc = op_bad = op_first = 0;
	switch (op) {
	case D_START:		/* usbd's main(), up to its first wait */
		usbd_detach();
		usbd_attach(0);
		zu_wr(ZU_IRQEN, usbd_poll());
		break;
	case D_STATE:
		op_rc = usbd_state();
		op_bad = (int)zu_rd(ZU_NSTAT);
		break;
	case D_CON_PUSH:
		klog_end += (uint32_t)op_arg;
		break;
	case D_CON_STATS:
		op_rc = (int)con_in;
		op_bad = (int)con_bad;
		op_first = (int)con_calls;
		break;
	case D_NET_UP:
		op_rc = usbd_eth_init(mac);
		net_on = 1;
		break;
	case D_NET_DOWN:
		zu_wr(ZU_NCTL, 0);
		break;
	case D_NET_SEND:
		len = op_arg & 0xffff;
		for (i = 0; i < len; i++) buf[i] = ecm_pat((uint8_t)(op_arg >> 16), i);
		op_rc = usbd_eth_send(buf, (uint16_t)len);
		break;
	case D_NET_STATS:
		op_rc = (int)net_frames;
		op_bad = (int)net_bad;
		op_first = (int)net_last;
		break;
	case D_DETACH:
		usbd_detach();
		break;
	case D_RING:
		ring_cap = (uint32_t)op_arg;
		break;
	case D_NET_HOLD:
		net_hold = op_arg;
		break;
	case D_TX_ROOM:
		op_rc = usbd_eth_tx_room();
		break;
#ifdef USBD_MSC
	case D_MSC_ON:
		if (!card_filled) { card_fill(); card_filled = 1; }
		usbd_msc_medium(1);
		break;
	case D_MSC_OFF:
		usbd_msc_medium(0);
		break;
	case D_MSC_STATS:
		op_rc = (int)card_reads;
		op_bad = (int)card_writes;
		op_first = usbd_msc_ejected();
		break;
	case D_MSC_CHECK:
		for (i = 0; i < 512; i++)
			if (card[op_arg * 512 + i] != (uint8_t)(i * 5 + op_arg + 0x5a)) op_bad++;
		break;
#endif
	}
}

/* -- the thread's side of the handshake -- */

static void bus_xfer(void)
{
	pthread_mutex_lock(&mtx);
	state = ST_REQUEST;
	pthread_cond_signal(&to_sim);
	while (state == ST_REQUEST)
		pthread_cond_wait(&to_driver, &mtx);
	pthread_mutex_unlock(&mtx);
}

uint32_t zu_rd(uint32_t a)
{
	req_adr = a; req_we = 0;
	bus_xfer();
	return req_rdat;
}

void zu_wr(uint32_t a, uint32_t v)
{
	req_adr = a; req_we = 1; req_wdat = v;
	bus_xfer();
}

static void *device_main(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&mtx);
		state = ST_IDLE;
		go = 0;
		pthread_cond_signal(&to_sim);
		while (!go) pthread_cond_wait(&to_driver, &mtx);
		state = ST_COMPUTING;
		pthread_mutex_unlock(&mtx);

		if (op_pending) {
			run_op();
			op_pending = 0;
		} else {
			// one pass of usbd's loop, then net's: as the two
			// processes would run after an interrupt or a timeout
			uint32_t want = usbd_poll();
			net_poll();
			zu_wr(ZU_IRQEN, want);
		}
	}
	return NULL;
}

/* -- the simulator's side -- */

static void put_int(vpiHandle arg, int v)
{
	s_vpi_value val;
	val.format = vpiIntVal;
	val.value.integer = v;
	vpi_put_value(arg, &val, NULL, vpiNoDelay);
}

static int get_int(vpiHandle arg)
{
	s_vpi_value val;
	val.format = vpiIntVal;
	vpi_get_value(arg, &val);
	return val.value.integer;
}

static void report(vpiHandle *a)
{
	if (state == ST_REQUEST) {
		put_int(a[0], 1);
		put_int(a[1], req_we);
		put_int(a[2], (int)req_adr);
		put_int(a[3], (int)req_wdat);
	} else {
		put_int(a[0], 0);
		put_int(a[1], 0);
		put_int(a[2], 0);
		put_int(a[3], 0);
	}
}

/* $usbd_step(valid, we, adr, wdat): run the device code until it wants
 * a bus cycle (valid 1) or has finished this pass (valid 0) */
static PLI_INT32 usbd_step_calltf(PLI_BYTE8 *ud)
{
	vpiHandle sys, argv, a[4];
	int i;
	(void)ud;
	sys = vpi_handle(vpiSysTfCall, NULL);
	argv = vpi_iterate(vpiArgument, sys);
	for (i = 0; i < 4; i++) a[i] = vpi_scan(argv);
	if (!started) {
		started = 1;
		pthread_create(&thr, NULL, device_main, NULL);
		pthread_mutex_lock(&mtx);
		while (state != ST_IDLE) pthread_cond_wait(&to_sim, &mtx);
		pthread_mutex_unlock(&mtx);
	}
	pthread_mutex_lock(&mtx);
	go = 1;
	state = ST_COMPUTING;
	pthread_cond_signal(&to_driver);
	while (state == ST_COMPUTING)
		pthread_cond_wait(&to_sim, &mtx);
	report(a);
	pthread_mutex_unlock(&mtx);
	return 0;
}

/* $usbd_done(rdat, valid, we, adr, wdat): the cycle's result, and the
 * next request in the same call */
static PLI_INT32 usbd_done_calltf(PLI_BYTE8 *ud)
{
	vpiHandle sys, argv, a[5];
	int i;
	(void)ud;
	sys = vpi_handle(vpiSysTfCall, NULL);
	argv = vpi_iterate(vpiArgument, sys);
	for (i = 0; i < 5; i++) a[i] = vpi_scan(argv);
	pthread_mutex_lock(&mtx);
	req_rdat = (uint32_t)get_int(a[0]);
	state = ST_COMPUTING;
	pthread_cond_signal(&to_driver);
	while (state == ST_COMPUTING)
		pthread_cond_wait(&to_sim, &mtx);
	report(a + 1);
	pthread_mutex_unlock(&mtx);
	return 0;
}

static PLI_INT32 usbd_tick_calltf(PLI_BYTE8 *ud)
{
	(void)ud;
	ticks++;
	return 0;
}

static PLI_INT32 usbd_arm_calltf(PLI_BYTE8 *ud)
{
	vpiHandle sys, argv;
	(void)ud;
	sys = vpi_handle(vpiSysTfCall, NULL);
	argv = vpi_iterate(vpiArgument, sys);
	op = get_int(vpi_scan(argv));
	op_arg = get_int(vpi_scan(argv));
	vpi_free_object(argv);
	op_pending = 1;
	return 0;
}

static PLI_INT32 usbd_result_calltf(PLI_BYTE8 *ud)
{
	vpiHandle sys, argv;
	(void)ud;
	sys = vpi_handle(vpiSysTfCall, NULL);
	argv = vpi_iterate(vpiArgument, sys);
	put_int(vpi_scan(argv), op_rc);
	put_int(vpi_scan(argv), op_bad);
	put_int(vpi_scan(argv), op_first);
	vpi_free_object(argv);
	return 0;
}

static PLI_INT32 compiletf_any(PLI_BYTE8 *ud) { (void)ud; return 0; }

static void reg1(const char *name, PLI_INT32 (*f)(PLI_BYTE8 *))
{
	s_vpi_systf_data t;
	memset(&t, 0, sizeof(t));
	t.type = vpiSysTask;
	t.tfname = (PLI_BYTE8 *)name;
	t.calltf = f;
	t.compiletf = compiletf_any;
	vpi_register_systf(&t);
}

void usbd_register(void)
{
	reg1("$usbd_step", usbd_step_calltf);
	reg1("$usbd_done", usbd_done_calltf);
	reg1("$usbd_tick", usbd_tick_calltf);
	reg1("$usbd_arm", usbd_arm_calltf);
	reg1("$usbd_result", usbd_result_calltf);
}

void (*vlog_startup_routines[])(void) = { usbd_register, 0 };
