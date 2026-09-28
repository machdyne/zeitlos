/*
 * Tests for the radio link (core/fradio.c): nodes, each in its own
 * process with its own store, and this process playing THE AIR -- every
 * packet a node sends goes to every other node, at most 233 bytes, and
 * the air can lose packets (per receiver), damage fragments, and carry
 * objects from a stranger.
 *
 *   make -C sw/apps/fed test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "../core/fradio.h"
#include "../core/fstore.h"
#include "../core/fobj.h"
#include "../../../ext/monocypher/monocypher.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NODES 3
#define PER   3				// objects each node publishes (small enough)

static uint8_t sk[NODES + 1][64], pk[NODES + 1][32];	// the last: a stranger

static uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// -- a node, in its own process --

static int g_sock;

static bool radio_send(const uint8_t *p, uint32_t n, void *ctx) {
	(void)ctx;
	return send(g_sock, p, n, 0) == (ssize_t)n;
}
static uint32_t clock_s(void *ctx) { (void)ctx; return (uint32_t)time(NULL); }
static bool members(const uint8_t origin[32], const char *topic, void *ctx) {
	(void)topic; (void)ctx;
	for (int i = 0; i < NODES; i++) if (!memcmp(origin, pk[i], 32)) return true;
	return false;
}

static int make(int who, uint32_t plen, uint32_t seq, uint8_t *obj, uint8_t id[32]) {
	static uint8_t payload[4000];
	fobj_t o;
	memset(&o, 0, sizeof(o));
	snprintf(o.topic, sizeof(o.topic), "t/forum/general");
	snprintf(o.type, sizeof(o.type), "bbs.post");
	o.format = FOBJ_BYTES;
	o.kind = FOBJ_LOG;
	o.time = (uint64_t)time(NULL);
	o.seq = seq;
	o.len = plen;
	for (uint32_t i = 0; i < plen; i++) payload[i] = (uint8_t)(i * 13 + seq + who);
	int n = fobj_make(&o, payload, sk[who], obj, FOBJ_MAX);
	fobj_t q;
	fobj_parse(obj, (uint32_t)n, 0, &q, NULL);
	memcpy(id, q.id, 32);
	return n;
}

// Runs node `me` for `secs`; then writes to `rep`: the ids it holds (32
// bytes each), a zero id, then its stats.
static void node(int me, int sock, int rep, const char *dir, int secs, uint32_t max_obj) {
	static uint8_t obj[FOBJ_MAX];
	uint8_t id[32];
	uint32_t pos;
	g_sock = sock;
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
	if (system(cmd)) _exit(2);
	if (fstore_open(dir, (uint32_t)time(NULL), NULL, 64)) _exit(3);
	// what it publishes -- and tells the air, so it knows whose each id is
	static const uint32_t sizes[PER] = { 150, 700, 1500 };
	for (int i = 0; i < PER; i++) {
		int n = make(me, sizes[i], (uint32_t)i + 1, obj, id);
		fstore_put(obj, (uint32_t)n, (uint32_t)time(NULL), &pos);
		if (write(rep, id, 32) != 32) _exit(4);
	}
	if (me == 0) {											// one too big for the link
		int n = make(me, 3000, 99, obj, id);
		fstore_put(obj, (uint32_t)n, (uint32_t)time(NULL), &pos);
		if (write(rep, id, 32) != 32) _exit(4);
	}
	memset(id, 0, 32);
	if (write(rep, id, 32) != 32) _exit(4);

	fradio_cfg_t c;
	memset(&c, 0, sizeof(c));
	c.max_object = max_obj;
	c.pace_ms = 4;
	c.announce_ms = 800;
	c.retry_ms = 250;
	c.retries = 12;
	c.send = radio_send;
	c.clock = clock_s;
	c.origin_ok = members;
	fradio_init(&c, now_ms());
	fradio_up(true);
	uint32_t end = now_ms() + (uint32_t)secs * 1000;
	while ((int32_t)(now_ms() - end) < 0) {
		struct pollfd pf = { sock, POLLIN, 0 };
		if (poll(&pf, 1, 2) > 0) {
			uint8_t p[512];
			ssize_t r = recv(sock, p, sizeof(p), 0);
			if (r > 0) fradio_packet(p, (uint32_t)r, now_ms());
		}
		fradio_poll(now_ms());
	}
	uint32_t p = 0;
	while ((p = fstore_next(p))) {
		int n = fstore_get(p, obj, sizeof(obj));
		fobj_t o;
		if (n > 0 && !fobj_parse(obj, (uint32_t)n, 0, &o, NULL) && write(rep, o.id, 32) != 32) _exit(4);
	}
	memset(id, 0, 32);
	if (write(rep, id, 32) != 32) _exit(4);
	if (write(rep, fradio_stats(), sizeof(fradio_stats_t)) != (ssize_t)sizeof(fradio_stats_t)) _exit(4);
	fstore_close();
	_exit(0);
}

// -- the air --

typedef struct {
	int loss_pct, damage_pct;
	bool stranger;
	int secs;
} air_t;

typedef struct {
	uint8_t pub[PER + 1][32];			// what it published
	int npub;
	uint8_t has[64][32];				// what it held at the end
	int nhas;
	fradio_stats_t st;
} report_t;

static int rd(int fd, void *b, size_t n) {
	size_t got = 0;
	while (got < n) { ssize_t r = read(fd, (uint8_t *)b + got, n - got); if (r <= 0) return -1; got += (size_t)r; }
	return 0;
}

static bool holds(const report_t *r, const uint8_t id[32]) {
	for (int i = 0; i < r->nhas; i++) if (!memcmp(r->has[i], id, 32)) return true;
	return false;
}

// The stranger's object, announced and sent whole, fragment by fragment.
static void inject(int *socks, uint8_t sid[32]) {
	static uint8_t obj[FOBJ_MAX];
	int n = make(NODES, 400, 1, obj, sid);
	uint8_t p[FRADIO_PKT];
	p[0] = 'I'; p[1] = 1; memcpy(p + 2, sid, FRADIO_ID);
	for (int k = 0; k < NODES; k++) if (send(socks[k], p, 2 + FRADIO_ID, 0) < 0) {}
	uint32_t count = ((uint32_t)n + FRADIO_FRAG_DATA - 1) / FRADIO_FRAG_DATA;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t off = i * FRADIO_FRAG_DATA, len = (uint32_t)n - off;
		if (len > FRADIO_FRAG_DATA) len = FRADIO_FRAG_DATA;
		p[0] = 'F'; memcpy(p + 1, sid, FRADIO_ID);
		p[1 + FRADIO_ID] = (uint8_t)n; p[2 + FRADIO_ID] = (uint8_t)(n >> 8);
		p[3 + FRADIO_ID] = (uint8_t)i; p[4 + FRADIO_ID] = (uint8_t)count;
		memcpy(p + 5 + FRADIO_ID, obj + off, len);
		for (int k = 0; k < NODES; k++) if (send(socks[k], p, 5 + FRADIO_ID + len, 0) < 0) {}
	}
}

static void run(const char *name, air_t air) {
	int socks[NODES], reps[NODES];
	pid_t pids[NODES];
	report_t R[NODES];
	int oversize = 0, echoes = 0, damaged = 0, big_announced = 0;
	uint8_t sid[32];

	for (int i = 0; i < NODES; i++) {
		int sv[2], pv[2];
		if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) || pipe(pv)) { perror("socketpair"); exit(1); }
		char dir[80];
		snprintf(dir, sizeof(dir), "/tmp/fradio-test-%d-%d", (int)getpid(), i);
		pids[i] = fork();
		if (!pids[i]) { close(sv[0]); close(pv[0]); node(i, sv[1], pv[1], dir, air.secs, 2048); }
		close(sv[1]); close(pv[1]);
		socks[i] = sv[0];
		reps[i] = pv[0];
		memset(&R[i], 0, sizeof(R[i]));
		for (;;) {
			uint8_t id[32], zero[32] = { 0 };
			if (rd(reps[i], id, 32)) { printf("FAIL: node %d did not start\n", i); exit(1); }
			if (!memcmp(id, zero, 32)) break;
			memcpy(R[i].pub[R[i].npub++], id, 32);
		}
	}
	if (air.stranger) inject(socks, sid);
	srand(4242);
	uint32_t end = now_ms() + (uint32_t)air.secs * 1000 + 500;
	while ((int32_t)(now_ms() - end) < 0) {
		struct pollfd pf[NODES];
		for (int i = 0; i < NODES; i++) { pf[i].fd = socks[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
		if (poll(pf, NODES, 5) <= 0) continue;
		for (int i = 0; i < NODES; i++) {
			if (!(pf[i].revents & POLLIN)) continue;
			uint8_t p[1024];
			ssize_t n = recv(socks[i], p, sizeof(p), 0);
			if (n <= 0) continue;
			if (n > FRADIO_PKT) { oversize++; continue; }
			// an echo: an announcement of an object this node did not publish
			// and got by radio -- Meshtastic relays across hops; this would loop
			if (p[0] == 'I') for (int k = 0; k < p[1]; k++) {
				// the too-big one: never even announced -- asking for what cannot
				// come would waste the air
				if (!memcmp(R[0].pub[PER], p + 2 + k * FRADIO_ID, FRADIO_ID)) big_announced++;
				bool own = false;
				for (int j = 0; j < R[i].npub; j++) if (!memcmp(R[i].pub[j], p + 2 + k * FRADIO_ID, FRADIO_ID)) own = true;
				if (!own) echoes++;
			}
			for (int k = 0; k < NODES; k++) {
				if (k == i || rand() % 100 < air.loss_pct) continue;
				uint8_t q[FRADIO_PKT];
				memcpy(q, p, (size_t)n);
				if (q[0] == 'F' && n > 30 && rand() % 100 < air.damage_pct) { q[25 + rand() % (int)(n - 25)] ^= 0x40; damaged++; }
				if (send(socks[k], q, (size_t)n, 0) < 0) {}
			}
		}
	}
	for (int i = 0; i < NODES; i++) {
		uint8_t id[32], zero[32] = { 0 };
		while (!rd(reps[i], id, 32) && memcmp(id, zero, 32)) if (R[i].nhas < 64) memcpy(R[i].has[R[i].nhas++], id, 32);
		rd(reps[i], &R[i].st, sizeof(R[i].st));
		waitpid(pids[i], NULL, 0);
		close(socks[i]); close(reps[i]);
	}

	// every small object everywhere; the big one only at home
	int missing = 0, big_elsewhere = 0, rejected = 0;
	for (int from = 0; from < NODES; from++)
		for (int j = 0; j < PER; j++)
			for (int at = 0; at < NODES; at++) if (!holds(&R[at], R[from].pub[j])) missing++;
	for (int at = 1; at < NODES; at++) if (holds(&R[at], R[0].pub[PER])) big_elsewhere++;
	for (int i = 0; i < NODES; i++) rejected += (int)R[i].st.objects_rejected;
	CK(missing == 0, "%s: every object of 150-1500 bytes on every node (%d missing)", name, missing);
	CK(big_elsewhere == 0 && holds(&R[0], R[0].pub[PER]), "%s: the 3000-byte one, over the link's 2048, stays home", name);
	CK(big_announced == 0, "%s: and is never announced -- no air spent on it (%d times)", name, big_announced);
	CK(oversize == 0, "%s: no packet over %d bytes (%d)", name, FRADIO_PKT, oversize);
	CK(echoes == 0, "%s: no node announces what it got by radio (%d echoes)", name, echoes);
	if (air.damage_pct) CK(damaged > 0 && rejected > 0, "%s: fragments damaged (%d) -- and the damaged objects refused (%d), then fetched whole",
		name, damaged, rejected);
	if (air.stranger) {
		int took = 0;
		for (int i = 0; i < NODES; i++) if (holds(&R[i], sid)) took++;
		CK(took == 0, "%s: a stranger's object, sent whole: taken by no one (%d)", name, took);
	}
	uint32_t sent = 0;
	for (int i = 0; i < NODES; i++) sent += R[i].st.sent_pkts;
	printf("  %s: %u packets sent in all\n", name, (unsigned)sent);
}

int main(void) {
	signal(SIGPIPE, SIG_IGN);
	for (int i = 0; i <= NODES; i++) {
		uint8_t seed[32];
		memset(seed, 0x30 + i, 32);
		crypto_ed25519_key_pair(sk[i], pk[i], seed);
	}
	run("a clean air", (air_t){ 0, 0, false, 3 });
	run("30% of packets lost", (air_t){ 30, 0, false, 8 });
	run("10% of fragments damaged", (air_t){ 0, 10, false, 8 });
	run("a stranger on the air", (air_t){ 0, 0, true, 3 });
	printf("fradio: %d checks, %d failed\n", checks, fails);
	int r = system("rm -rf /tmp/fradio-test-*");
	(void)r;
	return fails != 0;
}
