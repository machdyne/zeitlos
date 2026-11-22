/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * usbd -- the USB-C socket as a USB device, in software (`USB_DEV,
 * rtl/usbd_ep.v, docs/usb_dev.md). A driver process, like `usb` for the
 * host controller: it takes Z_IRQ_USBD and serves endpoints 0-3.
 *
 * One configuration, a composite of two functions, each an interface
 * association (USB 2.0 ECN "IAD") of a CDC communication interface and
 * its data interface. Written from the USB 2.0 specification (chapter
 * 9) and the CDC 1.2, PSTN 1.2 and ECM 1.2 class specifications:
 *
 *   0  CDC-ACM communication   endpoint 1 IN, interrupt (never sent)
 *   1  CDC data                endpoint 2 OUT and IN, bulk: the console
 *   2  CDC-ECM communication   endpoint 3 IN, interrupt: the link
 *   3  CDC data                alt 0 none; alt 1 endpoint 4 OUT and IN,
 *                              bulk: Ethernet frames, served by the
 *                              gateware and `net` (usbd_eth.c), not here
 *
 * The console. What the kernel's console prints is in its console log
 * (sw/os/uart.c, the same log `console` serves to term): this reads it
 * from where it last stopped and sends it, 64 bytes a packet, while a
 * terminal has the port open (DTR). Opening the port starts from the
 * oldest byte the log still holds -- the boot, on a machine that has
 * just started -- so nothing printed before this process ran is lost.
 * Nothing waits for a terminal: with none open, nothing is sent and the
 * kernel's output goes on as if this were not here. What the terminal
 * types goes into the kernel's receive ring as typing on the serial
 * cable (z_console_input_phys()).
 *
 * The network. The ECM function's data interface has two alternate
 * settings; the host selecting alt 1 turns the gateware's frame
 * endpoint on (EPTYPE bit 12), and from then frames pass between the
 * host and `net` without this process. What is left here: the class
 * request SET_ETHERNET_PACKET_FILTER, accepted; and the link, reported
 * on endpoint 3 as NETWORK_CONNECTION (and CONNECTION_SPEED_CHANGE)
 * whenever `net` brings its side up or down (NCTL bit 0, an event).
 * The host's side of the link has the address in iMACAddress,
 * 02:5A:4C:00:00:01 (locally administered); `net` uses its own.
 *
 * The card reader. Built with -DUSBD_MSC (sw/apps/share, which links
 * this file) a fifth interface is added:
 *
 *   4  mass storage            endpoint 5 OUT and IN, bulk: the SD card
 *                              (Bulk-Only Transport 1.0, SCSI)
 *
 * with bcdDevice 0x0202, so the host does not reuse what it cached for
 * 0x0201. The sectors come from the program around it (usbd_card_*()
 * below); `share` runs this only while `fs` has let go of the card. The
 * commands are machboot's (fw/usbd.c): those a PC sends a card reader.
 *
 * Who runs the device. One process at a time: `usbd` (in flash), or
 * `share` (on the card) while the card is shared. `share` asks `usbd`
 * to step aside (Z_USBD_PAUSE, zusbd.h): `usbd` leaves the bus, gives
 * up IRQ 11 and answers with how far into the console log its terminal
 * had read. `share` then joins the bus as the device with the card
 * reader; giving the card back it leaves, and Z_USBD_RESUME hands the
 * position back. Each change is a new device to the host: it
 * re-enumerates, and the terminal and the network interface come back
 * by themselves. A `share` that dies without resuming is noticed
 * within a second.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zconsole.h"
#include "../../common/zusbd.h"

#ifdef USBD_MSC
/* The card, from the program around this (sw/apps/share/share.c, or the
 * co-simulation): its size in 512-byte sectors (0: none), and n
 * sectors from or to lba (0: done). */
uint32_t usbd_card_sectors(void);
int usbd_card_read(uint32_t lba, uint8_t *buf, uint32_t n);
int usbd_card_write(uint32_t lba, const uint8_t *buf, uint32_t n);
void usbd_msc_medium(int on);
int usbd_msc_ejected(void);
#endif

#ifdef USBD_COSIM
/* rtl/tb/cosim/usbd_vpi.c stands in for the kernel's console log and
 * receive ring (tb_usbd_cosim.v). */
uint32_t cosim_klog_read(uint32_t *pos, uint8_t *buf, uint32_t len, uint32_t *lost);
uint32_t cosim_console_input(const uint8_t *buf, uint32_t len);
#define z_klog_read cosim_klog_read
#define z_console_input_phys cosim_console_input
#endif

#define VID		0x16D0		/* Machdyne's, as tinydfu, machboot and `USB_CDC */
#define PID		0x116D
#ifdef USBD_MSC
#define BCD_DEVICE	0x0202		/* this interface set: console + ethernet + card */
#else
#define BCD_DEVICE	0x0201		/* this interface set: console + ethernet */
#endif

#define IF_ACM		0
#define IF_ACM_DATA	1
#define IF_ECM		2
#define IF_ECM_DATA	3
#define EP_ACM_NOTIFY	1
#define EP_ACM		2
#define EP_ECM_NOTIFY	3
#define EP_NET		4		/* the gateware's frame endpoint */
#define IF_MSC		4
#define EP_MSC		5

#ifdef USBD_MSC
#define N_IF		5
#else
#define N_IF		4
#endif

#define S_MAC		4		/* the iMACAddress string */

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,		/* USB 2.0 */
	0xEF, 0x02, 0x01,		/* composite, with interface associations */
	64,
	VID & 0xff, VID >> 8, PID & 0xff, PID >> 8,
	BCD_DEVICE & 0xff, BCD_DEVICE >> 8,
	1, 2, 0,			/* manufacturer, product, no serial number */
	1,
};

