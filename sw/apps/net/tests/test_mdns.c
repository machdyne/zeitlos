/*
 * Zeitlos
 *
 * Host test for mdns.c: the questions it sends, and what it makes of
 * answers -- compressed names, records in any order, the follow-up for
 * a missing SRV or address, names with dots inside a label, malformed
 * packets, and the reply's text.
 *
 *   make test   (or: cc -std=gnu99 -Wall -I. -I../../common \
 *                  -o /tmp/t tests/test_mdns.c && /tmp/t)
 *
 * INCLUDES mdns.c, with udp/ip/messaging stubbed: the test plays the
 * responders (feeds packets to the handler udp_listen() was given) and
 * the requester (reads the Z_NET_BROWSE_REPLY).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/znet.h"
#include "udp.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* -- stubs -- */

static udp_handler_t handler;
static uint16_t listen_port;
static bool listening;
static uint32_t now;
static uint32_t our_ip = 0x0A000002;

static uint8_t sent[8][600];
static int sent_len[8], nsent;
static uint32_t sent_ip;
static uint16_t sent_dport;

static int nreplies;
static uint32_t reply_tag;
static bool reply_ok;
static char reply_text[4096];

uint32_t z_uptime_ticks(void) { return now; }
uint32_t ip_our_addr(void) { return our_ip; }
void udp_listen(uint16_t port, udp_handler_t h) { listen_port = port; handler = h; listening = true; }
void udp_close(uint16_t port) { if (port == listen_port) listening = false; }
bool udp_send(uint32_t ip, uint16_t dport, uint16_t sport, const uint8_t *p, uint16_t len) {
	(void)sport;
	sent_ip = ip;
	sent_dport = dport;
	if (nsent < 8) { memcpy(sent[nsent], p, len); sent_len[nsent] = len; }
	nsent++;
	return true;
}
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	(void)to;
	if (subject != Z_NET_BROWSE_REPLY || obj.type != Z_MAP) return Z_OK;
	z_obj_table_t *t = obj.val.ptr;
	nreplies++;
	reply_tag = tag;
	reply_ok = t->b[0].val.uint32 == 1;
	snprintf(reply_text, sizeof(reply_text), "%s", t->b[1].val.str);
	return Z_OK;
}

#include "../mdns.c"

/* -- building answers -- */

static uint8_t pk[4096];		// the full house below is bigger than a frame: fine here
static int pl;
static int nrr;

static void begin(uint16_t id) {
	memset(pk, 0, 12);
	pk[0] = (uint8_t)(id >> 8); pk[1] = (uint8_t)id;
	pk[2] = 0x84;			// QR, AA
	pl = 12;
	nrr = 0;
}

// a dotted name, or "NAME@OFF": the labels in NAME, then a pointer to OFF
static int put_name(const char *s) {
	int at = pl;
	const char *ptr = strchr(s, '@');
	const char *end = ptr ? ptr : s + strlen(s);
	while (s < end) {
		const char *e = s;
		while (e < end && *e != '.') e++;
		pk[pl++] = (uint8_t)(e - s);
		memcpy(pk + pl, s, (size_t)(e - s));
		pl += (int)(e - s);
		s = e < end ? e + 1 : e;
	}
	if (ptr) {
		int off = 0;
		for (ptr++; *ptr; ptr++) off = off * 10 + (*ptr - '0');
		pk[pl++] = (uint8_t)(0xC0 | (off >> 8));
		pk[pl++] = (uint8_t)off;
	} else pk[pl++] = 0;
	return at;
}

// a label holding a dot, as a responder sends it
static int put_raw_label_name(const char *label, const char *rest) {
	int at = pl;
	int l = (int)strlen(label);
	pk[pl++] = (uint8_t)l;
	memcpy(pk + pl, label, (size_t)l);
	pl += l;
	put_name(rest);
	return at;
}

