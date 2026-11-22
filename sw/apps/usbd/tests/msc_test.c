/*
 * Zeitlos -- host test, not for the board.
 *
 * The card reader's Bulk-Only Transport and SCSI commands (usbd.c built
 * with -DUSBD_MSC), one command at a time, against a model of what the
 * CPU sees of rtl/usbd_ep.v: each endpoint's OUT buffer (full, SETUP,
 * length) and IN buffer (armed, length). The "host" here puts a packet
 * in an OUT buffer and takes one from an armed IN buffer, as the SIE
 * would; usbd_poll() is one pass of the driver's loop.
 *
 * What the co-simulation (tb_usbd_cosim.v, COSIM_MSC) cannot reach,
 * because the Zeitlos host sends one sector a command: transfers of
 * many sectors, run through the card MSC_SECS at a time; and what a PC
 * may do that the host does not: a read past the end, a length that
 * does not match, a command it does not know, a card that fails, the
 * eject, a reset in the middle of a command.
 *
 *   make test_usbd_msc
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "zusbd.h"

uint32_t usbd_poll(void);
int usbd_state(void);
void usbd_detach(void);
void usbd_attach(uint32_t kp);
void usbd_msc_medium(int on);
int usbd_msc_ejected(void);

static int errors, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { errors++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* -- the gateware, as the CPU sees it -- */

static struct {
	int full, setup, len;
	uint8_t data[64];
} outb[8];
static struct {
	int armed, len;
	uint8_t data[64];
} inb[8];

uint32_t zu_rd(uint32_t a)
{
	uint32_t o = a - ZU_BASE, n, w = 0, i;
	if (o >= 0x400 && o < 0x800) {
		n = (o - 0x400) / 0x80;
		o = (o - 0x400) % 0x80;
		if (o < 0x40)
			for (i = 0; i < 4; i++) w |= (uint32_t)outb[n].data[o + i] << (8 * i);
		return w;
	}
	if (o >= 0x100 && o < 0x180) {
		n = (o - 0x100) / 16;
		switch ((o - 0x100) % 16) {
		case 0: return (uint32_t)outb[n].full | (uint32_t)outb[n].setup << 1 | (uint32_t)outb[n].len << 8;
		case 4: return inb[n].armed ? ZU_IN_ARMED : 0;
		default: return 0;
		}
	}
	return 0;		/* EVENTS, NSTAT, ...: nothing happening */
}

void zu_wr(uint32_t a, uint32_t v)
{
	uint32_t o = a - ZU_BASE, n, i;
	if (o >= 0x400 && o < 0x800) {
		n = (o - 0x400) / 0x80;
		o = (o - 0x400) % 0x80;
		if (o >= 0x40)
			for (i = 0; i < 4; i++) inb[n].data[o - 0x40 + i] = (uint8_t)(v >> (8 * i));
		return;
	}
	if (o >= 0x100 && o < 0x180) {
		n = (o - 0x100) / 16;
		switch ((o - 0x100) % 16) {
		case 0: outb[n].full = 0; outb[n].setup = 0; break;
		case 4: inb[n].armed = (v & ZU_IN_ARMED) != 0; inb[n].len = (int)(v & 0x7f); break;
		default: break;
		}
	}
}

/* usbd.c's console, which this does not test */
uint32_t cosim_klog_read(uint32_t *pos, uint8_t *buf, uint32_t len, uint32_t *lost)
{
	(void)pos; (void)buf; (void)len;
	if (lost) *lost = 0;
	return 0;
}
uint32_t cosim_console_input(const uint8_t *buf, uint32_t len) { (void)buf; return len; }

/* -- the card (share.c's) -- */

#define SECTORS 64
static uint8_t card[SECTORS * 512];
static int reads, writes, fail_lba = -1;
static uint32_t runs[64];
static int nruns;

uint32_t usbd_card_sectors(void) { return SECTORS; }

int usbd_card_read(uint32_t lba, uint8_t *buf, uint32_t n)
{
	if (fail_lba >= 0 && (uint32_t)fail_lba >= lba && (uint32_t)fail_lba < lba + n)
		return 1;
	memcpy(buf, card + lba * 512, n * 512);
	reads++;
	if (nruns < 64) runs[nruns++] = n;
	return 0;
}

int usbd_card_write(uint32_t lba, const uint8_t *buf, uint32_t n)
{
	memcpy(card + lba * 512, buf, n * 512);
	writes++;
	if (nruns < 64) runs[nruns++] = n;
	return 0;
}

/* -- the host -- */