#ifdef USBD_MSC
#define CFG_LEN	177
#else
#define CFG_LEN	154
#endif
static const uint8_t cfg_desc[] = {
	9, 2, CFG_LEN & 0xff, CFG_LEN >> 8, N_IF, 1, 0, 0x80, 250,	/* bus powered, 500 mA: it powers the board */

	/* the console: CDC-ACM */
	8, 0x0B, IF_ACM, 2, 0x02, 0x02, 0x01, 3,
	9, 4, IF_ACM, 0, 1, 0x02, 0x02, 0x01, 3,
	5, 0x24, 0x00, 0x10, 0x01,		/* header, CDC 1.10 */
	5, 0x24, 0x01, 0x00, IF_ACM_DATA,	/* call management */
	4, 0x24, 0x02, 0x02,			/* ACM: line coding and serial state */
	5, 0x24, 0x06, IF_ACM, IF_ACM_DATA,	/* union */
	7, 5, 0x80 | EP_ACM_NOTIFY, 0x03, 8, 0, 255,
	9, 4, IF_ACM_DATA, 0, 2, 0x0A, 0x00, 0x00, 0,
	7, 5, EP_ACM, 0x02, 64, 0, 0,
	7, 5, 0x80 | EP_ACM, 0x02, 64, 0, 0,

	/* the network: CDC-ECM */
	8, 0x0B, IF_ECM, 2, 0x02, 0x06, 0x00, 5,
	9, 4, IF_ECM, 0, 1, 0x02, 0x06, 0x00, 5,
	5, 0x24, 0x00, 0x10, 0x01,		/* header, CDC 1.10 */
	5, 0x24, 0x06, IF_ECM, IF_ECM_DATA,	/* union */
	13, 0x24, 0x0F, S_MAC,			/* Ethernet networking */
	0, 0, 0, 0,				/* no statistics */
	0xEA, 0x05,				/* wMaxSegmentSize 1514 */
	0, 0,					/* no multicast filters */
	0,					/* no power filters */
	7, 5, 0x80 | EP_ECM_NOTIFY, 0x03, 16, 0, 32,
	9, 4, IF_ECM_DATA, 0, 0, 0x0A, 0x00, 0x00, 0,	/* alt 0: no endpoints */
	9, 4, IF_ECM_DATA, 1, 2, 0x0A, 0x00, 0x00, 0,	/* alt 1: the frames */
	7, 5, EP_NET, 0x02, 64, 0, 0,
	7, 5, 0x80 | EP_NET, 0x02, 64, 0, 0,
#ifdef USBD_MSC

	/* the card: mass storage, SCSI transparent, bulk-only */
	9, 4, IF_MSC, 0, 2, 0x08, 0x06, 0x50, 6,
	7, 5, 0x80 | EP_MSC, 0x02, 64, 0, 0,
	7, 5, EP_MSC, 0x02, 64, 0, 0,
#endif
};
_Static_assert(sizeof(cfg_desc) == CFG_LEN, "cfg_desc's length");

static const char *const strings[] = {
	0,
	"Machdyne",
	"Zeitlos",
	"Zeitlos console",
	"025A4C000001",			/* S_MAC: the host's side of the link */
	"Zeitlos network",
#ifdef USBD_MSC
	"Zeitlos card",
#endif
};

#ifndef USBD_NO_MAIN
/* The few lines this prints, one Z_SYS_UART_PUTC each: into the
 * kernel's console (and so its log) without newlib's stdio, which would
 * be an eighth of this program. */
static void say(const char *s)
{
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t o;
	o.type = Z_INT32;
	for (; *s; s++) {
		if (*s == '\n') {
			o.val.int32 = '\r';
			k(Z_SYS_UART_PUTC, (uint32_t *)&o, 0);
		}
		o.val.int32 = *s;
		k(Z_SYS_UART_PUTC, (uint32_t *)&o, 0);
	}
}
#endif

/* -- endpoint buffers -- */

static void ep_send(int ep, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)p[i + k] << (8 * k);
		zu_wr(ZU_IBUF(ep) + i, w);
	}
	zu_wr(ZU_IN(ep), n | ZU_IN_ARMED);
}

static uint32_t ep_recv(int ep, uint8_t *p, uint32_t max)
{
	uint32_t n = (zu_rd(ZU_OUT(ep)) >> 8) & 0x7f;
	if (n > max)
		n = max;
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = zu_rd(ZU_OBUF(ep) + i);
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			p[i + k] = (uint8_t)(w >> (8 * k));
	}
	return n;
}

static bool ep_armed(int ep)
{
	return (zu_rd(ZU_IN(ep)) & ZU_IN_ARMED) != 0;
}

/* -- state -- */

static int configured;
static int net_alt;			/* the ECM data interface's alternate setting */
static uint32_t eptype;
static int cdc_open;			/* a terminal has the port (DTR) */
static uint32_t kpos_open;		/* where a terminal that opens starts in the log */