static int rr_head(uint16_t type, uint16_t cls) {
	pk[pl++] = (uint8_t)(type >> 8); pk[pl++] = (uint8_t)type;
	pk[pl++] = (uint8_t)(cls >> 8); pk[pl++] = (uint8_t)cls;
	pk[pl++] = 0; pk[pl++] = 0; pk[pl++] = 0x11; pk[pl++] = 0x94;	// TTL 4500
	int at = pl;
	pl += 2;
	nrr++;
	return at;			// where RDLENGTH goes
}

static void rr_end(int at) {
	int n = pl - at - 2;
	pk[at] = (uint8_t)(n >> 8); pk[at + 1] = (uint8_t)n;
}

static int ptr_rr(const char *name, const char *target) {
	put_name(name);
	int a = rr_head(T_PTR, 1);
	int t = pl;
	put_name(target);
	rr_end(a);
	return t;
}

static void srv_rr(const char *name, uint16_t port, const char *target) {
	put_name(name);
	int a = rr_head(T_SRV, 0x8001);
	pk[pl++] = 0; pk[pl++] = 0; pk[pl++] = 0; pk[pl++] = 0;
	pk[pl++] = (uint8_t)(port >> 8); pk[pl++] = (uint8_t)port;
	put_name(target);
	rr_end(a);
}

static void txt_rr(const char *name, const char *const *strs, int n) {
	put_name(name);
	int a = rr_head(T_TXT, 0x8001);
	for (int i = 0; i < n; i++) {
		int l = (int)strlen(strs[i]);
		pk[pl++] = (uint8_t)l;
		memcpy(pk + pl, strs[i], (size_t)l);
		pl += l;
	}
	rr_end(a);
}

static void a_rr(const char *name, uint32_t ip) {
	put_name(name);
	int a = rr_head(T_A, 0x8001);
	pk[pl++] = (uint8_t)(ip >> 24); pk[pl++] = (uint8_t)(ip >> 16);
	pk[pl++] = (uint8_t)(ip >> 8); pk[pl++] = (uint8_t)ip;
	rr_end(a);
}

static void deliver(int an) {
	pk[6] = (uint8_t)(an >> 8); pk[7] = (uint8_t)an;
	pk[10] = (uint8_t)((nrr - an) >> 8); pk[11] = (uint8_t)(nrr - an);
	if (listening) handler(0x0A000005, 5353, pk, (uint16_t)pl);
}

static uint16_t qid(void) { return (uint16_t)((sent[0][0] << 8) | sent[0][1]); }

// runs mdns_poll() until `ms` after the browse began
static void run_to(int ms) {
	while (now * 1000u / Z_TICK_HZ < (uint32_t)ms) {
		now += Z_TICK_HZ / 100;
		mdns_poll();
	}
}

static void start(const char *svcs) {
	nsent = 0;
	nreplies = 0;
	now = 1000;
	uint32_t t = now;
	mdns_browse_start(svcs, 7, 42);
	now = t;
	now = 0;			// times below are from the start
	b.t0 = 0;
}

// the question count and the n'th question's type in sent packet k
static int qcount(int k) { return (sent[k][4] << 8) | sent[k][5]; }

static bool has_question(int k, const char *dotted, uint16_t type) {
	wname_t want;
	int n = (int)strlen(dotted);
	// dotted is "x.y" without .local: name_from_text adds it
	if (!name_from_text(&want, dotted, n)) return false;
	uint16_t off = 12;
	for (int q = 0; q < qcount(k); q++) {
		wname_t got;
		off = read_name(sent[k], (uint16_t)sent_len[k], off, &got);
		if (!off) return false;
		uint16_t t = (uint16_t)((sent[k][off] << 8) | sent[k][off + 1]);
		off += 4;
		if (t == type && name_eq(&got, &want)) return true;
	}
	return false;
}

