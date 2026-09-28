/*
 * Host test for sw/apps/fed/core/fsess.c. docs/fed.md, "Peers and
 * sessions".
 *
 *   make -C sw/apps/fed test
 *
 * A session's two sides run in two processes, each with its own store
 * (a process has one), over a socket pair. The initiator's side can cut
 * the connection after any number of bytes, or flip a byte on the way.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include "../core/fsess.h"
#include "../core/fstore.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

extern int plat_quiet;

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NOW 1790505386u
#define MAXN 6

typedef struct { char dir[160]; uint8_t sk[64], pk[32]; const char *wants; } node_t;
static node_t nodes[MAXN];
static char base[128];
static uint8_t members[MAXN][32];
static int nmembers;

static bool is_member(const uint8_t k[32], void *ctx) {
	(void)ctx;
	for (int i = 0; i < nmembers; i++) if (!memcmp(members[i], k, 32)) return true;
	return false;
}
static bool origin_member(const uint8_t k[32], const char *topic, void *ctx) { (void)topic; return is_member(k, ctx); }

static void sh(const char *c) { int r = system(c); (void)r; }

static uint32_t ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// -- objects --

static uint32_t seqs[MAXN];
static void add(int who, const char *topic, int state, const char *key, int len) {
	static uint8_t buf[FOBJ_MAX], pl[FOBJ_PAYLOAD_MAX];
	fobj_t o;
	uint32_t pos;
	memset(&o, 0, sizeof(o));
	snprintf(o.topic, sizeof(o.topic), "%s", topic);
	snprintf(o.type, sizeof(o.type), "t");
	o.format = FOBJ_BYTES;
	o.kind = state ? FOBJ_STATE : FOBJ_LOG;
	if (state) snprintf(o.key, sizeof(o.key), "%s", key);
	o.time = NOW - 100 + seqs[who];
	o.seq = ++seqs[who];
	o.len = (uint32_t)len;
	for (int i = 0; i < len; i++) pl[i] = (uint8_t)(i + o.seq * 7 + who);
	int n = fobj_make(&o, pl, nodes[who].sk, buf, sizeof(buf));
	fstore_put(buf, (uint32_t)n, NOW, &pos);
}

// Every id in dir's store, sorted, into ids; the count.
static int ids_of(const char *dir, uint8_t (*ids)[32], int cap) {
	static uint8_t b[FOBJ_MAX];
	int n = 0;
	fstore_open(dir, NOW, NULL, 0);
	for (uint32_t p = 0; (p = fstore_next(p)) && n < cap; ) {
		int r = fstore_get(p, b, sizeof(b));
		fobj_t o;
		if (r > 0 && !fobj_parse(b, (uint32_t)r, 0, &o, NULL)) memcpy(ids[n++], o.id, 32);
	}
	fstore_close();
	for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
		if (memcmp(ids[i], ids[j], 32) > 0) { uint8_t t[32]; memcpy(t, ids[i], 32); memcpy(ids[i], ids[j], 32); memcpy(ids[j], t, 32); }
	return n;
}

// -- one side of a session over fd --

typedef struct { int state; fsess_stats_t st; uint32_t bytes_in; char error[80]; } side_t;

static void drive(fsess_t *s, int fd, long cut, long flip, side_t *res) {
	uint32_t in = 0, out = 0;
	uint32_t t0 = ms();
	while (s->state == FS_RUNNING) {
		const uint8_t *p;
		uint32_t n = fsess_output(s, &p);
		if (n) {
			static uint8_t tmp[FSESS_WIRE_MAX + 1024];
			memcpy(tmp, p, n);
			if (flip >= 0 && (long)out <= flip && flip < (long)(out + n)) tmp[flip - (long)out] ^= 0x20;
			ssize_t w = write(fd, tmp, n);
			if (w <= 0) break;
			out += (uint32_t)w;
			fsess_consumed(s, (uint32_t)w);
			continue;
		}
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, 200) <= 0) {
			fsess_poll(s, ms());
			if (ms() - t0 > 20000) break;			// a hang: the test says so below
			continue;
		}
		uint8_t b[4096];
		ssize_t r = read(fd, b, sizeof(b));
		if (r <= 0) { fsess_closed(s); break; }
		if (cut >= 0 && in + (uint32_t)r >= (uint32_t)cut) {
			fsess_input(s, b, (uint32_t)cut - in);
			in = (uint32_t)cut;
			fsess_closed(s);
			break;
		}
		in += (uint32_t)r;
		fsess_input(s, b, (uint32_t)r);
		fsess_poll(s, ms());
	}
	res->state = s->state;
	res->st = s->stats;
	res->bytes_in = in;
	snprintf(res->error, sizeof(res->error), "%s", s->error);
}

static fsess_t sess;

// A session: node a initiates (its side here), node b responds (a child).
// expect_peer: the key a expects (NULL: b's).
static void session(int a, int b, long cut, long flip, const uint8_t *expect_peer, side_t *ra, side_t *rb) {
	int sv[2], pp[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	if (pipe(pp) != 0) { perror("pipe"); exit(2); }
	pid_t pid = fork();
	if (pid == 0) {
		close(sv[0]); close(pp[0]);
		fstore_open(nodes[b].dir, NOW, NULL, 0);
		fsess_cfg_t c = { nodes[b].sk, NULL, is_member, origin_member, nodes[b].wants, NOW, NULL, false, NULL, NULL };
		fsess_init(&sess, &c, false);
		side_t r;
		memset(&r, 0, sizeof(r));
		drive(&sess, sv[1], -1, -1, &r);
		fstore_close();
		if (write(pp[1], &r, sizeof(r)) != sizeof(r)) _exit(3);
		_exit(0);
	}
	close(sv[1]); close(pp[1]);
	fstore_open(nodes[a].dir, NOW, NULL, 0);
	fsess_cfg_t c = { nodes[a].sk, expect_peer ? expect_peer : nodes[b].pk, NULL, origin_member, nodes[a].wants, NOW, NULL, false, NULL, NULL };
	fsess_init(&sess, &c, true);
	memset(ra, 0, sizeof(*ra));
	drive(&sess, sv[0], cut, flip, ra);
	close(sv[0]);
	fstore_close();
	memset(rb, 0, sizeof(*rb));
	if (read(pp[0], rb, sizeof(*rb)) != sizeof(*rb)) rb->state = -1;
	close(pp[0]);
	waitpid(pid, NULL, 0);
}

static void reset(int from_template) {
	char c[512];
	snprintf(c, sizeof(c), "rm -rf %s/n* && cp -r %s/template/. %s/", base, base, base);
	if (from_template) sh(c);
}

int main(void) {
	side_t ra, rb;
	static uint8_t ia[400][32], ib[400][32], ic[400][32];
	plat_quiet = getenv("FSESS_TEST_LOG") ? 0 : 1;
	snprintf(base, sizeof(base), "/tmp/fsess-test-%d", (int)getpid());
	for (int i = 0; i < MAXN; i++) {
		uint8_t seed[32];
		memset(seed, 60 + i, 32);
		crypto_ed25519_key_pair(nodes[i].sk, nodes[i].pk, seed);
		snprintf(nodes[i].dir, sizeof(nodes[i].dir), "%s/n%d", base, i);
		nodes[i].wants = "t/*\nfed/*";
		if (i < 4) memcpy(members[nmembers++], nodes[i].pk, 32);		// node 4 and 5: not members
	}
	char c[512];
	snprintf(c, sizeof(c), "rm -rf %s && mkdir -p %s", base, base);
	sh(c);

	// the stores: A (0) 20 objects, B (1) 10, C (2) 8, one shared with A
	fstore_open(nodes[0].dir, NOW, NULL, 0);
	for (int i = 0; i < 20; i++) add(0, i % 2 ? "t/a" : "t/b", 0, "", 50 + i * 40);
	add(0, "t/nodes", 1, "list", 30);
	add(4, "t/a", 0, "", 20);					// from a non-member
	add(0, "t/a", 0, "", 16000);				// near the largest
	fstore_close();
	fstore_open(nodes[1].dir, NOW, NULL, 0);
	for (int i = 0; i < 10; i++) add(1, "t/a", 0, "", 100 + i);
	fstore_close();
	fstore_open(nodes[2].dir, NOW, NULL, 0);
	for (int i = 0; i < 8; i++) add(2, "t/c", 0, "", 70 + i);
	fstore_close();
	snprintf(c, sizeof(c), "mkdir -p %s/template && cp -r %s/n0 %s/n1 %s/n2 %s/template/", base, base, base, base, base);
	sh(c);

	// -- 1. A and B: both pull --
	session(0, 1, -1, -1, NULL, &ra, &rb);
	CK(ra.state == FS_DONE && rb.state == FS_DONE, "a session finishes (%s / %s)", ra.error, rb.error);
	CK(ra.st.got_new == 10 && rb.st.got_new == 22, "each got the other's: A 10 new, B %u new (21 member objects and the state)", rb.st.got_new);
	CK(rb.st.got_rejected == 1, "B refused the non-member's object (%u)", rb.st.got_rejected);
	uint32_t total_in = ra.bytes_in;
	int na = ids_of(nodes[0].dir, ia, 400), nb = ids_of(nodes[1].dir, ib, 400);
	CK(na == 33 && nb == 32, "A holds 33, B 32 (all but the non-member's): %d, %d", na, nb);
	session(0, 1, -1, -1, NULL, &ra, &rb);
	CK(ra.state == FS_DONE && ra.st.sent == 0 && rb.st.sent == 0,
		"a second session sends nothing: the cursors held, and nothing echoes back (%u, %u)", ra.st.sent, rb.st.sent);

	// -- 2. relay: B with C, then A gets C's through B --
	session(2, 1, -1, -1, NULL, &ra, &rb);
	CK(ra.state == FS_DONE && rb.st.got_new == 8, "C to B: 8 (%u)", rb.st.got_new);
	session(0, 1, -1, -1, NULL, &ra, &rb);
	CK(ra.state == FS_DONE && ra.st.got_new == 8, "A got C's 8 through B (%u)", ra.st.got_new);
	session(0, 2, -1, -1, NULL, &ra, &rb);
	CK(ra.state == FS_DONE && ra.st.got_new == 0 && rb.st.got_new == 0,
		"A with C directly: nothing new either way -- each already had it all by other routes (%u, %u)", ra.st.got_new, rb.st.got_new);
	na = ids_of(nodes[0].dir, ia, 400); nb = ids_of(nodes[1].dir, ib, 400);
	int nc = ids_of(nodes[2].dir, ic, 400);
	CK(na == 41 && nb == 40 && nc == 40 && !memcmp(ib, ic, (size_t)nb * 32),
		"by three routes, no duplicates: %d %d %d, B and C the same", na, nb, nc);

	// -- 3. a lost store: B starts again, with a new epoch --
	snprintf(c, sizeof(c), "rm -rf %s", nodes[1].dir);
	sh(c);
	// ... and makes 5 new objects: positions 1-5 again, below A's old
	// cursor into it -- which must now mean nothing
	fstore_open(nodes[1].dir, NOW, NULL, 0);
	for (int i = 0; i < 5; i++) add(1, "t/new", 0, "", 60 + i);
	fstore_close();
	session(0, 1, -1, -1, NULL, &ra, &rb);
	CK(ra.state == FS_DONE && rb.st.got_new == 40 && ra.st.got_new == 5,
		"B lost everything and started again: A sends all (%u), and gets B's 5 new ones despite its old cursor (%u)",
		rb.st.got_new, ra.st.got_new);

	// -- 4. cut at many points, then a whole session: the union, once --
	{
		int bad = 0, tried = 0;
		for (long cut = 0; cut <= (long)total_in + 50; cut += (cut < 300 ? 13 : (long)total_in / 23 + 1)) {
			reset(1);
			session(0, 1, cut, -1, NULL, &ra, &rb);
			session(0, 1, -1, -1, NULL, &ra, &rb);
			na = ids_of(nodes[0].dir, ia, 400); nb = ids_of(nodes[1].dir, ib, 400);
			tried++;
			if (ra.state != FS_DONE || na != 33 || nb != 32) {
				bad++;
				if (bad < 4) printf("  cut at %ld: %s, A %d, B %d\n", cut, ra.error, na, nb);
			}
		}
		CK(bad == 0, "cut after %d different byte counts, then a whole session: the union, nothing lost or doubled (%d bad)", tried, bad);
		printf("  (cut at %d points, from inside the first message to past the last byte, %u bytes in all)\n", tried, total_in);
	}

	// -- 5. refusals --
	reset(1);
	session(4, 1, -1, -1, NULL, &ra, &rb);
	CK(ra.state == FS_FAILED && ra.bytes_in == 0 && rb.state == FS_FAILED,
		"a stranger: not one byte back (%u), refused before any cryptography (%s)", ra.bytes_in, rb.error);
	{
		// an impostor: claims A's key in the clear, cannot sign as A
		uint8_t keep[64];
		memcpy(keep, nodes[5].sk, 64);
		memcpy(nodes[5].sk + 32, nodes[0].pk, 32);		// A's public half, node 5's secret
		session(5, 1, -1, -1, NULL, &ra, &rb);
		memcpy(nodes[5].sk, keep, 64);
		CK(rb.state == FS_FAILED && strstr(rb.error, "signature") && rb.st.got_new == 0,
			"an impostor naming a member's key is refused at AUTH (%s)", rb.error);
	}
	session(0, 1, -1, -1, nodes[2].pk, &ra, &rb);
	CK(ra.state == FS_FAILED && strstr(ra.error, "not the peer"), "the initiator reached the wrong node: it gives up (%s)", ra.error);
	session(0, 1, -1, FSESS_M0 + 10, NULL, &ra, &rb);
	CK(rb.state == FS_FAILED && strstr(rb.error, "decrypt"), "a byte flipped in a frame: the session ends (%s)", rb.error);
	session(0, 1, -1, 69 + 500, NULL, &ra, &rb);
	CK(ra.state == FS_FAILED && rb.state == FS_FAILED, "a byte flipped in the ML-KEM key: no session (%s / %s)", ra.error, rb.error);
	session(0, 1, -1, 10, NULL, &ra, &rb);
	CK(ra.state == FS_FAILED && rb.state == FS_FAILED, "a byte flipped in the first message: no session (%s / %s)", ra.error, rb.error);
	{
		// a bad signature in A's store: B does not take it
		static uint8_t buf[FOBJ_MAX], pl[10] = { 1 };
		fobj_t o; uint32_t pos;
		reset(1);
		memset(&o, 0, sizeof(o));
		snprintf(o.topic, sizeof(o.topic), "t/a"); snprintf(o.type, sizeof(o.type), "t");
		o.format = FOBJ_BYTES; o.kind = FOBJ_LOG; o.time = NOW - 5; o.seq = 99; o.len = 10;
		int n = fobj_make(&o, pl, nodes[0].sk, buf, sizeof(buf));
		buf[n - 136 - 1] ^= 1;				// the payload's last byte: a new id, the old signature
		fstore_open(nodes[0].dir, NOW, NULL, 0);
		CK(fstore_put(buf, (uint32_t)n, NOW, &pos) == FSTORE_NEW, "(the store takes it: it does not check signatures)");
		fstore_close();
		session(0, 1, -1, -1, NULL, &ra, &rb);
		CK(ra.state == FS_DONE && rb.st.got_rejected == 2 && rb.st.got_new == 22,
			"B refuses the forged object and the non-member's (%u), takes the rest", rb.st.got_rejected);
	}
	{
		// B wants only t/a
		reset(1);
		nodes[1].wants = "t/a";
		session(0, 1, -1, -1, NULL, &ra, &rb);
		nodes[1].wants = "t/*\nfed/*";
		CK(ra.state == FS_DONE && rb.st.got_new == 11 && rb.st.got_rejected == 1,
		"B wants t/a only: A sends only those -- B refuses just the non-member's (%u new, %u refused)", rb.st.got_new, rb.st.got_rejected);
	}
	CK(fsess_wanted("t/*", "t/a") && fsess_wanted("t/*", "t/a/b") && !fsess_wanted("t/*", "t") &&
		!fsess_wanted("t/*", "tx/a") && fsess_wanted("x\nt/a", "t/a") && !fsess_wanted("t/a", "t/ab"),
		"patterns: below a/*, exact, several");

	printf("fsess: %d checks, %d failed\n", checks, fails);
	if (!fails) { snprintf(c, sizeof(c), "rm -rf %s", base); sh(c); }
	return fails != 0;
}