/* -- control transfers on endpoint 0 -- */

enum { C_IDLE, C_IN, C_OUT, C_STATUS_IN };

static int ctl;
static uint8_t rq[8];
static uint8_t ctl_buf[64];
static const uint8_t *tx_p;
static uint32_t tx_left;
static int tx_zlp;
static uint8_t *rx_p;
static uint32_t rx_left;
static void (*rx_done)(void);
static int set_addr = -1;
static int in0_busy;

static void stall0(void)
{
	zu_wr(ZU_STALL(0), 1);
	ctl = C_IDLE;
}

static void in0_next(void)
{
	uint32_t n = tx_left < 64 ? tx_left : 64;
	ep_send(0, tx_p, n);
	tx_p += n;
	tx_left -= n;
	in0_busy = 1;
}

/* an IN data stage: n bytes of what the host asked wlen of */
static void ctl_send(const uint8_t *p, uint32_t n, uint32_t wlen)
{
	if (n > wlen)
		n = wlen;
	tx_p = p;
	tx_left = n;
	tx_zlp = n && (n < wlen) && (n % 64 == 0);	/* a short transfer ends with a short packet */
	ctl = C_IN;
	in0_next();
}

static void status_in(void)
{
	ep_send(0, 0, 0);
	in0_busy = 1;
	ctl = C_STATUS_IN;
}

/* an OUT data stage into p; done() runs when it is all there */
static void ctl_recv(uint8_t *p, uint32_t n, void (*done)(void))
{
	rx_p = p;
	rx_left = n;
	rx_done = done;
	ctl = C_OUT;
}

static void in0_done(void)
{
	if (ctl == C_IN) {
		if (tx_left)
			in0_next();
		else if (tx_zlp) {
			tx_zlp = 0;
			ep_send(0, 0, 0);
			in0_busy = 1;
		} else {
			ctl = C_IDLE;		/* the host's status OUT follows */
		}
	} else if (ctl == C_STATUS_IN) {
		if (set_addr >= 0) {
			zu_wr(ZU_CTRL, 1 | ((uint32_t)set_addr << 8));
			set_addr = -1;
		}
		ctl = C_IDLE;
	}
}

/* -- the console -- */

static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };	/* 115200 8N1: stored, means nothing */
static uint32_t kpos;			/* how far into the console log this has sent */
static int cdc_busy;			/* a packet is armed on endpoint 2 */
static uint32_t cdc_last;		/* the last packet's length: after a 64, a short one */
static uint8_t rxp[64];			/* typed, not yet taken by the kernel's ring */
static uint32_t rxp_o, rxp_n;

static void cdc_tx(void)
{
	uint8_t b[64];
	uint32_t lost = 0, n;

	if (!cdc_open)
		return;
	if (ep_armed(EP_ACM)) {
		cdc_busy = 1;
		return;
	}
	cdc_busy = 0;
	n = z_klog_read(&kpos, b, sizeof(b), &lost);
	if (n) {
		ep_send(EP_ACM, b, n);
		cdc_last = n;
		cdc_busy = 1;
	} else if (cdc_last == 64) {
		/* a transfer the host may be reading in larger pieces ends
		 * with a short packet, or the last 64 bytes wait in its buffer */
		ep_send(EP_ACM, 0, 0);
		cdc_last = 0;
		cdc_busy = 1;
	}
}

static void cdc_rx(void)
{
	uint32_t o, k;

	if (rxp_n) {
		k = z_console_input_phys(rxp + rxp_o, rxp_n);
		rxp_o += k;
		rxp_n -= k;
		if (rxp_n)
			return;			/* the ring is full: the endpoint NAKs until it is not */
	}
	o = zu_rd(ZU_OUT(EP_ACM));
	if (!(o & ZU_OUT_FULL))
		return;
	rxp_n = ep_recv(EP_ACM, rxp, sizeof(rxp));
	rxp_o = 0;
	zu_wr(ZU_OUT(EP_ACM), 0);
	if (rxp_n) {
		k = z_console_input_phys(rxp, rxp_n);
		rxp_o = k;
		rxp_n -= k;
	}
}

static void line_coding_done(void)
{
	status_in();
}

/* -- the network's link, on endpoint 3 -- */

static int ntf_sent = -1;		/* the link the host was last told; -1 none yet */
static int ntf_stage;			/* 1: NETWORK_CONNECTION armed, 2: the speed */
static int ntf_busy;

static void notify(void)
{
	static const uint8_t speed[16] = {
		0xA1, 0x2A, 0, 0, IF_ECM, 0, 8, 0,
		0x00, 0x1B, 0xB7, 0x00,		/* 12 Mbit/s down */
		0x00, 0x1B, 0xB7, 0x00,		/* and up */
	};
	uint8_t nc[8] = { 0xA1, 0x00, 0, 0, IF_ECM, 0, 0, 0 };
	int link;

	if (ep_armed(EP_ECM_NOTIFY)) {
		ntf_busy = 1;
		return;
	}
	ntf_busy = 0;
	if (!net_alt)
		return;
	if (ntf_stage == 1 && ntf_sent == 1) {
		ep_send(EP_ECM_NOTIFY, speed, sizeof(speed));
		ntf_stage = 2;
		ntf_busy = 1;
		return;
	}
	ntf_stage = 0;
	link = (zu_rd(ZU_NSTAT) & ZU_N_LINK) ? 1 : 0;
	if (link == ntf_sent)
		return;
	nc[2] = (uint8_t)link;
	ep_send(EP_ECM_NOTIFY, nc, sizeof(nc));
	ntf_sent = link;
	ntf_stage = 1;
	ntf_busy = 1;
}

