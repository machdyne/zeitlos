/*
 * Host test for relay.c: the REAL tcp.c and relay.c together, with this
 * file playing the network (segments in, segments out -- as
 * test_tcp_pool.c does) and the listening process (messages in and
 * out, through a scripted kernel).
 *
 *   sudo sysctl -w vm.mmap_min_addr=0
 *   make test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "../../../common/zeitlos.h"
#include "../../../common/zobj.h"
#include "../../../common/zport.h"
#include "../../../common/znet.h"
#include "../../../common/zproc.h"

/* -- the network -- */

typedef struct { uint32_t seq, ack; uint8_t flags; uint16_t window, len; uint8_t data[1600]; } seg_t;
static seg_t segs[128];
static int nseg;
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
bool ip_send(uint32_t ip, uint8_t proto, const uint8_t *p, uint16_t len) {
	(void)ip; (void)proto;
	seg_t *s = &segs[nseg < 127 ? nseg++ : 127];
	uint8_t off = (uint8_t)((p[12] >> 4) * 4);
	s->seq = be32(p + 4); s->ack = be32(p + 8); s->flags = p[13];
	s->window = be16(p + 14); s->len = (uint16_t)(len - off);
	memcpy(s->data, p + off, s->len);
	return true;
}
uint32_t ip_our_addr(void) { return 0xC0A80101; }
uint32_t ip_our_netmask(void) { return 0xFFFFFF00; }
static uint32_t rngs = 7;
void z_rng_bytes(void *b, uint32_t n) { uint8_t *p = b; while (n--) { rngs = rngs * 1103515245u + 12345u; *p++ = (uint8_t)(rngs >> 16); } }
bool z_rng_secure(void) { return true; }
void z_rng_stir_event(void) { }

#include "../tcp.c"
#include "../relay.c"

/* -- the listener, through a scripted kernel -- */

#define OWNER 50
#define OTHER 51
typedef struct { uint32_t to, subject, tag, type, u32; uint8_t data[1600]; uint32_t len; } msg_out_t;
static msg_out_t mo[256];
static int nmo;
static z_obj_t k_ok, k_fail;
static uint32_t dead_pid;          /* a listener the test has "killed" */
static uint32_t ticks = 1000;


