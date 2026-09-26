/*
 * End to end: a TCP peer (telnet), the REAL tcp.c and relay.c (net),
 * the REAL netserve.c, and a scripted backend playing posix -- all in
 * one process, with a scripted kernel carrying messages between them
 * by pid. The unit tests check each half; this checks the hand-off
 * between them under real flow control: net withholding acks while
 * TCP drains one segment per round trip, netserve hitting zport's
 * limit of 8 unacked sends.
 *
 *   make test   (see ../Makefile)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "../../../../common/zeitlos.h"
#include "../../../../common/zobj.h"
#include "../../../../common/zport.h"
#include "../../../../common/znet.h"
#include "../../../../common/zcfg.h"
#include "../../../../common/zauth.h"
#include "../../tcp.h"
#include "../../relay.h"

#define NET   10
#define NS    30
#define PX    21
#define PEER  0xC0A80109u
#define US    0xC0A80101u

// -- netserve, from the other translation unit --
void ns_init(void);
void ns_step(void);

// -- the network, as tcp.c sees it --
bool ip_send(uint32_t ip, uint8_t proto, const uint8_t *p, uint16_t len);
uint32_t ip_our_addr(void) { return US; }
uint32_t ip_our_netmask(void) { return 0xFFFFFF00; }
/* z_rng_*: tests/fake_fs.c */

typedef struct { uint32_t seq, ack; uint8_t flags; uint16_t win, len; uint8_t data[1600]; } seg_t;
static seg_t wire[256];
static int nwire;
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
bool ip_send(uint32_t ip, uint8_t proto, const uint8_t *p, uint16_t len) {
	(void)ip; (void)proto;
	if (nwire >= 256) return true;
	seg_t *s = &wire[nwire++];
	uint8_t off = (uint8_t)((p[12] >> 4) * 4);
	s->seq = be32(p + 4); s->ack = be32(p + 8); s->flags = p[13];
	s->win = be16(p + 14);
	s->len = (uint16_t)(len - off);
	memcpy(s->data, p + off, s->len);
	(void)be16;
	return true;
}

// -- the kernel: one mailbox per pid --
typedef struct { z_msg_t m[512]; int n; } box_t;
static box_t box_net, box_ns, box_px;
static uint32_t current = NET;
static uint32_t ticks = 1000;
static z_obj_t k_ok, k_fail;