static void set_net_alt(int alt)
{
	net_alt = alt;
	if (alt)
		eptype |= ZU_EPTYPE_FRAME;
	else
		eptype &= ~ZU_EPTYPE_FRAME;
	zu_wr(ZU_EPTYPE, eptype);
	zu_wr(ZU_TOGGLE, ZU_TOG_IN(EP_NET) | ZU_TOG_OUT(EP_NET));
	ntf_sent = -1;		/* tell the host the link again */
	ntf_stage = 0;
}

/* -- the card reader, on endpoint 5 (USB Mass Storage Bulk-Only
 * Transport 1.0; SCSI SPC-2/SBC-2, the few commands a PC sends a card
 * reader: machboot's fw/usbd.c) -- */

#ifdef USBD_MSC

#define MSC_SECS	8		/* sectors a card transfer: CMD18/CMD25 runs */
#define MSC_BUF		(MSC_SECS * 512)

enum { M_CBW, M_IN, M_OUT, M_CSW };
enum { MS_OK = 0, MS_FAIL = 1 };
enum { OP_BUF = 0, OP_PAD = -1, OP_READ = 0x28, OP_WRITE = 0x2A };

static int m_state;
static uint8_t m_cbw[31];
static uint32_t m_tag, m_len, m_done;	/* the host's length, bytes moved */
static int m_status;
static uint8_t m_sense[3];		/* key, ASC, ASCQ */
static uint8_t m_buf[MSC_BUF] __attribute__((aligned(4)));	/* the card's DMA wants words */
static uint32_t m_buf_len, m_buf_pos;
static uint32_t m_lba, m_count;		/* READ/WRITE: sectors left */
static int m_op;
static int m_medium;			/* the card is the host's */
static int m_changed;			/* tell the host once: the medium changed */
static int m_ejected;			/* the host said START STOP UNIT, eject */
static uint32_t m_sectors;

static void put32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
	m_sense[0] = key; m_sense[1] = asc; m_sense[2] = ascq;
}

void usbd_msc_medium(int on)
{
	if (on) {
		m_sectors = usbd_card_sectors();
		on = m_sectors != 0;
	}
	if (on == m_medium)
		return;
	m_medium = on;
	m_changed = 1;
	if (on)
		m_ejected = 0;
}

static int msc_ready(void)
{
	if (!m_medium) {
		sense(0x02, 0x3A, 0x00);	/* NOT READY, medium not present */
		return 0;
	}
	if (m_changed) {
		m_changed = 0;
		sense(0x06, 0x28, 0x00);	/* UNIT ATTENTION, medium may have changed */
		return 0;
	}
	return 1;
}

/* answer with n bytes of m_buf, at most what the host asked for */
static void m_reply(uint32_t n)
{
	m_buf_len = n < m_len ? n : m_len;
	m_buf_pos = 0;
	m_op = OP_BUF;
	m_state = M_IN;
}

static void m_fail(uint8_t key, uint8_t asc)
{
	sense(key, asc, 0);
	m_status = MS_FAIL;
	m_buf_len = 0;
	m_buf_pos = 0;
	m_op = OP_PAD;
	if (m_len && (m_cbw[12] & 0x80))
		m_state = M_IN;		/* zeros, all it asked for (BOT 6.7.2), then fail */
	else if (m_len)
		m_state = M_OUT;	/* take and drop the data */
	else
		m_state = M_CSW;
}