#define EP_MSC 5

static void host_out(int ep, const uint8_t *p, int n, int setup)
{
	memcpy(outb[ep].data, p, (size_t)n);
	outb[ep].len = n;
	outb[ep].setup = setup;
	outb[ep].full = 1;
}

/* a control request with an IN data stage (or none): bytes received */
static int control(const uint8_t rq[8], uint8_t *in, int max)
{
	int got = 0, i;
	host_out(0, rq, 8, 1);
	for (i = 0; i < 20; i++) {
		usbd_poll();
		if (inb[0].armed) {
			int n = inb[0].len;
			if (in && got + n <= max) memcpy(in + got, inb[0].data, (size_t)n);
			got += n;
			inb[0].armed = 0;
			if (n < 64) break;
		}
	}
	usbd_poll();
	return got;
}

typedef struct {
	int status, residue, in_len, csw_ok;
} bot_t;

/* One command: CBW, its data (out[] sent, in[] taken), its CSW. */
static bot_t bot(const uint8_t *cdb, int cdb_len, uint32_t len, int dir_in,
		const uint8_t *out, uint8_t *in)
{
	static uint32_t tag = 0x1000;
	uint8_t cbw[31];
	bot_t r = { -1, -1, 0, 0 };
	uint32_t sent = 0;
	int i, csw_next = (len == 0 || !dir_in);

	memset(cbw, 0, sizeof(cbw));
	memcpy(cbw, "USBC", 4);
	tag++;
	for (i = 0; i < 4; i++) {
		cbw[4 + i] = (uint8_t)(tag >> (8 * i));
		cbw[8 + i] = (uint8_t)(len >> (8 * i));
	}
	cbw[12] = dir_in ? 0x80 : 0;
	cbw[14] = (uint8_t)cdb_len;
	memcpy(cbw + 15, cdb, (size_t)cdb_len);
	host_out(EP_MSC, cbw, 31, 0);

	for (i = 0; i < 200000; i++) {
		usbd_poll();
		if (!dir_in && sent < len && !outb[EP_MSC].full) {
			uint32_t n = len - sent < 64 ? len - sent : 64;
			host_out(EP_MSC, out + sent, (int)n, 0);
			sent += n;
			continue;
		}
		if (!inb[EP_MSC].armed)
			continue;
		if (!csw_next) {
			int n = inb[EP_MSC].len;
			memcpy(in + r.in_len, inb[EP_MSC].data, (size_t)n);
			r.in_len += n;
			inb[EP_MSC].armed = 0;
			if ((uint32_t)r.in_len >= len || n < 64)
				csw_next = 1;
			continue;
		}
		/* the CSW */
		{
			const uint8_t *c = inb[EP_MSC].data;
			uint32_t t = 0, res = 0;
			int k;
			for (k = 0; k < 4; k++) {
				t |= (uint32_t)c[4 + k] << (8 * k);
				res |= (uint32_t)c[8 + k] << (8 * k);
			}
			r.csw_ok = inb[EP_MSC].len == 13 && !memcmp(c, "USBS", 4) && t == tag;
			r.residue = (int)res;
			r.status = c[12];
			inb[EP_MSC].armed = 0;
			usbd_poll();
			return r;
		}
	}
	printf("  FAIL no CSW\n");
	errors++;
	return r;
}

static void cdb10(uint8_t *c, uint8_t op, uint32_t lba, uint32_t count)
{
	memset(c, 0, 10);
	c[0] = op;
	c[2] = (uint8_t)(lba >> 24); c[3] = (uint8_t)(lba >> 16);
	c[4] = (uint8_t)(lba >> 8); c[5] = (uint8_t)lba;
	c[7] = (uint8_t)(count >> 8); c[8] = (uint8_t)count;
}

static uint8_t sense[3];

static void request_sense(void)
{
	uint8_t c[6] = { 0x03, 0, 0, 0, 18, 0 }, b[64];
	bot_t r = bot(c, 6, 18, 1, NULL, b);
	CHECK(r.status == 0 && r.in_len == 18, "REQUEST SENSE: status %d, %d bytes", r.status, r.in_len);
	sense[0] = b[2]; sense[1] = b[12]; sense[2] = b[13];
}

static uint8_t big[64 * 512], big2[64 * 512];

