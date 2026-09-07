/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for tcp.c's out-of-order reassembly.
 *
 *   make test
 *
 * INCLUDES tcp.c directly, with the platform stubbed out, so what is
 * tested is the code that ships rather than a copy of it. The
 * reassembly path is the one part of this stack that can be exercised
 * without hardware -- it is sequence arithmetic over a buffer and
 * nothing else.
 *
 * It is worth testing because it is where a mistake is silent. A
 * reassembler that drops data makes a page load slow; one that
 * SPLICES data in the wrong order corrupts a TLS record, and the
 * error surfaces four layers away as a decryption failure. This
 * session already spent several rounds chasing exactly that shape of
 * bug from the other end.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -- platform stubs -- */
static uint8_t sent[65536]; static int nsent;
bool ip_send(uint32_t ip, uint8_t proto, const uint8_t *p, uint16_t len) {
	(void)ip;(void)proto;(void)p; nsent++; sent[0]=len&0xff; return true; }
uint32_t z_uptime_ticks(void) { return 0; }
void z_rng_bytes(void *b, uint32_t n) { memset(b, 0x5a, n); }
bool z_rng_secure(void) { return true; }
void z_rng_stir_event(void) { }

#define TCP_REASSEMBLY 1
#include "../tcp.c"

/* -- what the listener saw -- */
static uint8_t got[300000]; static uint32_t got_len;
static void sink(tcp_event_t ev, const uint8_t *d, uint16_t n) {
	if (ev == TCP_EVENT_DATA && got_len + n <= sizeof(got)) {
		memcpy(got + got_len, d, n); got_len += n; }
}

static int fails, checks;
static void ck(int c, const char *w) {
	checks++; if (!c) { fails++; printf("FAIL: %s\n", w); } }

/* Feed a segment as if it had arrived. */
static void seg(uint32_t seq, const uint8_t *d, uint16_t n) {
	if (seq == tcb.rcv_nxt) {
		tcb.rcv_nxt += n;
		notify(TCP_EVENT_DATA, d, n);
		ooo_drain();
	} else if ((int32_t)(seq - tcb.rcv_nxt) < 0) {
		/* duplicate */
	} else {
		ooo_store(seq, d, n);
	}
}

int main(void) {
	static uint8_t src[4096];
	for (unsigned i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 31 + 7);

	tcb.state = TCP_ESTABLISHED;
	tcb.handler = sink;

	/* -- the case that matters: one lost segment, rest arrive early -- */
	tcb.rcv_nxt = 1000; got_len = 0; ooo_reset();
	seg(1000, src,        100);          /* in order            */
	seg(1200, src + 200,  100);          /* early: 1100 missing */
	seg(1300, src + 300,  100);          /* early, contiguous   */
	ck(got_len == 100, "held back until the hole is filled");
	seg(1100, src + 100,  100);          /* the retransmit      */
	ck(got_len == 400, "hole filled releases everything behind it");
	ck(!memcmp(got, src, 400), "reassembled bytes are in order");
	ck(tcb.rcv_nxt == 1400, "rcv_nxt advanced past the run");

	/* -- a duplicate of already-held data must not corrupt the run -- */
	tcb.rcv_nxt = 0; got_len = 0; ooo_reset();
	seg(0,   src,        50);
	seg(100, src + 100,  50);
	seg(100, src + 100,  50);            /* same again */
	seg(50,  src + 50,   50);
	ck(got_len == 150, "duplicate early segment does not duplicate data");
	ck(!memcmp(got, src, 150), "bytes still in order after a duplicate");

	/* -- a second, separate gap is dropped, not mis-filed -- */
	tcb.rcv_nxt = 0; got_len = 0; ooo_reset();
	seg(0,   src,        50);
	seg(100, src + 100,  50);            /* held */
	seg(300, src + 300,  50);            /* second hole: must be dropped */
	seg(50,  src + 50,   50);
	ck(got_len == 150, "second gap dropped rather than mis-assembled");
	ck(!memcmp(got, src, 150), "no foreign bytes spliced in");

	/* -- oversized run is refused rather than overrunning -- */
	tcb.rcv_nxt = 0; got_len = 0; ooo_reset();
	for (uint32_t off = 100; off < TCP_OOO_BUF + 4096; off += 100)
		seg(off, src + (off % 2048), 100);
	ck(ooo_len <= TCP_OOO_BUF, "held run never exceeds the buffer");

	printf("%s: %d checks, %d failures\n", fails ? "FAIL":"ok", checks, fails);
	return fails ? 1 : 0;
}