static void msc_command(void)
{
	const uint8_t *cb = m_cbw + 15;
	m_status = MS_OK;
	m_done = 0;
	switch (cb[0]) {
	case 0x00:		/* TEST UNIT READY */
		if (!msc_ready())
			m_status = MS_FAIL;
		m_state = M_CSW;
		return;
	case 0x03:		/* REQUEST SENSE */
		memset(m_buf, 0, 18);
		m_buf[0] = 0x70;
		m_buf[2] = m_sense[0];
		m_buf[7] = 10;
		m_buf[12] = m_sense[1];
		m_buf[13] = m_sense[2];
		sense(0, 0, 0);
		m_reply(18);
		return;
	case 0x12: {		/* INQUIRY */
		static const char id[] = "Machdyne" "Zeitlos SD card " "1   ";
		memset(m_buf, 0, 36);
		m_buf[1] = 0x80;	/* removable */
		m_buf[2] = 0x04;	/* SPC-2 */
		m_buf[3] = 0x02;
		m_buf[4] = 31;
		memcpy(m_buf + 8, id, 28);
		m_reply(36);
		return;
	}
	case 0x1A:		/* MODE SENSE (6) */
		memset(m_buf, 0, 4);
		m_buf[0] = 3;
		m_reply(4);
		return;
	case 0x5A:		/* MODE SENSE (10) */
		memset(m_buf, 0, 8);
		m_buf[1] = 6;
		m_reply(8);
		return;
	case 0x1E:		/* PREVENT/ALLOW MEDIUM REMOVAL */
	case 0x35:		/* SYNCHRONIZE CACHE: every write is on the card when its CSW goes */
	case 0x2F:		/* VERIFY */
		m_state = M_CSW;
		return;
	case 0x1B:		/* START STOP UNIT: eject gives the card back */
		if ((cb[4] & 0x03) == 0x02 && m_medium) {
			m_medium = 0;
			m_changed = 0;
			m_ejected = 1;
		}
		m_state = M_CSW;
		return;
	case 0x25:		/* READ CAPACITY (10) */
		if (!msc_ready()) { m_fail(m_sense[0], m_sense[1]); return; }
		put32be(m_buf, m_sectors - 1);
		put32be(m_buf + 4, 512);
		m_reply(8);
		return;
	case 0x23:		/* READ FORMAT CAPACITIES */
		if (!msc_ready()) { m_fail(m_sense[0], m_sense[1]); return; }
		memset(m_buf, 0, 12);
		m_buf[3] = 8;
		put32be(m_buf + 4, m_sectors);
		m_buf[8] = 0x02;	/* formatted */
		m_buf[10] = 0x02;	/* 512 */
		m_reply(12);
		return;
	case OP_READ:		/* READ (10) */
	case OP_WRITE:		/* WRITE (10) */
		if (!msc_ready()) { m_fail(m_sense[0], m_sense[1]); return; }
		m_lba = (uint32_t)cb[2] << 24 | (uint32_t)cb[3] << 16 | (uint32_t)cb[4] << 8 | cb[5];
		m_count = (uint32_t)cb[7] << 8 | cb[8];
		if (m_lba + m_count > m_sectors || m_lba + m_count < m_lba || m_count * 512 != m_len) {
			m_fail(0x05, 0x21);	/* ILLEGAL REQUEST, LBA out of range */
			return;
		}
		m_op = cb[0];
		m_buf_len = 0;
		m_buf_pos = 0;
		m_state = cb[0] == OP_READ ? M_IN : M_OUT;
		return;
	default:
		m_fail(0x05, 0x20);		/* ILLEGAL REQUEST, invalid command */
		return;
	}
}

/* the next run of sectors to read or write: at most MSC_SECS */
static uint32_t m_run(void)
{
	return m_count < MSC_SECS ? m_count : MSC_SECS;
}

static void msc_poll(void)
{
	if (m_state == M_CBW) {
		uint32_t n;
		if (!(zu_rd(ZU_OUT(EP_MSC)) & ZU_OUT_FULL))
			return;
		n = ep_recv(EP_MSC, m_cbw, sizeof(m_cbw));
		zu_wr(ZU_OUT(EP_MSC), 0);
		if (n != 31 || m_cbw[0] != 'U' || m_cbw[1] != 'S' || m_cbw[2] != 'B' || m_cbw[3] != 'C')
			return;		/* not a CBW: ignore it */
		m_tag = (uint32_t)m_cbw[4] | (uint32_t)m_cbw[5] << 8 | (uint32_t)m_cbw[6] << 16 | (uint32_t)m_cbw[7] << 24;
		m_len = (uint32_t)m_cbw[8] | (uint32_t)m_cbw[9] << 8 | (uint32_t)m_cbw[10] << 16 | (uint32_t)m_cbw[11] << 24;
		msc_command();
		return;
	}
	if (m_state == M_IN) {
		uint32_t n;
		if (ep_armed(EP_MSC))
			return;
		if (m_buf_pos >= m_buf_len) {
			/* the next run of sectors, padding, or the end */
			if (m_op == OP_READ && m_count) {
				uint32_t k = m_run();
				if (usbd_card_read(m_lba, m_buf, k)) {
					sense(0x03, 0x11, 0);	/* MEDIUM ERROR, unrecovered read */
					m_status = MS_FAIL;
					m_op = OP_PAD;		/* the rest is zeros: the host gets all it asked for */
				} else {
					m_lba += k;
					m_count -= k;
					m_buf_len = k * 512;
					m_buf_pos = 0;
				}
			}
			if (m_op == OP_PAD && m_done < m_len) {
				m_buf_len = m_len - m_done < MSC_BUF ? m_len - m_done : MSC_BUF;
				memset(m_buf, 0, m_buf_len);
				m_buf_pos = 0;
			} else if (m_buf_pos >= m_buf_len) {
				m_state = M_CSW;
				msc_poll();
				return;
			}
		}
		n = m_buf_len - m_buf_pos < 64 ? m_buf_len - m_buf_pos : 64;
		ep_send(EP_MSC, m_buf + m_buf_pos, n);
		m_buf_pos += n;
		m_done += n;
		return;
	}
	if (m_state == M_OUT) {
		uint32_t n;
		if (!(zu_rd(ZU_OUT(EP_MSC)) & ZU_OUT_FULL))
			return;
		n = ep_recv(EP_MSC, m_buf + m_buf_pos, MSC_BUF - m_buf_pos);
		zu_wr(ZU_OUT(EP_MSC), 0);
		m_buf_pos += n;
		m_done += n;
		if (m_op == OP_WRITE && m_buf_pos >= m_run() * 512) {
			uint32_t k = m_run();
			if (usbd_card_write(m_lba, m_buf, k)) {
				sense(0x03, 0x0C, 0);	/* MEDIUM ERROR, write error */
				m_status = MS_FAIL;
				m_op = OP_PAD;
			}
			m_lba += k;
			m_count -= k;
			m_buf_pos = 0;
		} else if (m_op != OP_WRITE && m_buf_pos + 64 > MSC_BUF) {
			m_buf_pos = 0;
		}
		if (m_done >= m_len || n < 64) {
			m_state = M_CSW;
			msc_poll();
		}
		return;
	}
	if (m_state == M_CSW) {
		uint8_t csw[13];
		uint32_t res = m_len > m_done ? m_len - m_done : 0;
		int i;
		if (ep_armed(EP_MSC))
			return;
		csw[0] = 'U'; csw[1] = 'S'; csw[2] = 'B'; csw[3] = 'S';
		for (i = 0; i < 4; i++) {
			csw[4 + i] = (uint8_t)(m_tag >> (8 * i));
			csw[8 + i] = (uint8_t)(res >> (8 * i));
		}
		csw[12] = (uint8_t)m_status;
		ep_send(EP_MSC, csw, 13);
		m_state = M_CBW;
	}
}