int main(void) {

	// -- the question --
	start("_ipp._tcp _pdl-datastream._tcp");
	CHECK(nsent == 1);
	CHECK(sent_ip == 0xE00000FB && sent_dport == 5353);
	CHECK(listening && listen_port != 5353);		// legacy unicast: not from 5353
	CHECK(qcount(0) == 2);
	CHECK(has_question(0, "_ipp._tcp", T_PTR));
	CHECK(has_question(0, "_pdl-datastream._tcp", T_PTR));
	CHECK(sent[0][2] == 0 && sent[0][3] == 0);		// a plain query

	// -- one printer, everything in one compressed answer, as most send --
	begin(qid());
	pk[5] = 1;
	put_name("_ipp._tcp.local");				// the question echoed, at 12
	pk[pl++] = 0; pk[pl++] = T_PTR; pk[pl++] = 0; pk[pl++] = 1;
	{
		char n[16];
		snprintf(n, sizeof(n), "@%d", ptr_rr("@12", "Canon MG3600 series@12"));
		srv_rr(n, 631, "Canon-MG3600.local");
		static const char *const tx[] = { "txtvers=1", "rp=ipp/print", "ty=Canon MG3600 series", "pdl=application/pdf,image/pwg-raster" };
		txt_rr(n, tx, 4);
		a_rr("Canon-MG3600.local", 0x0A000005);
	}
	deliver(1);
	CHECK(b.nents == 1 && complete(&b.ents[0]));
	run_to(1250);
	CHECK(nreplies == 1 && reply_ok && reply_tag == 42);
	// TXT: only what a list of printers uses (rp= ty= product= note=)
	CHECK(!strcmp(reply_text, "_ipp._tcp\tCanon MG3600 series\t10.0.0.5\t631\trp=ipp/print\tty=Canon MG3600 series\n"));
	CHECK(!listening);
	run_to(3000);
	CHECK(nreplies == 1);					// once only

	// -- records out of order, SRV missing until asked --
	start("_ipp._tcp");
	begin(qid());
	a_rr("brother.local", 0xC0A80117);			// before anything names it
	ptr_rr("_ipp._tcp.local", "Brother HL-L2350DW._ipp._tcp.local");
	deliver(2);
	CHECK(b.nents == 1 && !b.ents[0].have_srv);
	run_to(350);
	CHECK(nsent == 2 && has_question(1, "Brother HL-L2350DW._ipp._tcp", T_SRV));
	CHECK(has_question(1, "Brother HL-L2350DW._ipp._tcp", T_TXT));
	CHECK(has_question(1, "_ipp._tcp", T_PTR));		// the second PTR send
	begin(qid());
	srv_rr("Brother HL-L2350DW._ipp._tcp.local", 631, "brother.local");
	deliver(1);
	CHECK(b.ents[0].have_srv && !b.ents[0].ip);		// the early A was for nobody yet
	run_to(650);
	CHECK(nsent == 3 && has_question(2, "brother", T_A));
	begin(qid());
	a_rr("BROTHER.local", 0xC0A80117);			// names compare without case
	deliver(1);
	CHECK(b.ents[0].ip == 0xC0A80117);
	run_to(1850);
	CHECK(nreplies == 1 && reply_ok);
	CHECK(!strcmp(reply_text, "_ipp._tcp\tBrother HL-L2350DW\t192.168.1.23\t631\n"));	// no TXT: still listed

	// -- a dot inside the instance label, a tab in a TXT string, and one
	//    that never gives an address (left out) --
	start("_ipp._tcp");
	begin(qid());
	{
		put_name("_ipp._tcp.local");
		int a = rr_head(T_PTR, 1);
		int t = put_raw_label_name("HP LaserJet 400 (1.2.3)", "_ipp._tcp.local");
		rr_end(a);
		char n[16];
		snprintf(n, sizeof(n), "@%d", t);
		srv_rr(n, 8631, "hp.local");
		static const char *const tx[] = { "note=2nd\tfloor" };
		txt_rr(n, tx, 1);
		a_rr("hp.local", 0x0A00000B);
		ptr_rr("_ipp._tcp.local", "Ghost._ipp._tcp.local");
	}
	deliver(nrr);
	run_to(1850);
	CHECK(nreplies == 1 && reply_ok);
	CHECK(!strcmp(reply_text, "_ipp._tcp\tHP LaserJet 400 (1.2.3)\t10.0.0.11\t8631\tnote=2nd floor\n"));

	// -- what is ignored: the wrong id, a query, a source port not 5353,
	//    pointer loops, truncation, a PTR for another service --
	start("_ipp._tcp");
	begin((uint16_t)(qid() + 1));
	ptr_rr("_ipp._tcp.local", "A._ipp._tcp.local");
	deliver(1);
	CHECK(b.nents == 0);
	begin(qid());
	pk[2] = 0;
	ptr_rr("_ipp._tcp.local", "A._ipp._tcp.local");
	deliver(1);
	CHECK(b.nents == 0);
	begin(qid());
	ptr_rr("_ipp._tcp.local", "A._ipp._tcp.local");
	pk[6] = 0; pk[7] = 1;
	handler(0x0A000005, 5354, pk, (uint16_t)pl);
	CHECK(b.nents == 0);
	begin(qid());
	{
		put_name("_ipp._tcp.local");
		int a = rr_head(T_PTR, 1);
		int t = pl;
		pk[pl++] = 0xC0; pk[pl++] = (uint8_t)t;		// a target pointing at itself
		rr_end(a);
	}
	deliver(1);
	CHECK(b.nents == 0);
	begin(qid());
	ptr_rr("_printer._tcp.local", "C._printer._tcp.local");
	ptr_rr("_ipp._tcp.local", "_ipp._tcp.local");	// no instance label
	deliver(2);
	CHECK(b.nents == 0);
	begin(qid());
	ptr_rr("_ipp._tcp.local", "D._ipp._tcp.local");
	for (int cut = 12; cut < pl; cut++) {
		pk[7] = 1;
		handler(0x0A000005, 5353, pk, (uint16_t)cut);	// every truncation
	}
	CHECK(b.nents == 0);
	run_to(1850);
	CHECK(nreplies == 1 && reply_ok && reply_text[0] == 0);

	// -- refusals --
	start("_ipp._tcp");
	nreplies = 0;
	mdns_browse_start("_ipp._tcp", 9, 43);
	CHECK(nreplies == 1 && !reply_ok && reply_tag == 43);	// busy
	run_to(1850);
	CHECK(nreplies == 2 && reply_ok);
	nreplies = 0;
	mdns_browse_start("", 9, 44);
	CHECK(nreplies == 1 && !reply_ok);
	mdns_browse_start(NULL, 9, 45);
	CHECK(nreplies == 2 && !reply_ok);
	our_ip = 0;
	mdns_browse_start("_ipp._tcp", 9, 46);
	CHECK(nreplies == 3 && !reply_ok && !b.busy);
	our_ip = 0x0A000002;

	// -- a full house: eight kept, the reply fits --
	start("_ipp._tcp");
	begin(qid());
	for (int i = 0; i < 10; i++) {
		char n[64], h[32];
		snprintf(n, sizeof(n), "Printer number %d with a long name._ipp._tcp.local", i);
		snprintf(h, sizeof(h), "p%d.local", i);
		ptr_rr("_ipp._tcp.local", n);
		srv_rr(n, 631, h);
		a_rr(h, 0x0A000100u + (uint32_t)i);
	}
	deliver(10);
	CHECK(b.nents == MAX_ENTS);			// the rest left out
	run_to(1850);
	{
		int lines = 0;
		for (char *p = reply_text; *p; p++) lines += *p == '\n';
		CHECK(lines == MAX_ENTS);
	}

	printf("test_mdns: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