int main(void)
{
	static const uint8_t set_config[8] = { 0x00, 9, 1, 0, 0, 0, 0, 0 };
	static const uint8_t max_lun[8] = { 0xA1, 0xFE, 0, 0, 4, 0, 1, 0 };
	static const uint8_t bot_reset[8] = { 0x21, 0xFF, 0, 0, 4, 0, 0, 0 };
	uint8_t b[64], c[10];
	bot_t r;
	int i, n;

	for (i = 0; i < SECTORS * 512; i++)
		card[i] = (uint8_t)(i * 7 + (i >> 9) * 13);

	printf("== card reader: Bulk-Only Transport and SCSI, against a register model ==\n");
	usbd_detach();
	usbd_msc_medium(1);
	usbd_attach(0);
	control(set_config, NULL, 0);
	CHECK(usbd_state() & 1, "configured");
	n = control(max_lun, b, sizeof(b));
	CHECK(n == 1 && b[0] == 0, "GET MAX LUN: %d bytes, %d", n, b[0]);

	/* bring-up */
	{
		uint8_t tur[6] = { 0 };
		r = bot(tur, 6, 0, 0, NULL, NULL);
		CHECK(r.csw_ok && r.status == 1, "first TEST UNIT READY fails: %d", r.status);
		request_sense();
		CHECK(sense[0] == 6 && sense[1] == 0x28, "UNIT ATTENTION, medium changed: %x/%x", sense[0], sense[1]);
		r = bot(tur, 6, 0, 0, NULL, NULL);
		CHECK(r.csw_ok && r.status == 0, "then ready: %d", r.status);
	}
	{
		uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
		r = bot(inq, 6, 36, 1, NULL, b);
		CHECK(r.status == 0 && r.in_len == 36 && b[1] == 0x80 && !memcmp(b + 8, "Machdyne", 8),
			"INQUIRY: removable, Machdyne");
		inq[4] = 5;
		r = bot(inq, 6, 5, 1, NULL, b);
		CHECK(r.status == 0 && r.in_len == 5 && r.residue == 0, "INQUIRY of 5: %d bytes", r.in_len);
	}
	{
		uint8_t cap[10] = { 0x25 };
		r = bot(cap, 10, 8, 1, NULL, b);
		CHECK(r.status == 0 && r.in_len == 8 && b[3] == SECTORS - 1 && b[6] == 2,
			"READ CAPACITY: last %d", b[3]);
	}

	/* many sectors a command, through the card MSC_SECS at a time */
	nruns = 0;
	cdb10(c, 0x28, 1, 20);
	r = bot(c, 10, 20 * 512, 1, NULL, big);
	CHECK(r.status == 0 && r.residue == 0 && r.in_len == 20 * 512, "READ 20: status %d, %d bytes", r.status, r.in_len);
	CHECK(!memcmp(big, card + 512, 20 * 512), "READ 20: the card's bytes");
	CHECK(nruns == 3 && runs[0] == 8 && runs[1] == 8 && runs[2] == 4,
		"READ 20 as 8 + 8 + 4: %d runs", nruns);

	for (i = 0; i < 17 * 512; i++) big2[i] = (uint8_t)(i * 3 + 1);
	nruns = 0;
	cdb10(c, 0x2A, 30, 17);
	r = bot(c, 10, 17 * 512, 0, big2, NULL);
	CHECK(r.status == 0 && r.residue == 0, "WRITE 17: status %d, residue %d", r.status, r.residue);
	CHECK(!memcmp(card + 30 * 512, big2, 17 * 512), "WRITE 17: on the card");
	CHECK(nruns == 3 && runs[0] == 8 && runs[1] == 8 && runs[2] == 1,
		"WRITE 17 as 8 + 8 + 1: %d runs", nruns);
	cdb10(c, 0x28, 30, 17);
	r = bot(c, 10, 17 * 512, 1, NULL, big);
	CHECK(r.status == 0 && !memcmp(big, big2, 17 * 512), "and read back");
	cdb10(c, 0x28, 0, 64);
	r = bot(c, 10, 64 * 512, 1, NULL, big);
	CHECK(r.status == 0 && !memcmp(big, card, 64 * 512), "the whole card in one READ");

	/* what a PC may send that the host does not */
	cdb10(c, 0x28, 60, 8);
	r = bot(c, 10, 8 * 512, 1, NULL, big);
	CHECK(r.csw_ok && r.status == 1 && r.in_len == 8 * 512, "READ past the end: fails, padded (%d bytes)", r.in_len);
	request_sense();
	CHECK(sense[0] == 5 && sense[1] == 0x21, "ILLEGAL REQUEST, LBA out of range: %x/%x", sense[0], sense[1]);

	cdb10(c, 0x28, 0, 2);
	r = bot(c, 10, 512, 1, NULL, big);
	CHECK(r.csw_ok && r.status == 1, "READ whose length does not match: fails");

	cdb10(c, 0x2A, 70, 1);
	r = bot(c, 10, 512, 0, big2, NULL);
	CHECK(r.csw_ok && r.status == 1, "WRITE past the end: fails, its data taken");
	memcpy(big, card + 56 * 512, 8 * 512);
	cdb10(c, 0x2A, 60, 16);
	r = bot(c, 10, 16 * 512, 0, big2, NULL);
	CHECK(r.csw_ok && r.status == 1 && r.residue == 0,
		"WRITE of 8 KB past the end: all taken and dropped (residue %d)", r.residue);
	CHECK(!memcmp(card + 56 * 512, big, 8 * 512), "and nothing written");
	cdb10(c, 0x28, 2, 1);
	r = bot(c, 10, 512, 1, NULL, big);
	CHECK(r.status == 0 && !memcmp(big, card + 2 * 512, 512), "the next command works");

	memset(c, 0, 10);
	c[0] = 0xA0;		/* REPORT LUNS: not one of these */
	r = bot(c, 10, 0, 0, NULL, NULL);
	CHECK(r.csw_ok && r.status == 1, "an unknown command fails");
	request_sense();
	CHECK(sense[0] == 5 && sense[1] == 0x20, "ILLEGAL REQUEST, invalid command: %x/%x", sense[0], sense[1]);

	cdb10(c, 0x28, 0, 0);
	r = bot(c, 10, 0, 1, NULL, big);
	CHECK(r.csw_ok && r.status == 0 && r.in_len == 0, "READ of no sectors");

	fail_lba = 12;
	cdb10(c, 0x28, 10, 10);
	r = bot(c, 10, 10 * 512, 1, NULL, big);
	CHECK(r.csw_ok && r.status == 1 && r.in_len == 10 * 512, "a card that fails: the command fails, all bytes sent");
	request_sense();
	CHECK(sense[0] == 3 && sense[1] == 0x11, "MEDIUM ERROR, unrecovered read: %x/%x", sense[0], sense[1]);
	fail_lba = -1;

	/* a reset in the middle of a long read: the next command works */
	cdb10(c, 0x28, 0, 16);
	{
		uint8_t cbw[31];
		memset(cbw, 0, sizeof(cbw));
		memcpy(cbw, "USBC", 4);
		cbw[8] = 0; cbw[9] = 0x20;		/* 8192 */
		cbw[12] = 0x80;
		cbw[14] = 10;
		memcpy(cbw + 15, c, 10);
		host_out(EP_MSC, cbw, 31, 0);
		for (i = 0; i < 5; i++) {
			usbd_poll();
			inb[EP_MSC].armed = 0;	/* a few packets taken */
		}
		control(bot_reset, NULL, 0);
		inb[EP_MSC].armed = 0;
	}
	cdb10(c, 0x28, 5, 1);
	r = bot(c, 10, 512, 1, NULL, big);
	CHECK(r.csw_ok && r.status == 0 && !memcmp(big, card + 5 * 512, 512), "after a reset, a READ works");

	/* eject */
	{
		uint8_t ssu[6] = { 0x1B, 0, 0, 0, 0x02, 0 }, tur[6] = { 0 };
		CHECK(!usbd_msc_ejected(), "not ejected yet");
		r = bot(ssu, 6, 0, 0, NULL, NULL);
		CHECK(r.csw_ok && r.status == 0, "START STOP UNIT, eject");
		CHECK(usbd_msc_ejected(), "ejected, once its CSW has gone");
		r = bot(tur, 6, 0, 0, NULL, NULL);
		CHECK(r.status == 1, "then not ready");
		request_sense();
		CHECK(sense[0] == 2 && sense[1] == 0x3A, "NOT READY, medium not present: %x/%x", sense[0], sense[1]);
		cdb10(c, 0x28, 0, 1);
		r = bot(c, 10, 512, 1, NULL, big);
		CHECK(r.csw_ok && r.status == 1 && r.in_len == 512, "a READ fails, padded");
		usbd_msc_medium(1);
		CHECK(!usbd_msc_ejected(), "shared again");
		r = bot(tur, 6, 0, 0, NULL, NULL);
		CHECK(r.status == 1, "UNIT ATTENTION again");
		r = bot(tur, 6, 0, 0, NULL, NULL);
		CHECK(r.status == 0, "then ready");
	}

	printf("%d checks, %d failed\n", checks, errors);
	if (!errors) printf("PASS\n");
	return errors ? 1 : 0;
}