/* what should wake the loop for the card reader */
static uint32_t msc_want(void)
{
	if (m_state == M_CBW || m_state == M_OUT)
		return ZU_P_OUT(EP_MSC);
	return ZU_P_IN(EP_MSC);		/* M_IN, M_CSW: the packet before has gone */
}

/* the host ejected the card, and the answer has gone: it may be given back */
int usbd_msc_ejected(void)
{
	return m_ejected && m_state == M_CBW && !ep_armed(EP_MSC);
}

#endif

/* -- requests -- */

static void configure(int cfg)
{
	configured = cfg;
	net_alt = 0;
	ntf_sent = -1;
	ntf_stage = 0;
	if (cfg) {
		uint32_t bulk = 1u << EP_ACM;
		uint32_t intr = (1u << EP_ACM_NOTIFY) | (1u << EP_ECM_NOTIFY);
#ifdef USBD_MSC
		bulk |= 1u << EP_MSC;
		m_state = M_CBW;
#endif
		eptype = bulk | (bulk << 8) | (intr << 16);
		zu_wr(ZU_EPTYPE, eptype);
		zu_wr(ZU_TOGGLE, bulk | intr | (bulk << 8) | ZU_TOG_IN(EP_NET) | ZU_TOG_OUT(EP_NET));
	} else {
		eptype = 0;
		zu_wr(ZU_EPTYPE, 0);
		cdc_open = 0;
	}
}

static void get_descriptor(uint8_t type, uint8_t index, uint16_t len)
{
	if (type == 1) {
		ctl_send(dev_desc, sizeof(dev_desc), len);
	} else if (type == 2) {
		ctl_send(cfg_desc, sizeof(cfg_desc), len);
	} else if (type == 3) {
		uint32_t n;
		if (index == 0) {
			ctl_buf[2] = 0x09;	/* English (US) */
			ctl_buf[3] = 0x04;
			n = 4;
		} else if (index < sizeof(strings) / sizeof(strings[0])) {
			const char *s = strings[index];
			n = 2;
			for (; *s && n + 2 <= sizeof(ctl_buf); s++) {
				ctl_buf[n++] = (uint8_t)*s;
				ctl_buf[n++] = 0;
			}
		} else {
			stall0();
			return;
		}
		ctl_buf[0] = (uint8_t)n;
		ctl_buf[1] = 3;
		ctl_send(ctl_buf, n, len);
	} else {
		stall0();		/* device qualifier and others: full speed only */
	}
}

