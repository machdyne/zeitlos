/*
 * sftp.c, scp.c and xfer.c by hand: what the OpenSSH programs never
 * send (tests/test_openssh.sh covers what they do). Paths made
 * absolute, requests split at every byte, a WRITE through the ring's
 * wrap, malformed and oversized requests, plain RENAME refusing to
 * replace, read-only, and scp names that would climb out of a folder.
 *
 *   make test   (see ../Makefile)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../xfer.h"
#include "../nsfs.h"

void nsfs_posix_root(const char *r);

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[256];

static void xlogl(void *u, const char *l) { (void)u; if (getenv("XLOG")) printf("log: %s\n", l); }

// -- building requests --

static uint8_t pkt[70000];
static uint32_t pn;

static void p_u8(uint8_t v) { pkt[pn++] = v; }
static void p_u32(uint32_t v) { p_u8((uint8_t)(v >> 24)); p_u8((uint8_t)(v >> 16)); p_u8((uint8_t)(v >> 8)); p_u8((uint8_t)v); }
static void p_u64(uint64_t v) { p_u32((uint32_t)(v >> 32)); p_u32((uint32_t)v); }
static void p_str(const void *s, uint32_t n) { p_u32(n); memcpy(pkt + pn, s, n); pn += n; }
static void p_cstr(const char *s) { p_str(s, (uint32_t)strlen(s)); }
static void p_begin(uint8_t type, uint32_t id) { pn = 4; p_u8(type); if (type != 1) p_u32(id); }
static void p_end(void) { uint32_t l = pn - 4; pkt[0] = (uint8_t)(l >> 24); pkt[1] = (uint8_t)(l >> 16); pkt[2] = (uint8_t)(l >> 8); pkt[3] = (uint8_t)l; }

// -- running one --

static uint8_t got[200000];
static uint32_t gn;

// Feeds `n` bytes `chunk` at a time, as the window allows, stepping
// and collecting the output between.
static void run(xfer_t *x, const uint8_t *d, uint32_t n, uint32_t chunk) {
	uint32_t at = 0;
	for (int guard = 0; guard < 1000000; guard++) {
		uint32_t room = xfer_in_room(x), k = n - at;
		if (k > chunk) k = chunk;
		if (k > room) k = room;
		if (k) { xfer_put(x, d + at, k); at += k; }
		uint32_t used = xfer_step(x, 3);
		uint32_t on;
		const uint8_t *o = xfer_out(x, &on);
		if (on) {
			memcpy(got + gn, o, on);
			gn += on;
			xfer_sent(x, on);
		}
		// settled: everything given, nothing taken or answered this pass
		if (at == n && !k && !used && !on) break;
	}
}

static void send_pkt(xfer_t *x, uint32_t chunk) { p_end(); run(x, pkt, pn, chunk); }

// The replies collected, one at a time.
static uint32_t rd_at;
static uint32_t g32(uint32_t at) { return ((uint32_t)got[at] << 24) | ((uint32_t)got[at + 1] << 16) | ((uint32_t)got[at + 2] << 8) | got[at + 3]; }

// Next reply: its type, id and where its body starts.
static bool next_reply(uint8_t *type, uint32_t *id, uint32_t *body, uint32_t *len) {
	if (rd_at + 5 > gn) return false;
	*len = g32(rd_at);
	*type = got[rd_at + 4];
	*id = *type == 2 ? 0 : g32(rd_at + 5);
	*body = rd_at + (*type == 2 ? 5 : 9);
	rd_at += 4 + *len;
	return true;
}

static uint32_t status_of(uint32_t want_id) {
	uint8_t t; uint32_t id, b, l;
	while (next_reply(&t, &id, &b, &l)) if (id == want_id && t == 101) return g32(b);
	return 999;
}

static void reset_got(void) { gn = 0; rd_at = 0; }

static void put_file(const char *rel, const void *d, size_t n) {
	char p[512];
	snprintf(p, sizeof(p), "%s%s", root, rel);
	FILE *f = fopen(p, "wb");
	fwrite(d, 1, n, f);
	fclose(f);
}

static long file_size(const char *rel) {
	char p[512];
	struct stat st;
	snprintf(p, sizeof(p), "%s%s", root, rel);
	return stat(p, &st) ? -1 : (long)st.st_size;
}

static void test_paths(void) {
	char o[XFER_PATH];
	struct { const char *home, *in, *want; } t[] = {
		{ "/home", "", "/home" }, { "/home", ".", "/home" }, { "/home", "a", "/home/a" },
		{ "/home", "a/b/../c", "/home/a/c" }, { "/home", "/x//y/", "/x/y" },
		{ "/home", "..", "/" }, { "/home", "../../..", "/" }, { "/home", "~", "/home" },
		{ "/home", "~/n", "/home/n" }, { "/", "./a/./b", "/a/b" }, { "/home", "/", "/" },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		bool ok = xfer_path(o, t[i].home, t[i].in, (uint32_t)strlen(t[i].in));
		CHECK(ok && !strcmp(o, t[i].want), "xfer_path(%s, %s) = %s, want %s", t[i].home, t[i].in, ok ? o : "(refused)", t[i].want);
	}
	CHECK(!xfer_path(o, "/", "a\nb", 3), "a control character refused");
	CHECK(!xfer_path(o, "/", "a\0b", 3), "a NUL refused");
	char longp[300];
	memset(longp, 'x', 252); longp[0] = '/'; longp[252] = 0;
	CHECK(!xfer_path(o, "/", longp, 252), "no room for .part: refused");
	CHECK(xfer_join(o, "/", "a") && !strcmp(o, "/a"), "join at the root");
	CHECK(!xfer_join(o, "/x", ".."), "join refuses ..");
	CHECK(!xfer_join(o, "/x", "a/b"), "join refuses a slash");
}

static xfer_t *sftp_new(uint32_t flags) {
	xfer_t *x = xfer_sftp("/home", flags, xlogl, NULL);
	reset_got();
	p_begin(1, 0); p_u32(3); send_pkt(x, 4096);
	uint8_t t; uint32_t id, b, l;
	CHECK(next_reply(&t, &id, &b, &l) && t == 2 && g32(b) == 3, "VERSION 3");
	return x;
}

static void test_sftp(void) {
	xfer_t *x = sftp_new(0);
	static uint8_t data[50000];
	for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7 + (i >> 9));

	// open for writing, a WRITE of 50000 split at every 1, 7 and 1000 bytes
	uint32_t chunks[] = { 1, 7, 1000 };
	for (int c = 0; c < 3; c++) {
		reset_got();
		p_begin(3, 10); p_cstr("w.bin"); p_u32(0x02 | 0x08 | 0x10); p_u32(0); send_pkt(x, chunks[c]);
		uint8_t t; uint32_t id, b, l;
		CHECK(next_reply(&t, &id, &b, &l) && t == 102 && id == 10, "OPEN -> HANDLE (chunk %u)", chunks[c]);
		uint8_t h[4]; memcpy(h, got + b + 4, 4);
		CHECK(file_size("/home/w.bin") == (c ? (long)sizeof(data) : -1), "until CLOSE, the name is the old file or nothing");
		CHECK(file_size("/home/w.bin.part") == 0, "the .part meanwhile");
		p_begin(6, 11); p_str(h, 4); p_u64(0); p_str(data, sizeof(data)); send_pkt(x, chunks[c]);
		CHECK(status_of(11) == 0, "WRITE ok (chunk %u)", chunks[c]);
		p_begin(4, 12); p_str(h, 4); send_pkt(x, chunks[c]);
		CHECK(status_of(12) == 0, "CLOSE ok");
		CHECK(file_size("/home/w.bin") == (long)sizeof(data) && file_size("/home/w.bin.part") < 0, "in place");
	}

	// READ: at most 2048 a reply, past the end EOF
	reset_got();
	p_begin(3, 20); p_cstr("/home/w.bin"); p_u32(0x01); p_u32(0); send_pkt(x, 4096);
	uint8_t t; uint32_t id, b, l, h_at;
	CHECK(next_reply(&t, &id, &b, &l) && t == 102, "OPEN for reading");
	h_at = b + 4;
	uint8_t h[4]; memcpy(h, got + h_at, 4);
	reset_got();
	p_begin(5, 21); p_str(h, 4); p_u64(100); p_u32(32768); send_pkt(x, 4096);
	CHECK(next_reply(&t, &id, &b, &l) && t == 103 && g32(b) == 2048 && !memcmp(got + b + 4, data + 100, 2048),
		"READ of 32768: 2048, the right bytes");
	reset_got();
	p_begin(5, 22); p_str(h, 4); p_u64(sizeof(data) - 10); p_u32(100); send_pkt(x, 4096);
	CHECK(next_reply(&t, &id, &b, &l) && t == 103 && g32(b) == 10, "READ at the end: what is there");
	reset_got();
	p_begin(5, 23); p_str(h, 4); p_u64(sizeof(data)); p_u32(100); send_pkt(x, 4096);
	CHECK(status_of(23) == 1, "READ past the end: EOF");
	reset_got();
	p_begin(4, 24); p_str(h, 4); send_pkt(x, 4096);
	CHECK(status_of(24) == 0, "CLOSE");
	reset_got();
	p_begin(5, 25); p_str(h, 4); p_u64(0); p_u32(10); send_pkt(x, 4096);
	CHECK(status_of(25) == 4, "a closed handle: FAILURE");

	// RENAME (v3) refuses to replace; posix-rename replaces
	put_file("/home/r1", "one", 3);
	put_file("/home/r2", "two!", 4);
	reset_got();
	p_begin(18, 30); p_cstr("r1"); p_cstr("r2"); send_pkt(x, 4096);
	CHECK(status_of(30) == 4 && file_size("/home/r2") == 4, "RENAME onto a file: refused");
	reset_got();
	p_begin(200, 31); p_cstr("posix-rename@openssh.com"); p_cstr("r1"); p_cstr("r2"); send_pkt(x, 4096);
	CHECK(status_of(31) == 0 && file_size("/home/r2") == 3 && file_size("/home/r1") < 0, "posix-rename replaces");

	// OPEN with CREAT|EXCL on a file there: refused; without CREAT on one not there: NO_SUCH_FILE
	reset_got();
	p_begin(3, 40); p_cstr("r2"); p_u32(0x02 | 0x08 | 0x20); p_u32(0); send_pkt(x, 4096);
	CHECK(status_of(40) == 4, "EXCL on a file there");
	reset_got();
	p_begin(3, 41); p_cstr("nope"); p_u32(0x01); p_u32(0); send_pkt(x, 4096);
	CHECK(status_of(41) == 2, "no such file");

	// SETSTAT with a size truncates
	reset_got();
	p_begin(9, 42); p_cstr("w.bin"); p_u32(1); p_u64(123); send_pkt(x, 4096);
	CHECK(status_of(42) == 0 && file_size("/home/w.bin") == 123, "SETSTAT size");

	// an unknown request, and an unknown extension
	reset_got();
	p_begin(99, 50); send_pkt(x, 4096);
	CHECK(status_of(50) == 8, "unknown type: OP_UNSUPPORTED");
	reset_got();
	p_begin(200, 51); p_cstr("nonsense@x"); send_pkt(x, 4096);
	CHECK(status_of(51) == 8, "unknown extension: OP_UNSUPPORTED");

	// a request too long to assemble: FAILURE, and the next is answered
	reset_got();
	{
		static char big[3000];
		memset(big, 'a', sizeof(big) - 1);
		p_begin(17, 60); p_cstr(big); send_pkt(x, 777);
		CHECK(status_of(60) == 4, "a 3000-byte path: FAILURE");
		rd_at = 0;
		p_begin(17, 61); p_cstr("r2"); send_pkt(x, 4096);
		uint32_t ii; uint8_t tt; uint32_t bb, ll; bool seen = false;
		while (next_reply(&tt, &ii, &bb, &ll)) if (ii == 61 && tt == 105) seen = true;
		CHECK(seen, "the next request answered");
	}

	// open files are limited, and say so
	reset_got();
	for (int i = 0; i < 5; i++) { p_begin(3, 70 + i); p_cstr("r2"); p_u32(1); p_u32(0); send_pkt(x, 4096); }
	CHECK(status_of(74) == 4, "a fifth open file: FAILURE");

	// an upload not closed when the session ends: .part gone, old file kept
	reset_got();
	p_begin(3, 80); p_cstr("r2"); p_u32(0x02 | 0x08 | 0x10); p_u32(0); send_pkt(x, 4096);
	xfer_free(x);       // with four files and this one open
	CHECK(file_size("/home/r2") == 3 && file_size("/home/r2.part") < 0, "stopped: the old file, no .part");

	// a length no request has: the session ends
	x = sftp_new(0);
	reset_got();
	{ uint8_t bad[4] = { 0x7f, 0, 0, 0 }; run(x, bad, 4, 4); }
	uint32_t st = 0;
	CHECK(xfer_done(x, &st) && st == 1, "impossible length: over");
	xfer_free(x);

	// read-only
	x = sftp_new(XFER_READONLY);
	reset_got();
	p_begin(3, 90); p_cstr("ro"); p_u32(0x02 | 0x08); p_u32(0); send_pkt(x, 4096);
	CHECK(status_of(90) == 3 && file_size("/home/ro") < 0, "read-only: no new file");
	reset_got();
	p_begin(13, 91); p_cstr("r2"); send_pkt(x, 4096);
	CHECK(status_of(91) == 3 && file_size("/home/r2") == 3, "read-only: no remove");
	reset_got();
	p_begin(3, 92); p_cstr("r2"); p_u32(0x01); p_u32(0); send_pkt(x, 4096);
	CHECK(next_reply(&t, &id, &b, &l) && t == 102, "read-only: reading is fine");
	xfer_eof(x);
	xfer_step(x, 3);
	CHECK(xfer_done(x, &st) && st == 0, "EOF: over, status 0");
	xfer_free(x);
}

static void test_scp(void) {
	char why[96];
	uint32_t st;

	CHECK(!xfer_scp("ls -l", "/home", 0, xlogl, NULL, why, sizeof(why)), "not scp: refused");
	CHECK(!xfer_scp("scp -Z -t x", "/home", 0, xlogl, NULL, why, sizeof(why)), "unknown option: refused");
	CHECK(!xfer_scp("scp -r", "/home", 0, xlogl, NULL, why, sizeof(why)), "no -t/-f: refused");

	// sink: a folder with a file in it, then a name that climbs out
	mkdir("/tmp/.unused", 0755);
	xfer_t *x = xfer_scp("scp -r -t '/home/in coming'", "/home", 0, xlogl, NULL, why, sizeof(why));
	CHECK(x != NULL, "scp -r -t with a quoted path");
	reset_got();
	static const char s1[] = "D0755 0 top\nC0644 5 f.txt\nhello\0E\n";
	run(x, (const uint8_t *)s1, sizeof(s1) - 1, 3);
	CHECK(gn == 5 && !memcmp(got, "\0\0\0\0\0", 5), "five acknowledgements: %u", gn);
	CHECK(file_size("/home/in coming") == 4096 || file_size("/home/in coming") >= 0, "the target folder made");
	CHECK(file_size("/home/in coming/f.txt") == 5, "the file in it (target did not exist: D is it)");
	reset_got();
	static const char s2[] = "C0644 3 ../evil\n";
	run(x, (const uint8_t *)s2, sizeof(s2) - 1, 64);
	CHECK(gn > 1 && got[0] == 2, "../ in a name: fatal");
	CHECK(xfer_done(x, &st) && st == 1, "and over");
	xfer_free(x);

	// sink: the client stops in the middle -- old file kept, no .part
	put_file("/home/keep", "old", 3);
	x = xfer_scp("scp -t /home/keep", "/home", 0, xlogl, NULL, why, sizeof(why));
	reset_got();
	static const char s3[] = "C0644 10 keep\nabc";
	run(x, (const uint8_t *)s3, sizeof(s3) - 1, 64);
	xfer_eof(x);
	xfer_step(x, 3);
	CHECK(xfer_done(x, &st) && st == 1, "stopped: over, status 1");
	CHECK(file_size("/home/keep") == 3 && file_size("/home/keep.part") < 0, "stopped: old file, no .part");
	xfer_free(x);

	// -d: the target must be a folder
	x = xfer_scp("scp -d -t /home/keep", "/home", 0, xlogl, NULL, why, sizeof(why));
	reset_got();
	run(x, (const uint8_t *)"", 0, 1);
	CHECK(gn > 1 && got[0] == 2, "-d onto a file: fatal");
	xfer_free(x);

	// read-only: a file refused before its data
	x = xfer_scp("scp -t /home/ro", "/home", XFER_READONLY, xlogl, NULL, why, sizeof(why));
	reset_got();
	static const char s4[] = "C0644 2 ro\n";
	run(x, (const uint8_t *)s4, sizeof(s4) - 1, 64);
	CHECK(gn > 2 && got[0] == 0 && got[1] == 1, "read-only: ready, then an error for the file");
	CHECK(file_size("/home/ro") < 0, "read-only: nothing written");
	xfer_free(x);

	// source: a file, acknowledged step by step
	put_file("/home/src.txt", "0123456789", 10);
	x = xfer_scp("scp -f /home/src.txt", "/home", 0, xlogl, NULL, why, sizeof(why));
	reset_got();
	run(x, (const uint8_t *)"\0", 1, 1);
	CHECK(gn > 10 && !memcmp(got, "C0644 10 src.txt\n", 17), "C header first: %.*s", (int)gn, got);
	reset_got();
	run(x, (const uint8_t *)"\0", 1, 1);
	CHECK(gn == 11 && !memcmp(got, "0123456789\0", 11), "then the data and a 0");
	reset_got();
	run(x, (const uint8_t *)"\0", 1, 1);
	CHECK(xfer_done(x, &st) && st == 0, "acknowledged: over");
	xfer_free(x);
}

int main(void) {
	char cmd[300];
	snprintf(root, sizeof(root), "/tmp/test_xfer.%d", (int)getpid());
	snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/home", root, root);
	if (system(cmd)) return 1;
	nsfs_posix_root(root);

	test_paths();
	test_sftp();
	test_scp();

	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	if (system(cmd)) { }
	printf("test_xfer: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