static box_t *box_for(uint32_t pid) {
	return pid == NET ? &box_net : pid == NS ? &box_ns : pid == PX ? &box_px : NULL;
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32; ((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_SEND: {
		z_msg_t *m = (z_msg_t *)args;
		box_t *bx = box_for(m->to);
		if (!bx || bx->n >= 512) return (uint32_t *)&k_fail;
		bx->m[bx->n] = *m;
		bx->m[bx->n].from = current;
		bx->n++;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_MSG_READ: {
		box_t *bx = box_for(current);
		if (!bx || !bx->n) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = bx->m[0];
		memmove(bx->m, bx->m + 1, sizeof(z_msg_t) * (size_t)(--bx->n));
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_PID_REGISTER: {
		static char nm[] = "netserve0";
		((z_obj_t *)args)->type = Z_STR; ((z_obj_t *)args)->val.str = nm;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_PID_LOOKUP: {
		z_obj_t *o = (z_obj_t *)args;
		if (!strcmp(o->val.str, "net0")) { o->type = Z_UINT32; o->val.uint32 = NET; return (uint32_t *)&k_ok; }
		if (!strcmp(o->val.str, "posix0")) { o->type = Z_UINT32; o->val.uint32 = PX; return (uint32_t *)&k_ok; }
		return (uint32_t *)&k_fail;
	}
	case Z_SYS_CFG_GET: {
		z_cfg_get_args_t *a = (z_cfg_get_args_t *)args;
		const char *v = NULL;
		if (a->key && !strcmp(a->key, "apps.netserve.telnet")) v = "23 posix0";
		if (a->key && !strcmp(a->key, "apps.netserve.echo")) v = "7";
		a->found = v != NULL;
		if (v) snprintf(a->val, a->vallen, "%s", v);
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_AUTH: {
		z_auth_args_t *a = (z_auth_args_t *)args;
		a->result = Z_AUTH_OK;
		if (a->op == Z_AUTH_STATUS) {
			memset(a->st, 0, sizeof(*a->st));
			a->st->flags = Z_AUTH_HAS_PASSWORD | Z_AUTH_NET_OK;
		} else if (a->op == Z_AUTH_CHECK && (a->pwlen != 13 || memcmp(a->pw, "correct horse", 13)))
			a->result = Z_AUTH_E_BAD;
		return (uint32_t *)&k_ok;
	}
	default:
		return (uint32_t *)&k_ok;
	}
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

// -- net's main loop, the parts that matter --
static void net_step(void) {
	z_msg_t m;
	current = NET;
	while (z_msg_read(&m) == Z_OK) {
		if (relay_msg(&m)) continue;
		if (m.subject == Z_NET_LISTEN) relay_listen(&m);
	}
	tcp_poll();
	relay_poll();
}

// -- the peer: a telnet client that acks what it gets --
static uint32_t peer_seq = 5000, peer_ack;
static uint8_t screen[65536];
static uint32_t screen_len;
static uint16_t lport_peer = 40001;

static void peer_rx(uint8_t flags, const void *d, uint16_t n) {
	uint8_t p[1600];
	memset(p, 0, 20);
	p[0] = (uint8_t)(lport_peer >> 8); p[1] = (uint8_t)lport_peer; p[2] = 0; p[3] = 23;
	p[4] = (uint8_t)(peer_seq >> 24); p[5] = (uint8_t)(peer_seq >> 16); p[6] = (uint8_t)(peer_seq >> 8); p[7] = (uint8_t)peer_seq;
	p[8] = (uint8_t)(peer_ack >> 24); p[9] = (uint8_t)(peer_ack >> 16); p[10] = (uint8_t)(peer_ack >> 8); p[11] = (uint8_t)peer_ack;
	p[12] = 5 << 4; p[13] = flags; p[14] = 0xFF; p[15] = 0xFF;
	if (n) memcpy(p + 20, d, n);
	current = NET;
	tcp_handle(PEER, p, (uint16_t)(20 + n));
	peer_seq += n + ((flags & 0x02) ? 1 : 0);
}

// What the stack sent: data goes on the screen, and is acked.
static void peer_take(void) {
	for (int i = 0; i < nwire; i++) {
		seg_t *s = &wire[i];
		if ((s->flags & 0x12) == 0x12) { peer_ack = s->seq + 1; continue; }   // SYN-ACK
		if (s->len && s->seq == peer_ack) {
			memcpy(screen + screen_len, s->data, s->len);
			screen_len += s->len;
			peer_ack += s->len;
		}
	}
	nwire = 0;
}

// -- the backend: posix, scripted --
static uint32_t px_conn;
static void px_step(void) {
	z_msg_t m;
	current = PX;
	while (z_msg_read(&m) == Z_OK) {
		if (m.subject == Z_PORT_CONNECT) {
			z_msg_new_send(m.from, Z_PORT_CONNECTED, 0, z_obj_uint32(1));
			px_conn = 1;
		} else if (m.subject == Z_PORT_DATA) {
			z_port_send_ack(&m);
		}
	}
}

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(int rounds, bool peer_acks) {
	for (int i = 0; i < rounds; i++) {
		ticks += 2;
		net_step();
		current = NS; ns_step();
		px_step();
		current = NET;
		if (peer_acks) { peer_take(); peer_rx(0x10, NULL, 0); }
	}
}

static bool on_screen(const char *t) {
	size_t n = strlen(t);
	for (uint32_t i = 0; i + n <= screen_len; i++) if (!memcmp(screen + i, t, n)) return true;
	return false;
}

static void echo_test(bool fin, uint16_t ep) {
	//
	// The peer sends as fast as the advertised window allows, several
	// segments per pass (the RMII MAC holds four frames), and acks what
	// it gets. On hardware: 512 came back.
	{
		uint32_t eseq = 90000, eack = 0, ewin = 0, sent = 0, got = 0, acked_by_us = 90001;
		static uint8_t back[4000];
		// handshake
		nwire = 0;
		{
			uint8_t p[40];
			memset(p, 0, 20);
			p[0] = (uint8_t)(ep >> 8); p[1] = (uint8_t)ep; p[3] = 7;
			p[4] = (uint8_t)(eseq >> 24); p[5] = (uint8_t)(eseq >> 16); p[6] = (uint8_t)(eseq >> 8); p[7] = (uint8_t)eseq;
			p[12] = 5 << 4; p[13] = 0x02; p[14] = 0xFF; p[15] = 0xFF;
			current = NET; tcp_handle(PEER, p, 20);
		}
		for (int i = 0; i < nwire; i++) if ((wire[i].flags & 0x12) == 0x12) { eack = wire[i].seq + 1; ewin = wire[i].win; }
		nwire = 0;
		eseq++;
		for (int round = 0; round < 3000 && got < 3000; round++) {
			uint8_t p[1600];
			// the peer: everything the window allows, in segments of up to 512
			uint32_t burst = 0;
			while (sent < 3000 && (eseq - acked_by_us) < ewin && burst < 4) {
				uint32_t room = ewin - (eseq - acked_by_us), n = 3000 - sent;
				if (n > room) n = room;
				if (n > 512) n = 512;
				memset(p, 0, 20);
				p[0] = (uint8_t)(ep >> 8); p[1] = (uint8_t)ep; p[3] = 7;
				p[4] = (uint8_t)(eseq >> 24); p[5] = (uint8_t)(eseq >> 16); p[6] = (uint8_t)(eseq >> 8); p[7] = (uint8_t)eseq;
				p[8] = (uint8_t)(eack >> 24); p[9] = (uint8_t)(eack >> 16); p[10] = (uint8_t)(eack >> 8); p[11] = (uint8_t)eack;
				p[12] = 5 << 4; p[13] = (fin && sent + n == 3000) ? 0x19 : 0x18; p[14] = 0xFF; p[15] = 0xFF;
				memset(p + 20, 'x', n);
				current = NET; tcp_handle(PEER, p, (uint16_t)(20 + n));
				eseq += n + ((fin && sent + n == 3000) ? 1 : 0); sent += n; burst++;
			}
			ticks += 2;
			net_step();
			current = NS; ns_step();
			// what came back: data on the screen, acks and windows noted
			bool data = false;
			for (int i = 0; i < nwire; i++) {
				seg_t *w = &wire[i];
				if (w->flags & 0x10) { acked_by_us = w->ack; ewin = w->win; }
				if (w->len && w->seq == eack) { memcpy(back + got, w->data, w->len); got += w->len; eack += w->len; data = true; }
			}
			nwire = 0;
			if (data) {
				memset(p, 0, 20);
				p[0] = (uint8_t)(ep >> 8); p[1] = (uint8_t)ep; p[3] = 7;
				p[4] = (uint8_t)(eseq >> 24); p[5] = (uint8_t)(eseq >> 16); p[6] = (uint8_t)(eseq >> 8); p[7] = (uint8_t)eseq;
				p[8] = (uint8_t)(eack >> 24); p[9] = (uint8_t)(eack >> 16); p[10] = (uint8_t)(eack >> 8); p[11] = (uint8_t)eack;
				p[12] = 5 << 4; p[13] = 0x10; p[14] = 0xFF; p[15] = 0xFF;
				current = NET; tcp_handle(PEER, p, 20);
			}
		}
		CK(got == 3000, "echo%s: 3000 bytes in, %u back (sent %u)", fin ? ", the last with FIN as nc sends it" : "", got, sent);
	}
}


int main(void) {
	if (!k_install()) { printf("e2e test: skipped\n"); return 77; }
	current = NET;
	tcp_init(US);
	tcp_set_rx_window_max(5360);
	relay_init();
	current = NS;
	ns_init();
	run(5, false);

	// telnet connects
	nwire = 0;
	peer_rx(0x02, NULL, 0);                 // SYN
	peer_take();
	peer_rx(0x10, NULL, 0);                 // ACK
	run(10, true);
	CK(on_screen("password: "), "the password prompt reaches the peer");

	peer_rx(0x18, "correct horse\r\n", 15);
	run(20, true);
	CK(px_conn == 1 && on_screen("connecting to posix0"), "logged in, connected to posix");

	// `help`: one message of about 1.4 KB, as posix sends it
	{
		static char help[1500];
		int n = 0;
		for (int i = 0; i < 27; i++) n += snprintf(help + n, sizeof(help) - (size_t)n, "  %-8s %s\n", "builtin", "what it does, roughly this long");
		current = PX;
		z_port_t px;
		memset(&px, 0, sizeof(px));
		px.peer_pid = NS; px.conn_id = 1; px.connected = true;
		uint32_t before = screen_len;
		CK(z_port_send(&px, help, (uint32_t)n) == Z_OK, "posix sends %d bytes in one message", n);
		run(200, true);
		CK(screen_len - before == (uint32_t)n, "all %d bytes reach the telnet client (%u did)", n, screen_len - before);

		// and a 4 KB one, OUT_BATCH-sized, twice in a row
		static char big[4096];
		memset(big, 'x', sizeof(big));
		before = screen_len;
		current = PX;
		z_port_send(&px, big, sizeof(big));
		current = PX;
		z_port_send(&px, big, sizeof(big));
		run(400, true);
		CK(screen_len - before == 2 * sizeof(big), "two 4 KB messages arrive whole (%u of %u)",
			screen_len - before, (unsigned)(2 * sizeof(big)));
	}

	echo_test(false, 40100);
	echo_test(true, 40200);

	printf("e2e test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