static void setup(void)
{
	uint8_t type = (rq[0] >> 5) & 3, recip = rq[0] & 0x1f, req = rq[1];
	uint16_t value = (uint16_t)(rq[2] | rq[3] << 8);
	uint16_t index = (uint16_t)(rq[4] | rq[5] << 8);
	uint16_t len = (uint16_t)(rq[6] | rq[7] << 8);
	int ep;

	ctl = C_IDLE;
	in0_busy = 0;

	if (type == 0) {
		switch (req) {
		case 6:		/* GET_DESCRIPTOR */
			get_descriptor((uint8_t)(value >> 8), (uint8_t)value, len);
			return;
		case 5:		/* SET_ADDRESS: after the status stage */
			set_addr = value & 0x7f;
			status_in();
			return;
		case 9:		/* SET_CONFIGURATION */
			if (value > 1) {
				stall0();
				return;
			}
			configure(value & 0xff);
			status_in();
			return;
		case 8:		/* GET_CONFIGURATION */
			ctl_buf[0] = (uint8_t)configured;
			ctl_send(ctl_buf, 1, len);
			return;
		case 0:		/* GET_STATUS */
			ctl_buf[0] = 0;
			ctl_buf[1] = 0;
			if (recip == 2 && (index & 0x0f) < 8 && (index & 0x0f) != EP_NET)
				ctl_buf[0] = (uint8_t)(zu_rd(ZU_STALL(index & 7)) & 1);
			ctl_send(ctl_buf, 2, len);
			return;
		case 1:		/* CLEAR_FEATURE */
		case 3:		/* SET_FEATURE */
			if (recip == 2 && value == 0) {		/* ENDPOINT_HALT */
				ep = index & 0x0f;
				if (ep == EP_NET) {
					/* no halt in the gateware's endpoint: a clear is
					 * a toggle reset, a set is accepted and ignored */
					if (req == 1)
						zu_wr(ZU_TOGGLE, (index & 0x80) ? ZU_TOG_IN(EP_NET) : ZU_TOG_OUT(EP_NET));
				} else if (ep > 7) {
					stall0();
					return;
				} else {
					zu_wr(ZU_STALL(ep), req == 3);
					if (req == 1)
						zu_wr(ZU_TOGGLE, (index & 0x80) ? ZU_TOG_IN(ep) : ZU_TOG_OUT(ep));
				}
			}
			status_in();
			return;
		case 10:	/* GET_INTERFACE */
			ctl_buf[0] = (uint8_t)(((index & 0xff) == IF_ECM_DATA) ? net_alt : 0);
			ctl_send(ctl_buf, 1, len);
			return;
		case 11:	/* SET_INTERFACE */
			if ((index & 0xff) == IF_ECM_DATA && value <= 1 && configured) {
				set_net_alt(value);
				status_in();
				return;
			}
			if (value != 0 || (index & 0xff) >= N_IF) {
				stall0();
				return;
			}
			status_in();
			return;
		default:
			stall0();
			return;
		}
	}
	if (type == 1 && recip == 1) {
		if ((index & 0xff) == IF_ACM) {
			switch (req) {
			case 0x20:	/* SET_LINE_CODING */
				ctl_recv(line_coding, sizeof(line_coding), line_coding_done);
				return;
			case 0x21:	/* GET_LINE_CODING */
				ctl_send(line_coding, sizeof(line_coding), len);
				return;
			case 0x22:	/* SET_CONTROL_LINE_STATE: DTR is "a terminal is open" */
				if ((value & 1) && !cdc_open) {
					/* everything the log still holds, or, after
					 * the card was shared, where the terminal
					 * had got to */
					kpos = kpos_open;
					cdc_last = 0;
				}
				cdc_open = value & 1;
				status_in();
				return;
			case 0x23:	/* SEND_BREAK */
				status_in();
				return;
			default:
				stall0();
				return;
			}
		}
		if ((index & 0xff) == IF_ECM) {
			if (req == 0x43) {	/* SET_ETHERNET_PACKET_FILTER: whatever it asks */
				status_in();
				return;
			}
		}
#ifdef USBD_MSC
		if ((index & 0xff) == IF_MSC) {
			if (req == 0xFE) {	/* GET MAX LUN: one unit */
				ctl_buf[0] = 0;
				ctl_send(ctl_buf, 1, len);
				return;
			}
			if (req == 0xFF) {	/* Bulk-Only Mass Storage Reset */
				m_state = M_CBW;
				zu_wr(ZU_IN(EP_MSC), 0);
				status_in();
				return;
			}
		}
#endif
	}
	stall0();
}

static void bus_reset(void)
{
	ctl = C_IDLE;
	in0_busy = 0;
	set_addr = -1;
	configure(0);
	cdc_busy = 0;
	ntf_busy = 0;
}

/* -- the loop -- */

static void service(void)
{
	uint32_t ev = zu_rd(ZU_EVENTS), o;

	if (ev & ZU_EV_RESET) {
		zu_wr(ZU_EVENTS, ZU_EV_RESET);
		bus_reset();
	}
	if (ev & ZU_EV_LINK)
		zu_wr(ZU_EVENTS, ZU_EV_LINK);	/* notify() reads the link itself */

	o = zu_rd(ZU_OUT(0));
	if (o & ZU_OUT_FULL) {
		if (o & ZU_OUT_SETUP) {
			memset(rq, 0, sizeof(rq));
			ep_recv(0, rq, sizeof(rq));
			zu_wr(ZU_OUT(0), 0);
			setup();
		} else if (ctl == C_OUT) {
			uint32_t n = ep_recv(0, rx_p, rx_left);
			zu_wr(ZU_OUT(0), 0);
			rx_p += n;
			rx_left -= n;
			if (rx_left == 0 || n < 64) {
				ctl = C_IDLE;
				rx_done();
			}
		} else {
			zu_wr(ZU_OUT(0), 0);	/* a status stage, or nothing we wait for */
		}
	}

	if (in0_busy && !ep_armed(0)) {
		in0_busy = 0;
		in0_done();
	}

	if (configured) {
		cdc_rx();
		cdc_tx();
		notify();
#ifdef USBD_MSC
		msc_poll();
#endif
	}
}

/* what should wake this: whatever is still to come */
static uint32_t want_irq(void)
{
	uint32_t m = ZU_P_OUT(0) | ZU_P_RESET | ZU_P_LINK;
	if (in0_busy)
		m |= ZU_P_IN(0);
	if (configured) {
		if (!rxp_n)
			m |= ZU_P_OUT(EP_ACM);
		if (cdc_busy)
			m |= ZU_P_IN(EP_ACM);
		if (ntf_busy)
			m |= ZU_P_IN(EP_ECM_NOTIFY);
#ifdef USBD_MSC
		m |= msc_want();
#endif
	}
	return m;
}

/* Off the bus, and every endpoint with it. */
static void usbd_off(void)
{
	zu_wr(ZU_CTRL, 0);
	zu_wr(ZU_EPTYPE, 0);
	zu_wr(ZU_IRQEN, 0);
	bus_reset();
}