static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	if (id == Z_SYS_PROC_STATUS) {
		/* every pid is running, but the one a test has killed */
		z_proc_status_args_t *a = (z_proc_status_args_t *)args;
		a->state = (a->pid == dead_pid) ? Z_PROC_STATE_EXITED : Z_PROC_STATE_RUNNING;
		return (uint32_t *)&k_ok;
	}
	if (id == Z_SYS_UPTIME) {
		((z_obj_t *)args)->type = Z_UINT32; ((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	}
	if (id == Z_SYS_MSG_SEND) {
		z_msg_t *m = (z_msg_t *)args;
		msg_out_t *o = &mo[nmo < 255 ? nmo++ : 255];
		memset(o, 0, sizeof(*o));
		o->to = m->to; o->subject = m->subject; o->tag = m->tag; o->type = m->obj.type;
		if (m->obj.type == Z_UINT32) o->u32 = m->obj.val.uint32;
		if (m->obj.type == Z_BLOB) {
			z_blob_t *bl = (z_blob_t *)m->obj.val.ptr;
			o->len = bl->len; memcpy(o->data, bl->data, bl->len);
		}
	}
	return (uint32_t *)&k_ok;
}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

static z_msg_t msg(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t o) {
	z_msg_t m;
	memset(&m, 0, sizeof(m));
	m.from = from; m.subject = subject; m.tag = tag; m.obj = o;
	return m;
}

static z_blob_t lblob[64];
static uint8_t ldata[64][1200];
static int nl;
static z_msg_t data_msg(uint32_t from, uint32_t tag, const void *d, uint32_t n) {
	int i = nl++ % 64;
	memcpy(ldata[i], d, n); lblob[i].len = n; lblob[i].data = ldata[i];
	z_obj_t o; o.type = Z_BLOB; o.val.ptr = &lblob[i];
	return msg(from, Z_PORT_DATA, tag, o);
}

static int mfind(uint32_t subject, int from_i) {
	for (int i = from_i; i < nmo; i++) if (mo[i].subject == subject) return i;
	return -1;
}

/* the peer */
#define PEER 0xC0A80109u
static void rx(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
		const void *d, uint16_t n) {
	uint8_t p[1600];
	memset(p, 0, 20);
	p[0] = (uint8_t)(sport >> 8); p[1] = (uint8_t)sport; p[2] = (uint8_t)(dport >> 8); p[3] = (uint8_t)dport;
	p[4] = (uint8_t)(seq >> 24); p[5] = (uint8_t)(seq >> 16); p[6] = (uint8_t)(seq >> 8); p[7] = (uint8_t)seq;
	p[8] = (uint8_t)(ack >> 24); p[9] = (uint8_t)(ack >> 16); p[10] = (uint8_t)(ack >> 8); p[11] = (uint8_t)ack;
	p[12] = 5 << 4; p[13] = flags; p[14] = 0x20;
	if (n) memcpy(p + 20, d, n);
	tcp_handle(PEER, p, (uint16_t)(20 + n));
}

/* The listener acks everything the relay has sent it, as netserve does,
 * and the relay gets a poll to notice. A relay waits for those acks
 * before it frees its slot (R_DRAIN). */
static void listener_acks_all(void) {
	for (int i = 0; i < RELAY_MAX; i++) {
		relay_t *r = &relays[i];
		while (r->state && r->port.pending_count) {
			z_msg_t m;
			memset(&m, 0, sizeof(m));
			m.from = r->owner; m.subject = Z_PORT_DATA_ACK; m.tag = r->port.conn_id;
			uint32_t before = r->port.pending_count;
			relay_msg(&m);
			if (r->port.pending_count == before) break;
		}
	}
	relay_poll();
}

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Brings up a connection on port 23 and returns our ISN + 1. */
static uint32_t handshake(uint16_t sport, uint32_t isn) {
	nseg = 0;
	rx(sport, 23, isn, 0, TCP_FLAG_SYN, NULL, 0);
	uint32_t s = segs[nseg - 1].seq + 1;
	rx(sport, 23, isn + 1, s, TCP_FLAG_ACK, NULL, 0);
	return s;
}

int main(void) {
	if (!k_install()) { printf("relay test: skipped\n"); return 77; }
	tcp_init(0xC0A80101);
	tcp_set_rx_window_max(5360);
	relay_init();

	/* -- listening -- */
	z_msg_t m = msg(OWNER, Z_NET_LISTEN, 23, z_obj_uint32(23));
	relay_listen(&m);
	CK(nmo == 1 && mo[0].subject == Z_NET_LISTEN_REPLY && mo[0].tag == 23 && mo[0].u32 == 0,
		"LISTEN answered with its tag, no error");
	m = msg(OWNER, Z_NET_LISTEN, 1, z_obj_uint32(70000));
	relay_listen(&m);
	CK(mo[nmo - 1].u32 == Z_NET_LISTEN_E_PORT, "an impossible port refused");

	/* -- an accepted connection is offered -- */
	int mark = nmo;
	nseg = 0;
	rx(4000, 23, 100, 0, TCP_FLAG_SYN, NULL, 0);
	CK(nseg == 1 && segs[0].flags == (TCP_FLAG_SYN | TCP_FLAG_ACK) && segs[0].window == RELAY_RX,
		"the SYN-ACK already advertises only what the relay holds (%u)", segs[0].window);
	uint32_t ours = segs[0].seq + 1;
	CK(mfind(Z_PORT_CONNECT, mark) < 0, "nothing offered before the handshake completes");
	rx(4000, 23, 101, ours, TCP_FLAG_ACK | TCP_FLAG_PSH, "early", 5);
	int c = mfind(Z_PORT_CONNECT, mark);
	CK(c >= 0 && mo[c].to == OWNER && mo[c].type == Z_BLOB, "CONNECT offered to the listener");
	z_net_accept_t info;
	memcpy(&info, mo[c].data, sizeof(info));
	CK(info.ip == PEER && info.port == 4000 && info.lport == 23 && (info.flags & Z_NET_ACCEPT_LOCAL),
		"with the peer, the port and 'local'");
	uint32_t rid = mo[c].tag;
	relay_poll();
	CK(mfind(Z_PORT_DATA, mark) < 0, "no DATA before the listener answers");

	/* -- the listener answers -- */
	m = msg(OWNER, Z_PORT_CONNECTED, rid, z_obj_uint32(77));
	CK(relay_msg(&m), "the answer is the relay's");
	mark = nmo;
	relay_poll();
	c = mfind(Z_PORT_DATA, mark);
	CK(c >= 0 && mo[c].tag == 77 && mo[c].len == 5 && !memcmp(mo[c].data, "early", 5),
		"what arrived early goes to the listener, tagged with its conn_id");
	m = msg(OWNER, Z_PORT_DATA_ACK, 77, z_obj_none());
	relay_msg(&m);

	/* -- listener -> peer -- */
	nseg = 0;
	mark = nmo;
	m = data_msg(OWNER, 77, "login: ", 7);
	CK(relay_msg(&m), "DATA from the listener handled");
	CK(mfind(Z_PORT_DATA_ACK, mark) >= 0, "and acked");
	relay_poll();
	CK(nseg >= 1 && segs[nseg - 1].len == 7 && !memcmp(segs[nseg - 1].data, "login: ", 7),
		"sent to the peer");
	rx(4000, 23, 106, ours + 7, TCP_FLAG_ACK, NULL, 0);

	/* -- backpressure: more than tx[] holds -- */
	mark = nmo;
	uint8_t big[600];
	memset(big, 'q', sizeof(big));
	for (int i = 0; i < 3; i++) { m = data_msg(OWNER, 77, big, sizeof(big)); relay_msg(&m); }
	int acks = 0;
	for (int i = mark; i < nmo; i++) if (mo[i].subject == Z_PORT_DATA_ACK) acks++;
	CK(acks == 1, "only what fits is acked; the rest held (%d)", acks);
	uint32_t sent = 0, seq = ours + 7;
	for (int round = 0; round < 20; round++) {
		nseg = 0;
		relay_poll();
		for (int i = 0; i < nseg; i++) if (segs[i].len) { sent += segs[i].len; seq = segs[i].seq + segs[i].len; }
		rx(4000, 23, 106, seq, TCP_FLAG_ACK, NULL, 0);
	}
	acks = 0;
	for (int i = mark; i < nmo; i++) if (mo[i].subject == Z_PORT_DATA_ACK) acks++;
	CK(sent == 1800 && acks == 3, "all 1800 bytes went out and every DATA was acked (%u, %d)", sent, acks);

	/* -- one message larger than tx[]: consumed in pieces -- */
	{
		static uint8_t huge[2600];
		for (int i = 0; i < (int)sizeof(huge); i++) huge[i] = (uint8_t)(i * 7);
		mark = nmo;
		m = data_msg(OWNER, 77, huge, sizeof(huge));
		relay_msg(&m);
		static uint8_t got[4000];
		uint32_t gl = 0;
		for (int round = 0; round < 30; round++) {
			nseg = 0;
			relay_poll();
			for (int i = 0; i < nseg; i++) if (segs[i].len) {
				memcpy(got + gl, segs[i].data, segs[i].len); gl += segs[i].len;
				seq = segs[i].seq + segs[i].len;
			}
			rx(4000, 23, 106, seq, TCP_FLAG_ACK, NULL, 0);
		}
		int a = 0;
		for (int i = mark; i < nmo; i++) if (mo[i].subject == Z_PORT_DATA_ACK) a++;
		CK(gl == sizeof(huge) && !memcmp(got, huge, sizeof(huge)) && a == 1,
			"a 2600-byte message (tx[] is 1024) goes out whole, acked once (%u, %d)", gl, a);
	}

	/* -- peer -> listener, with the listener not acking: the window closes -- */
	uint32_t pseq = 106;
	for (int i = 0; i < 24; i++) {
		rx(4000, 23, pseq, seq, TCP_FLAG_ACK, "0123456789012345678901234567890123456789", 40);
		pseq += 40;
		relay_poll();
	}
	nseg = 0;
	relay_poll();
	tcp_ack_now(relays[rid - 1].tcp);
	CK(segs[nseg - 1].window == 0, "the window closes while the listener is behind (%u)",
		segs[nseg - 1].window);
	mark = nmo;
	for (int i = 0; i < 12; i++) { m = msg(OWNER, Z_PORT_DATA_ACK, 77, z_obj_none()); relay_msg(&m); }
	nseg = 0;
	relay_poll(); relay_poll();
	bool reopened = false;
	for (int i = 0; i < nseg; i++) if (segs[i].window == RELAY_RX) reopened = true;
	CK(reopened, "and reopens, announced, once it catches up");

	/* -- the listener closes: flush (several segments), then FIN -- */
	{
		uint8_t last[900];
		memset(last, 'L', sizeof(last));
		m = data_msg(OWNER, 77, last, sizeof(last));
		relay_msg(&m);
		m = msg(OWNER, Z_PORT_CLOSE, 77, z_obj_none());
		relay_msg(&m);
		uint32_t got = 0, s2 = 0;
		bool fin_early = false, fin = false;
		for (int round = 0; round < 10 && !fin; round++) {
			nseg = 0;
			relay_poll();
			for (int i = 0; i < nseg; i++) {
				if (segs[i].flags & TCP_FLAG_FIN) { fin = true; if (got < sizeof(last)) fin_early = true; s2 = segs[i].seq + 1; }
				else if (segs[i].len) { got += segs[i].len; s2 = segs[i].seq + segs[i].len; }
			}
			if (!fin) rx(4000, 23, pseq, s2, TCP_FLAG_ACK, NULL, 0);
		}
		CK(got == sizeof(last) && fin && !fin_early, "all 900 bytes out before the FIN (%u)", got);
		rx(4000, 23, pseq, s2, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
		listener_acks_all();
		CK(relay_count() == 0, "the relay is freed when TCP is done");
	}

	/* -- the peer half-closes: the listener hears EOF, answers, then closes -- */
	{
		uint32_t o2 = handshake(4001, 500);
		rid = mo[mfind(Z_PORT_CONNECT, nmo - 1)].tag;
		m = msg(OWNER, Z_PORT_CONNECTED, rid, z_obj_uint32(78));
		relay_msg(&m);
		mark = nmo;
		rx(4001, 23, 501, o2, TCP_FLAG_ACK | TCP_FLAG_PSH | TCP_FLAG_FIN, "last", 4);
		relay_poll();
		int d = mfind(Z_PORT_DATA, mark), eof = mfind(Z_NET_EOF, mark);
		CK(d >= 0 && eof > d && mo[eof].tag == 78 && mfind(Z_PORT_CLOSE, mark) < 0,
			"the peer's FIN: its last bytes, then Z_NET_EOF -- not CLOSE");
		CK(relay_count() == 1, "the relay stays: the answer has still to go out");
		m = msg(OWNER, Z_PORT_DATA_ACK, 78, z_obj_none());
		relay_msg(&m);
		nseg = 0;
		m = data_msg(OWNER, 78, "reply", 5);
		relay_msg(&m);
		m = msg(OWNER, Z_PORT_CLOSE, 78, z_obj_none());
		relay_msg(&m);
		relay_poll();
		CK(nseg >= 1 && segs[0].len == 5 && !memcmp(segs[0].data, "reply", 5),
			"the answer goes out after the peer's FIN");
		rx(4001, 23, 506, segs[0].seq + 5, TCP_FLAG_ACK, NULL, 0);
		nseg = 0;
		relay_poll();
		CK(nseg == 1 && (segs[0].flags & TCP_FLAG_FIN), "then our FIN");
		rx(4001, 23, 506, segs[0].seq + 1, TCP_FLAG_ACK, NULL, 0);
		listener_acks_all();
		CK(relay_count() == 0, "and the relay is freed when it is acked");
	}

	/* -- refused: RST -- */
	handshake(4002, 900);
	rid = mo[nmo - 1].tag;
	nseg = 0;
	m = msg(OWNER, Z_PORT_REFUSED, rid, z_obj_none());
	relay_msg(&m);
	listener_acks_all();
	CK(nseg == 1 && (segs[0].flags & TCP_FLAG_RST) && relay_count() == 0, "REFUSED: RST, relay freed");

	/* -- the peer leaves before the listener answers -- */
	handshake(4003, 1300);
	rid = mo[nmo - 1].tag;
	rx(4003, 23, 1301, 0, TCP_FLAG_RST, NULL, 0);
	CK(relay_count() == 1, "offered and gone: the relay waits for the answer");
	mark = nmo;
	m = msg(OWNER, Z_PORT_CONNECTED, rid, z_obj_uint32(79));
	relay_msg(&m);
	c = mfind(Z_PORT_CLOSE, mark);
	CK(c >= 0 && mo[c].tag == 79 && relay_count() == 0, "then CLOSE at once, and freed");

	/* -- a relay whose listener has not acked everything waits for it -- */
	{
		uint32_t o3 = handshake(4010, 3300);
		rid = mo[mfind(Z_PORT_CONNECT, nmo - 1)].tag;
		m = msg(OWNER, Z_PORT_CONNECTED, rid, z_obj_uint32(90));
		relay_msg(&m);
		rx(4010, 23, 3301, o3, TCP_FLAG_ACK, "unacked", 7);
		relay_poll();                                   /* sent to the listener, not acked */
		rx(4010, 23, 3308, o3, TCP_FLAG_RST, NULL, 0);  /* and the peer resets */
		CK(relay_count() == 1 && relays[rid - 1].state == R_DRAIN,
			"TCP gone, a send still unacked: the relay drains, it is not freed");
		m = msg(OWNER, Z_PORT_DATA_ACK, 90, z_obj_none());
		relay_msg(&m);
		relay_poll();
		CK(relay_count() == 0, "freed once acked");
	}

	/* -- messages from a process that is not a listener are not ours -- */
	m = msg(99, Z_PORT_DATA, 77, z_obj_none());
	CK(!relay_msg(&m), "someone else's DATA falls through to net's own handlers");

	/* -- a takeover closes the old owner's connections -- */
	handshake(4004, 1700);
	rid = mo[nmo - 1].tag;
	m = msg(OWNER, Z_PORT_CONNECTED, rid, z_obj_uint32(80));
	relay_msg(&m);
	nseg = 0;
	m = msg(OTHER, Z_NET_LISTEN, 23, z_obj_uint32(23));
	relay_listen(&m);
	CK(relay_count() == 0 && nseg >= 1 && (segs[nseg - 1].flags & TCP_FLAG_RST),
		"a new listener on 23: the old owner's connection is reset");
	mark = nmo;
	handshake(4005, 2100);
	c = mfind(Z_PORT_CONNECT, mark);
	CK(c >= 0 && mo[c].to == OTHER, "and new ones go to the new owner");

	/* -- the listener dies mid-session -- */
	{
		/* a clean slate the way it happens for real: OTHER, which took
		 * port 23 over above, dies, and net cleans up after it */
		dead_pid = OTHER;
		ticks += 2 * Z_TICK_HZ;
		relay_poll();
		dead_pid = 0;
		listener_acks_all();
		CK(relay_count() == 0, "a dead listener's connections all go (%d left)", relay_count());
		m = msg(OWNER, Z_NET_LISTEN, 23, z_obj_uint32(23));
		relay_listen(&m);
		CK(mo[nmo - 1].u32 == 0, "its port is free for the next listener");
		uint32_t o5 = handshake(4100, 7000);
		c = mfind(Z_PORT_CONNECT, nmo - 1);
		if (c < 0) { printf("FAIL: no connection offered -- stopping\n"); return 1; }
		rid = mo[c].tag;
		m = msg(OWNER, Z_PORT_CONNECTED, rid, z_obj_uint32(91));
		relay_msg(&m);
		rx(4100, 23, 7001, o5, TCP_FLAG_ACK | TCP_FLAG_PSH, "typed", 5);
		relay_poll();                                   /* sent to the listener, never acked */
		CK(relays[rid - 1].port.pending_count == 1, "(a send out to the listener)");
		dead_pid = OWNER;                               /* killed */
		ticks += 2 * Z_TICK_HZ;
		nseg = 0;
		relay_poll();
		bool rst = false;
		for (int i = 0; i < nseg; i++) if (segs[i].flags & TCP_FLAG_RST) rst = true;
		CK(rst && relay_count() == 0, "its connection is reset at once, the relay freed");
		nseg = 0;
		rx(4101, 23, 8000, 0, TCP_FLAG_SYN, NULL, 0);
		CK(nseg == 1 && (segs[0].flags & TCP_FLAG_RST), "and its port no longer listens");
		dead_pid = 0;
	}

	/* -- a listener restarted under its old pid: its old connections go -- */
	{
		m = msg(OWNER, Z_NET_LISTEN, 23, z_obj_uint32(23));
		relay_listen(&m);
		uint32_t o6 = handshake(4200, 9000);
		(void)o6;
		c = mfind(Z_PORT_CONNECT, nmo - 1);
		if (c < 0) { printf("FAIL: no connection offered -- stopping\n"); return 1; }
		rid = mo[c].tag;
		m = msg(OWNER, Z_PORT_CONNECTED, rid, z_obj_uint32(92));
		relay_msg(&m);
		CK(relay_count() == 1, "(a session)");
		nseg = 0;
		m = msg(OWNER, Z_NET_LISTEN, 23, z_obj_uint32(23));     /* the same pid asks again */
		relay_listen(&m);
		CK(relay_count() == 0 && nseg >= 1 && (segs[nseg - 1].flags & TCP_FLAG_RST),
			"the same pid listening again: its old connection is reset");
		CK(mo[nmo - 1].subject == Z_NET_LISTEN_REPLY && mo[nmo - 1].u32 == 0, "and the port is its again");
	}

	/* -- no relay free: RST -- */
	for (int i = 0; i < RELAY_MAX; i++) handshake((uint16_t)(5000 + i), 3000u + 100u * (uint32_t)i);
	nseg = 0;
	rx(6000, 23, 9000, 0, TCP_FLAG_SYN, NULL, 0);
	CK(nseg == 1 && (segs[0].flags & TCP_FLAG_RST), "every relay busy: the next SYN gets a RST");

	printf("relay test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