/* Onto the bus, a while after usbd_off(): the host must have seen the
 * old device go (a few milliseconds). A terminal that opens starts at
 * `kp` in the console log. */
static void usbd_on(uint32_t kp)
{
	kpos_open = kp;
	zu_wr(ZU_EVENTS, ZU_EV_RESET | ZU_EV_LINK);
	zu_wr(ZU_CTRL, 1);		/* attach: the pull-up goes on */
}

/* Where the next device's terminal should start: where this one's had
 * got to, or, with none open, where this one's would have started. */
static uint32_t usbd_kpos(void)
{
	return cdc_open ? kpos : kpos_open;
}

#ifdef USBD_NO_MAIN
/* For the program that runs this without its main(): sw/apps/share,
 * and the co-simulation. */
uint32_t usbd_poll(void)
{
	service();
	return want_irq();
}
void usbd_detach(void)
{
	usbd_off();
}
void usbd_attach(uint32_t kp)
{
	usbd_on(kp);
}
uint32_t usbd_console_pos(void)
{
	return usbd_kpos();
}
int usbd_state(void)
{
	return configured | cdc_open << 1 | net_alt << 2 | (ntf_sent == 1) << 3;
}
#endif


#ifndef USBD_NO_MAIN
static bool irq;

static void take_irq(void)
{
	// The kernel clears IRQEN when the line fires, so that a level
	// cannot hold the CPU in its handler until this process runs; the
	// loop turns it back on once it has served what woke it.
	irq = z_irq_bind(Z_IRQ_USBD, ZU_IRQEN, 0) == Z_OK;
	if (!irq)
		say("usbd: cannot take IRQ 11; polling\n");
}

static bool is_share(uint32_t pid)
{
	uint32_t p;
	return z_pid_lookup(Z_USBD_SHARE_NAME, &p) && p == pid;
}

static void reply_paused(uint32_t to, bool ok)
{
	z_obj_t o;
	memset(&o, 0, sizeof(o));
	o.type = ok ? Z_UINT32 : Z_NONE;
	o.val.uint32 = usbd_kpos();
	(void)z_msg_new_send(to, Z_USBD_PAUSED, 0, o);
}

// The card is shared: `share` is the device until it says it is done,
// or is gone. Off the bus and IRQ 11 first, then the answer.
static void paused(uint32_t who)
{
	uint32_t kp = usbd_kpos();
	z_msg_t m;

	usbd_off();
	if (irq)
		(void)z_irq_unbind(Z_IRQ_USBD);
	irq = false;
	reply_paused(who, true);
	for (;;) {
		if (z_msg_recv(&m, Z_TICK_HZ) == Z_OK) {
			if (m.subject == Z_USBD_RESUME && m.from == who) {
				if (m.obj.type == Z_UINT32)
					kp = m.obj.val.uint32;
				break;
			}
			if (m.subject == Z_USBD_PAUSE)
				reply_paused(m.from, false);
			continue;
		}
		if (!is_share(who))
			break;		/* it died: the device is ours again */
	}
	take_irq();
	// share left the bus as it went; the host must see the gap
	z_proc_wait(Z_TICK_HZ / 50 + 1);
	usbd_on(kp);
}

int main(void)
{
	char name[16];
	uint32_t name_pid;

	if (!z_usb_dev() || zu_rd(ZU_MAGIC) != ZU_MAGIC_VAL) {
		say("usbd: no USB device in this bitstream, nothing to do\n");
		return 0;
	}
	if (!z_pid_register("usbd", name, sizeof(name))) {
		say("usbd: cannot register\n");
		return 1;
	}
	// Restarted while the card is shared: the device is share's until
	// it has gone (it hands back to the usbd it paused, not this one).
	while (z_pid_lookup(Z_USBD_SHARE_NAME, &name_pid))
		z_proc_wait(Z_TICK_HZ);
	take_irq();

	// Off the bus first, for a few milliseconds: after a restart the
	// host still has the old device, and must see it go and come back
	// (a fresh boot was never on the bus, and loses only the wait).
	usbd_off();
	z_proc_wait(Z_TICK_HZ / 50 + 1);
	usbd_on(0);		/* a terminal gets everything the log still holds */
	say("usbd: console and network on the USB-C port\n");

	// Serve, say what should wake this, then wait: in that order from
	// the first pass, so the interrupts are on before the host's first
	// request -- a wait first left IRQEN at 0 for the whole first
	// timeout, and the host's GET_DESCRIPTOR met an endpoint nobody
	// watched (tb_usbd_cosim.v found it).
	for (;;) {
		z_msg_t m;
		uint32_t t = irq ? Z_TICK_HZ : 1;

		service();
		if (irq)
			zu_wr(ZU_IRQEN, want_irq());
		// A terminal open: look in the console log about a hundred
		// times a second, which is what notices new output -- the
		// kernel has no wake for it. Input held back by a full ring:
		// try again soon.
		if (cdc_open || rxp_n)
			t = Z_TICK_HZ / 100 + 1;
		if (z_msg_recv(&m, t) == Z_OK && m.subject == Z_USBD_PAUSE) {
			if (is_share(m.from))
				paused(m.from);
			else
				reply_paused(m.from, false);
		}
	}
	return 0;
}
#endif
