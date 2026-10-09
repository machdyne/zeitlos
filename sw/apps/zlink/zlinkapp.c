/*
 * zlink -- files and a shell between two machines over a couple of
 * wires (docs/zlink_app.md; the link itself is docs/zlink.md).
 *
 *   zlink start [-f | -w] [-m auto|soft|stream] [-r KBIT] [PORT [X Y]]
 *   zlink stop
 *   zlink status
 *   zlink ls [PATH]
 *   zlink get REMOTE [LOCAL]
 *   zlink put LOCAL [REMOTE]
 *   zlink speed [KB]
 *   zlink loop [-i] [-r KBIT] PORT TX RX
 *
 * and, from term, `port zlink0`: a shell on the other machine, after
 * its password.
 *
 * One binary, two roles. `zlink start` launches a second copy as the
 * SERVICE (`zlink serve ...`, which nobody types): it owns the wires
 * and the link, registers as zlink0, and serves both its own users and
 * the other machine. Every other command is a CLIENT: it hands its
 * command line to zlink0 as a port connect and relays what comes back
 * to the shell's terminal, the way sw/apps/i2c prints.
 *
 * -- The link, and getting faster --
 *
 * Started without -m, the service begins with SOFT zlink on two open-
 * drain wires (zlink_soft.h): every board with a GPIO port can do that,
 * and it finds out which way round the cable is. Once the link is up,
 * if both machines have a stream engine, the primary offers an UPGRADE
 * (channel 0): the two agree a rate, both let go of the wires, and each
 * puts a stream engine on the same two pins -- the primary transmits
 * on its X, the secondary on the wire the primary's X does not reach.
 * If the faster link does not come up within 2.5 seconds both go back
 * to soft and try again at half the rate: 12, 6, 3, 1.5, 0.75 Mbit/s.
 * The timing that keeps two outputs from ever driving one wire is in
 * upgrade_run().
 *
 * -- Channels --
 *
 *   0  control       'U' rate16: upgrade?  'u' rate16: yes  'n': no
 *   1  requests      to the machine that serves: files, speed test
 *   2  responses     from it
 *   3  shell, in     'O' open, 'D' bytes, 'C' close
 *   4  shell, out    'D' bytes, 'C' reason: closed
 *
 * Requests and responses carry an id after the type byte, so a reply
 * to something abandoned is recognised and ignored:
 *
 *   'L' id path            list          -> 'T' id text...  'K' id
 *   'G' id path            get           -> 'S' id size32  'D' id bytes...  'E' id crc32
 *   'P' id size32 path     put           -> 'K' id,  then 'D' id bytes... 'E' id crc32 -> 'K' id
 *   'A' id                 abandon whatever is running
 *   'B' id bytes...        speed test data; 'b' id -> 'R' id count32
 *   any refusal or failure -> 'X' id message
 *
 * Numbers are little-endian. CRCs are zlink's CRC-32 over the file.
 *
 * -- Who may do what --
 *
 * A shell needs the other machine's password (z_auth_check(), as
 * netserve asks for it) and is refused if it has none. Files are
 * served only if that machine's zlink was started with -f (read) or
 * -w (read and write): anyone at the other end of the cable gets
 * them, so it is opt-in. Local commands are for this machine's own
 * processes; a connect from outside (netserve) is refused.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zport.h"
#include "../../common/zargs.h"
#include "../../common/zgpio.h"
#include "../../common/zgpio_stream.h"
#include "../../common/zlink.h"
#include "../../common/zlink_soft.h"
#include "../../common/zlink_stream.h"
#include "../../common/zfsapp.h"
#include "../../common/zfs.h"

#ifdef ZLINK_HOST_TEST
#include "tests/zlink_host.h"
#else
#include "../../common/zsoc.h"			/* Z_TICK_HZ */
#include "../../common/zwin.h"			/* z_launch_arg_take() */
#include "../../common/zrng.h"
#include "../../common/zauth.h"
#include "../posix/posix.h"				/* PX_STDOUT_TAG */
#endif

void uart_putc(char c);

// -- small things ---------------------------------------------------------

#define CH_CTL    0
#define CH_REQ    1
#define CH_RESP   2
#define CH_SH_IN  3
#define CH_SH_OUT 4

#define ZL_PATH   200		// longest path, either machine

#define RATES_N 5
static const uint16_t rates[RATES_N] = { 12000, 6000, 3000, 1500, 750 };	// kbit/s

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

static char *u2s(uint32_t v, char *b) {
	char t[11];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
	b[n] = 0;
	return b;
}

static long num(const char *s) {
	long v = 0;
	if (!s || !*s) return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || v > 100000000) return -1;
		v = v * 10 + (*s - '0');
	}
	return v;
}

// The console: what the service has to say when nobody asked.
static void logs(const char *a, const char *b) {
	const char *p;
	for (p = "zlink: "; *p; p++) uart_putc(*p);
	for (p = a; p && *p; p++) uart_putc(*p);
	for (p = b; p && *p; p++) uart_putc(*p);
	uart_putc('\r');
	uart_putc('\n');
}

// Milliseconds from kernel ticks (~732 Hz), without the overflow
// ticks * 1000 would hit after an hour and a half.
static uint32_t now_ms(void) {
	static bool init;
	static uint32_t last, ms, frac;
	uint32_t t = z_uptime_ticks();
	if (!init) { init = true; last = t; }
	frac += (t - last) * 1000u;
	last = t;
	ms += frac / Z_TICK_HZ;
	frac %= Z_TICK_HZ;
	return ms;
}

static bool has_password(void) {
	z_auth_status_t st;
	return z_auth_status(&st) == Z_AUTH_OK && (st.flags & Z_AUTH_HAS_PASSWORD);
}

static bool stream_built(void) {
	return z_gs_count() > 0 && (z_gs_modes() & (1u << Z_GS_ZLINK));
}

// The pins, checked the same way by `start` (to say what is wrong) and
// by the service (which trusts nothing).
static const char *pins_bad(int port, int a, int b) {
	if (!z_gpio_present()) return "this bitstream has no GPIO port";
	if (port < 0 || (uint32_t)port >= z_gpio_port_count()) return "no such GPIO port";
	if (a < 0 || a > 7 || b < 0 || b > 7) return "pins are 0-7";
	if (a == b) return "the two pins must differ";
	return NULL;
}

// =========================================================================
// The service
// =========================================================================

enum { M_AUTO, M_SOFT, M_STREAM };
enum { T_SOFT, T_STREAM };

static int g_port, g_x = 0, g_y = 1;	// soft: X and Y. -m stream: TX and RX
static int g_mode = M_AUTO;
static int g_files;						// 0 no, 1 read, 2 read and write
static int g_cap;						// index into rates[]: the fastest still allowed
static int g_tr = T_SOFT;
static int g_engine = -1;
static bool g_stop;

static zlink_t L;
static zl_soft_t S;
static zl_stream_t ST;
static zl_pins_t pins;
static uint16_t g_rate;					// kbit/s, while on a stream

static void pin_pull(void *ctx, int wire, bool low) {
	(void)ctx;
	z_gpio_od_write((uint32_t)g_port, (uint32_t)(wire ? g_y : g_x), !low);
}

static bool pin_level(void *ctx, int wire) {
	(void)ctx;
	return z_gpio_read((uint32_t)g_port, (uint32_t)(wire ? g_y : g_x));
}

static uint32_t seed(void) {
	uint32_t s;
	z_rng_bytes(&s, sizeof s);
	return s;
}

static uint8_t my_caps(void) {
	uint8_t c = ZL_CAP_SOFT;
	if (g_mode != M_SOFT && stream_built() && g_cap < RATES_N) c |= ZL_CAP_STREAM;
	if (g_files) c |= ZL_CAP_FILES;
	if (g_files > 1) c |= ZL_CAP_WRITE;
	if (has_password()) c |= ZL_CAP_SHELL;
	return c;
}

static void soft_start(void) {
	z_gpio_mode((uint32_t)g_port, (uint32_t)g_x, Z_GPIO_OD);
	z_gpio_mode((uint32_t)g_port, (uint32_t)g_y, Z_GPIO_OD);
	pins.ctx = NULL;
	pins.pull = pin_pull;
	pins.level = pin_level;
	zl_soft_init(&S, &pins);
	g_tr = T_SOFT;
	zl_init(&L, &S.t, seed(), my_caps());
}

// Put the claimed engine on the wires. false if the board says no.
static bool stream_start(int tx, int rx, uint16_t kbit) {
	z_gs_cfg_t c = Z_GS_CFG_INIT;
	c.mode = Z_GS_ZLINK;
	c.flags = Z_GS_RXEN;
	c.tx = Z_GS_PIN(g_port, tx);
	c.rx = Z_GS_PIN(g_port, rx);
	c.div = z_gs_div_for(Z_GS_ZLINK, (uint32_t)kbit * 1000u, NULL);
	if (z_gs_config(g_engine, &c) != Z_GS_OK) return false;
	zl_stream_init(&ST, g_engine);
	g_tr = T_STREAM;
	g_rate = kbit;
	zl_init(&L, &ST.t, seed(), my_caps());
	return true;
}

static void engine_free(void) {
	if (g_engine >= 0) z_gs_release(g_engine);
	g_engine = -1;
}

// -- output queue: everything for the link goes through here ----------------

#define OQ 8
static struct { uint8_t ch; uint16_t len; uint8_t d[ZL_PAYLOAD]; } oq[OQ];
static int oq_h, oq_n;

static int oq_free(void) { return OQ - oq_n; }

// A slot to fill in place, or NULL. Committed by oq_commit().
static uint8_t *oq_slot(void) {
	return oq_n < OQ ? oq[(oq_h + oq_n) % OQ].d : NULL;
}

static void oq_commit(uint8_t ch, uint16_t len) {
	int i = (oq_h + oq_n) % OQ;
	oq[i].ch = ch;
	oq[i].len = len;
	oq_n++;
}

// type, id, then a body -- the shape of every message here
static bool oq_put(uint8_t ch, uint8_t type, int id, const void *body, uint32_t n) {
	uint8_t *p = oq_slot();
	uint32_t h = id >= 0 ? 2 : 1;
	if (!p || n + h > ZL_PAYLOAD) return false;
	p[0] = type;
	if (id >= 0) p[1] = (uint8_t)id;
	if (n) memcpy(p + h, body, n);
	oq_commit(ch, (uint16_t)(n + h));
	return true;
}

static bool oq_str(uint8_t ch, uint8_t type, int id, const char *s) {
	return oq_put(ch, type, id, s, (uint32_t)strlen(s));
}

static bool oq_flush(void) {
	bool any = false;
	while (oq_n && zl_can_send(&L) &&
	       zl_send(&L, oq[oq_h].ch, oq[oq_h].d, oq[oq_h].len)) {
		oq_h = (oq_h + 1) % OQ;
		oq_n--;
		any = true;
	}
	return any;
}

// -- DATA from a local port, waiting for the link -----------------------------
//
// term's keys, or a shell's output. Not copied: a DATA payload stays the
// sender's until it is acked (zport.h), so it is read in place, a link
// message at a time, and acked when its last byte has gone. The
// sender's own limit -- Z_PORT_MAX_PENDING_SENDS unacked -- is the flow
// control: a shell printing faster than the link can carry stops when
// eight of its messages are waiting here. (posix sends up to 4 KB in
// one DATA; a byte ring that took and acked messages whole lost the
// tails of long ones.)

typedef struct {
	struct { const uint8_t *d; uint32_t n, off, from, tag; } m[Z_PORT_MAX_PENDING_SENDS];
	uint8_t h, cnt;
	uint32_t dropped;		// a ninth message: the sender broke the rule
} inq_t;

static void ack_late(uint32_t from, uint32_t tag) {
	z_msg_t m;
	memset(&m, 0, sizeof m);
	m.from = from;
	m.tag = tag;
	m.obj.type = Z_BLOB;
	z_port_send_ack(&m);
}

static void inq_in(inq_t *q, const z_msg_t *m) {
	uint32_t len = z_blob_len(&m->obj);
	const uint8_t *d = z_blob_data(&m->obj);
	if (!d || !len || q->cnt == Z_PORT_MAX_PENDING_SENDS) {
		if (d) q->dropped += len;
		z_port_send_ack(m);
		return;
	}
	int i = (q->h + q->cnt) % Z_PORT_MAX_PENDING_SENDS;
	q->m[i].d = d;
	q->m[i].n = len;
	q->m[i].off = 0;
	q->m[i].from = m->from;
	q->m[i].tag = m->tag;
	q->cnt++;
}

// Everything still waiting is acked, unsent: the session is over.
static void inq_reset(inq_t *q) {
	while (q->cnt) {
		ack_late(q->m[q->h].from, q->m[q->h].tag);
		q->h = (uint8_t)((q->h + 1) % Z_PORT_MAX_PENDING_SENDS);
		q->cnt--;
	}
	q->h = 0;
}

// The output queue keeps this many slots for messages that must not
// wait behind data: a close, an abort, a refusal, an answer.
#define OQ_RESERVE 3

// Into the output queue as 'D' messages on `ch`.
static bool inq_out(inq_t *q, uint8_t ch) {
	bool any = false;
	while (q->cnt && oq_free() > OQ_RESERVE) {
		uint8_t *p = oq_slot();
		uint32_t n = 0;
		p[0] = 'D';
		while (q->cnt && n < ZL_PAYLOAD - 1) {
			typeof(q->m[0]) *e = &q->m[q->h];
			uint32_t k = e->n - e->off;
			if (k > ZL_PAYLOAD - 1 - n) k = ZL_PAYLOAD - 1 - n;
			memcpy(p + 1 + n, e->d + e->off, k);
			n += k;
			e->off += k;
			if (e->off == e->n) {
				ack_late(e->from, e->tag);
				q->h = (uint8_t)((q->h + 1) % Z_PORT_MAX_PENDING_SENDS);
				q->cnt--;
			}
		}
		oq_commit(ch, (uint16_t)(n + 1));
		any = true;
	}
	return any;
}

// -- local clients: the command line ------------------------------------------

#define NCLI 3
#define CLI_ID0 1
typedef struct {
	bool used;
	bool closing;			// close once everything is sent and read
	z_port_t port;
	char out[2048];
	uint16_t out_n;
} cli_t;
static cli_t cli[NCLI];

static void say(int c, const char *s) {
	if (c < 0 || !cli[c].used) return;
	while (*s && cli[c].out_n < sizeof cli[c].out - 3) {
		char ch = *s++;
		// 0x01 is the exit status's marker (done()): never in text,
		// whatever a file name or the other machine says
		cli[c].out[cli[c].out_n++] = ch == 1 ? '?' : ch;
	}
}

static void sayn(int c, uint32_t v) {
	char b[12];
	say(c, u2s(v, b));
}

static bool say_room(int c, uint32_t n) {
	return c < 0 || !cli[c].used || cli[c].out_n + n < sizeof cli[c].out - 3;
}

// The exit status rides as a last DATA of two bytes, 0x01 and the
// status, ahead of the CLOSE: the client strips it.
static void done(int c, int rc) {
	if (c < 0 || !cli[c].used) return;
	cli[c].out[cli[c].out_n++] = 1;
	cli[c].out[cli[c].out_n++] = (char)('0' + rc);
	cli[c].closing = true;
}

static bool cli_flush(void) {
	bool any = false;
	for (int c = 0; c < NCLI; c++) {
		cli_t *k = &cli[c];
		if (!k->used) continue;
		while (k->out_n && k->port.pending_count < Z_PORT_MAX_PENDING_SENDS) {
			// 256 at most: the client's buffer counts on it
			uint16_t n = k->out_n > 256 ? 256 : k->out_n;
			if (z_port_send(&k->port, k->out, n) != Z_OK) break;
			memmove(k->out, k->out + n, k->out_n - n);
			k->out_n = (uint16_t)(k->out_n - n);
			any = true;
		}
		if (k->closing && !k->out_n && !k->port.pending_count) {
			z_port_close(&k->port);
			k->used = false;
		}
	}
	return any;
}

// -- the command running for a local client (one at a time) --------------------

enum { FC_NONE, FC_LS, FC_GET, FC_PUT, FC_SPEED };
enum { PH_WAIT, PH_DATA, PH_FINAL };

static struct {
	int op, phase;
	int c;					// the client
	uint8_t id;
	int h;					// local file
	char local[ZL_PATH + 1];
	uint32_t size, moved, crc;
	uint32_t t0, shown;
} fc = { .h = -1 };

static uint8_t next_id;

// An abandon ('A') the output queue had no room for: sent when it has.
static int abort_owed = -1;

static void fc_end(int rc) {
	if (fc.h >= 0) fs_close_handle(fc.h);
	if (rc && fc.op == FC_GET && fc.h >= 0) fs_unlink(fc.local);
	fc.h = -1;
	done(fc.c, rc);
	fc.op = FC_NONE;
}

static void fc_fail(const char *why) {
	say(fc.c, "zlink: ");
	say(fc.c, why);
	say(fc.c, "\n");
	fc_end(1);
}

// Abandoned from this end: tell the server, and forget it.
static void fc_abort(void) {
	if (fc.op == FC_NONE) return;
	if (!oq_put(CH_REQ, 'A', fc.id, NULL, 0)) abort_owed = fc.id;
	if (fc.h >= 0) fs_close_handle(fc.h);
	if (fc.op == FC_GET && fc.h >= 0) fs_unlink(fc.local);
	fc.h = -1;
	fc.op = FC_NONE;
}

// bytes, seconds, KB/s
static void report(int c, uint32_t bytes, uint32_t ms) {
	if (!ms) ms = 1;
	sayn(c, bytes);
	say(c, " bytes in ");
	sayn(c, ms / 1000);
	say(c, ".");
	char b[12];
	u2s(1000 + (ms % 1000) / 100, b);
	say(c, b + 3);
	say(c, " s, ");
	sayn(c, (uint32_t)((uint64_t)bytes * 1000u / ms / 1024u));
	say(c, " KB/s\n");
}

static bool changing(void);

static bool link_ready(int c, uint8_t need) {
	if (!zl_up(&L)) {
		say(c, "zlink: the link is down (zlink status)\n");
		done(c, 1);
		return false;
	}
	if (changing() || oq_free() < 2) {
		say(c, "zlink: busy -- changing to a faster link; try again in a few seconds\n");
		done(c, 1);
		return false;
	}
	if (need && !(zl_peer_caps(&L) & need)) {
		say(c, need == ZL_CAP_WRITE ? "zlink: the other machine only lends its files to read "
		       "(it would need zlink start -w)\n" :
		       "zlink: the other machine does not serve files (it would need zlink start -f)\n");
		done(c, 1);
		return false;
	}
	if (fc.op != FC_NONE) {
		say(c, "zlink: busy with another command\n");
		done(c, 1);
		return false;
	}
	return true;
}

static void cmd_status(int c);

static void cmd(int c, char *line) {
	static char buf[256];
	char *argv[8];
	uint8_t flags[8];
	int argc = z_args_split(line, buf, sizeof buf, argv, flags, 8);
	const char *v = argc > 0 ? argv[0] : "status";

	for (int a = 1; a < argc; a++) {
		if (strlen(argv[a]) > ZL_PATH) {
			say(c, "zlink: a path is too long (200 bytes at most)\n");
			done(c, 2);
			return;
		}
	}

	if (!strcmp(v, "status")) {
		cmd_status(c);
		done(c, 0);
	} else if (!strcmp(v, "stop")) {
		say(c, "zlink: stopped\n");
		done(c, 0);
		g_stop = true;
	} else if (!strcmp(v, "ls")) {
		const char *path = argc > 1 ? argv[1] : "/";
		if (!link_ready(c, ZL_CAP_FILES)) return;
		fc.op = FC_LS;
		fc.c = c;
		fc.id = ++next_id;
		oq_str(CH_REQ, 'L', fc.id, path);
	} else if (!strcmp(v, "get") && (argc == 2 || argc == 3)) {
		if (!link_ready(c, ZL_CAP_FILES)) return;
		fc.op = FC_GET;
		fc.phase = PH_WAIT;
		fc.c = c;
		fc.id = ++next_id;
		fc.h = -1;
		strcpy(fc.local, argv[argc - 1]);
		fc.moved = 0;
		fc.crc = 0;
		fc.t0 = fc.shown = now_ms();
		oq_str(CH_REQ, 'G', fc.id, argv[1]);
	} else if (!strcmp(v, "put") && (argc == 2 || argc == 3)) {
		uint8_t b[ZL_PAYLOAD];
		z_fs_info_t info;
		const char *remote = argv[argc - 1];
		if (!link_ready(c, ZL_CAP_WRITE)) return;
		if (!fs_stat(argv[1], &info) || info.type != Z_FS_TYPE_FILE ||
		    (fc.h = fs_open_read(argv[1])) < 0) {
			fc.h = -1;
			say(c, "zlink: ");
			say(c, argv[1]);
			say(c, ": cannot read it\n");
			done(c, 1);
			return;
		}
		fc.op = FC_PUT;
		fc.phase = PH_WAIT;
		fc.c = c;
		fc.id = ++next_id;
		fc.size = info.size;
		fc.moved = 0;
		fc.crc = 0;
		fc.t0 = fc.shown = now_ms();
		put32(b, fc.size);
		memcpy(b + 4, remote, strlen(remote));
		oq_put(CH_REQ, 'P', fc.id, b, 4 + (uint32_t)strlen(remote));
	} else if (!strcmp(v, "speed") && argc <= 2) {
		long kb = argc > 1 ? num(argv[1]) : 64;
		if (kb < 1 || kb > 65536) {
			say(c, "zlink: speed takes a size in KB, 1-65536\n");
			done(c, 2);
			return;
		}
		if (!link_ready(c, 0)) return;
		fc.op = FC_SPEED;
		fc.phase = PH_DATA;
		fc.c = c;
		fc.id = ++next_id;
		fc.size = (uint32_t)kb * 1024u;
		fc.moved = 0;
		fc.t0 = now_ms();
	} else {
		say(c, "usage: zlink start [-f | -w] [-m auto|soft|stream] [-r KBIT] [PORT [X Y]]\n"
		       "       zlink stop | status | ls [PATH] | speed [KB]\n"
		       "       zlink get REMOTE [LOCAL] | put LOCAL [REMOTE]\n"
		       "       zlink loop [-i] [-r KBIT] PORT TX RX\n"
		       "       term: port zlink0 -- a shell on the other machine\n");
		done(c, 2);
	}
}

// Sending, for put and speed: as much as the queue takes.
static bool fc_pump(void) {
	bool any = false;
	if (fc.op == FC_PUT && fc.phase == PH_DATA) {
		while (oq_free() > OQ_RESERVE) {
			uint8_t *p = oq_slot();
			int n = fs_read_chunk(fc.h, p + 2, ZL_PAYLOAD - 2);
			if (n < 0) {
				fc_abort();
				say(fc.c, "zlink: reading the file failed\n");
				done(fc.c, 1);
				return true;
			}
			if (n == 0) {
				uint8_t b[4];
				put32(b, fc.crc);
				oq_put(CH_REQ, 'E', fc.id, b, 4);
				fc.phase = PH_FINAL;
				break;
			}
			p[0] = 'D';
			p[1] = fc.id;
			oq_commit(CH_REQ, (uint16_t)(n + 2));
			fc.crc = zl_crc32_update(fc.crc, p + 2, (uint32_t)n);
			fc.moved += (uint32_t)n;
			any = true;
		}
	} else if (fc.op == FC_SPEED && fc.phase == PH_DATA) {
		while (oq_free() > OQ_RESERVE && fc.moved < fc.size) {
			uint8_t *p = oq_slot();
			uint32_t n = fc.size - fc.moved;
			if (n > ZL_PAYLOAD - 2) n = ZL_PAYLOAD - 2;
			p[0] = 'B';
			p[1] = fc.id;
			memset(p + 2, 0x55, n);
			oq_commit(CH_REQ, (uint16_t)(n + 2));
			fc.moved += n;
			any = true;
		}
		if (fc.moved == fc.size && oq_put(CH_REQ, 'b', fc.id, NULL, 0)) fc.phase = PH_FINAL;
	}
	// a line every few seconds on a long transfer
	if ((fc.op == FC_PUT || fc.op == FC_GET) && fc.moved && now_ms() - fc.shown > 5000 &&
	    say_room(fc.c, 64)) {
		fc.shown = now_ms();
		say(fc.c, "  ");
		sayn(fc.c, fc.moved);
		if (fc.size) {
			say(fc.c, " of ");
			sayn(fc.c, fc.size);
		}
		say(fc.c, " bytes\n");
	}
	return any;
}

// A response, on channel 2. false: not now, ask again.
static bool on_resp(const uint8_t *p, int n) {
	if (n < 2 || fc.op == FC_NONE || p[1] != fc.id) return true;	// stale
	const uint8_t *b = p + 2;
	int bn = n - 2;
	char text[ZL_PAYLOAD];

	if (p[0] == 'X') {
		memcpy(text, b, (size_t)bn);
		text[bn] = 0;
		fc_fail(text);
		return true;
	}
	switch (fc.op) {
	case FC_LS:
		if (p[0] == 'T') {
			if (!say_room(fc.c, (uint32_t)bn)) return false;
			memcpy(text, b, (size_t)bn);
			text[bn] = 0;
			say(fc.c, text);
		} else if (p[0] == 'K') {
			fc_end(0);
		}
		break;
	case FC_GET:
		if (p[0] == 'S' && bn >= 4 && fc.phase == PH_WAIT) {
			fc.size = get32(b);
			fc.h = fs_open_write(fc.local);
			if (fc.h < 0) {
				fc_abort();
				say(fc.c, "zlink: ");
				say(fc.c, fc.local);
				say(fc.c, ": cannot write it\n");
				done(fc.c, 1);
				break;
			}
			fc.phase = PH_DATA;
		} else if (p[0] == 'D' && fc.phase == PH_DATA) {
			if (fs_write_chunk(fc.h, b, bn) != bn) {
				fc_abort();
				say(fc.c, "zlink: writing the file failed (full?)\n");
				done(fc.c, 1);
				break;
			}
			fc.crc = zl_crc32_update(fc.crc, b, (uint32_t)bn);
			fc.moved += (uint32_t)bn;
		} else if (p[0] == 'E' && bn >= 4 && fc.phase == PH_DATA) {
			if (get32(b) != fc.crc || fc.moved != fc.size) {
				fc_fail("the file arrived damaged (CRC or size) -- removed");
				break;
			}
			say(fc.c, fc.local);
			say(fc.c, ": ");
			report(fc.c, fc.moved, now_ms() - fc.t0);
			fc_end(0);
		}
		break;
	case FC_PUT:
		if (p[0] == 'K' && fc.phase == PH_WAIT) {
			fc.phase = PH_DATA;
		} else if (p[0] == 'K' && fc.phase == PH_FINAL) {
			report(fc.c, fc.moved, now_ms() - fc.t0);
			fc_end(0);
		}
		break;
	case FC_SPEED:
		if (p[0] == 'R' && bn >= 4 && fc.phase == PH_FINAL) {
			uint32_t got = get32(b);
			say(fc.c, "zlink: sent ");
			report(fc.c, got, now_ms() - fc.t0);
			if (got != fc.size) say(fc.c, "zlink: the count is wrong -- a bug\n");
			fc_end(got == fc.size ? 0 : 1);
		}
		break;
	}
	return true;
}

// -- serving the other machine's requests ---------------------------------------

enum { FS_NONE, FS_LS, FS_GET, FS_PUT };

static struct {
	int op;
	uint8_t id;
	int h;
	char path[ZL_PATH + 1];
	uint32_t size, moved, crc;
	char text[2048];		// a listing, while it goes out
	uint16_t text_n, text_at;
} fs = { .h = -1 };

static uint32_t bench_bytes;

static void fs_reset(void) {
	if (fs.h >= 0) {
		fs_close_handle(fs.h);
		if (fs.op == FS_PUT) fs_unlink(fs.path);
	}
	fs.h = -1;
	fs.op = FS_NONE;
}

static void fs_refuse(const char *why) {
	fs_reset();
	oq_str(CH_RESP, 'X', fs.id, why);
}

static void text_add(const char *s) {
	while (*s && fs.text_n < sizeof fs.text - 1) fs.text[fs.text_n++] = *s++;
}

static void list_into_text(const char *path) {
	static char names[1536];
	static z_fs_info_t info[48];
	uint32_t count = 0, trunc = 0;
	char b[12];
	fs.text_n = fs.text_at = 0;
	if (!fs_list_ex(path, names, sizeof names, info, 48, &count, &trunc)) {
		text_add(path);
		text_add(": no such directory\n");
		return;
	}
	const char *p = names;
	for (uint32_t i = 0; i < count; i++, p += strlen(p) + 1) {
		const char *base = strrchr(p, '/');
		base = base ? base + 1 : p;
		if (info[i].type == Z_FS_TYPE_DIR) {
			text_add(base);
			text_add("/\n");
		} else {
			text_add(base);
			text_add("  ");
			text_add(u2s(info[i].size, b));
			text_add("\n");
		}
	}
	if (trunc) text_add("...\n");
	if (!count) text_add("(empty)\n");
}

// A request, on channel 1.
static bool on_req(const uint8_t *p, int n) {
	char path[ZL_PAYLOAD];
	if (n < 2) return true;
	uint8_t type = p[0], id = p[1];
	const uint8_t *b = p + 2;
	int bn = n - 2;

	if (type == 'B') {
		bench_bytes += (uint32_t)bn;
		return true;
	}
	if (oq_free() < 2) return false;		// room for any answer

	// What belongs to a running transfer. After a failure the rest of
	// it is dropped below, as for anything abandoned.
	if ((type == 'D' || type == 'E') && id == fs.id && fs.op == FS_PUT) {
		if (type == 'D') {
			if (fs_write_chunk(fs.h, b, bn) != bn) {
				fs_refuse("writing the file failed on the other machine (full?)");
				return true;
			}
			fs.crc = zl_crc32_update(fs.crc, b, (uint32_t)bn);
			fs.moved += (uint32_t)bn;
			return true;
		}
		fs_close_handle(fs.h);
		fs.h = -1;
		if (bn < 4 || get32(b) != fs.crc || fs.moved != fs.size) {
			fs_unlink(fs.path);
			fs.op = FS_NONE;
			oq_str(CH_RESP, 'X', id, "the file arrived damaged (CRC or size) -- removed");
			return true;
		}
		fs.op = FS_NONE;
		logs("received ", fs.path);
		oq_put(CH_RESP, 'K', id, NULL, 0);
		return true;
	}
	if (type == 'b') {
		uint8_t c[4];
		put32(c, bench_bytes);
		bench_bytes = 0;
		oq_put(CH_RESP, 'R', id, c, 4);
		return true;
	}
	if (type == 'D' || type == 'E') return true;	// for something abandoned

	// A new request: whatever was running is over (the client has
	// given up on it, or it would not ask).
	fs_reset();
	fs.id = id;
	bench_bytes = 0;
	if (type == 'A') return true;
	{
		int skip = type == 'P' ? 4 : 0;
		if (bn < skip) return true;
		if (bn - skip > ZL_PATH) {
			fs_refuse("a path is too long (200 bytes at most)");
			return true;
		}
		memcpy(path, b + skip, (size_t)(bn - skip));
		path[bn - skip] = 0;
	}
	if (changing()) {
		fs_refuse("busy -- changing to a faster link; try again in a few seconds");
		return true;
	}

	if (!g_files) {
		fs_refuse("the other machine does not serve files (it would need zlink start -f)");
		return true;
	}
	if (type == 'L') {
		list_into_text(path[0] ? path : "/");
		fs.op = FS_LS;
	} else if (type == 'G') {
		z_fs_info_t info;
		uint8_t s[4];
		if (!fs_stat(path, &info) || info.type != Z_FS_TYPE_FILE ||
		    (fs.h = fs_open_read(path)) < 0) {
			fs.h = -1;
			fs_refuse("no such file on the other machine");
			return true;
		}
		fs.op = FS_GET;
		fs.crc = 0;
		fs.moved = 0;
		strcpy(fs.path, path);
		put32(s, info.size);
		oq_put(CH_RESP, 'S', id, s, 4);
		logs("sending ", path);
	} else if (type == 'P') {
		if (g_files < 2) {
			fs_refuse("the other machine only lends its files to read (it would need zlink start -w)");
			return true;
		}
		if (!path[0] || (fs.h = fs_open_write(path)) < 0) {
			fs.h = -1;
			fs_refuse("cannot write that file on the other machine");
			return true;
		}
		fs.op = FS_PUT;
		fs.size = get32(b);
		fs.crc = 0;
		fs.moved = 0;
		strcpy(fs.path, path);
		oq_put(CH_RESP, 'K', id, NULL, 0);
	}
	return true;
}

static bool fs_pump(void) {
	bool any = false;
	if (fs.op == FS_LS) {
		while (oq_free() > OQ_RESERVE && fs.text_at < fs.text_n) {
			uint16_t k = (uint16_t)(fs.text_n - fs.text_at);
			if (k > ZL_PAYLOAD - 2) k = ZL_PAYLOAD - 2;
			oq_put(CH_RESP, 'T', fs.id, fs.text + fs.text_at, k);
			fs.text_at = (uint16_t)(fs.text_at + k);
			any = true;
		}
		if (fs.text_at == fs.text_n && oq_put(CH_RESP, 'K', fs.id, NULL, 0)) fs.op = FS_NONE;
	} else if (fs.op == FS_GET) {
		while (oq_free() > OQ_RESERVE) {
			uint8_t *p = oq_slot();
			int n = fs_read_chunk(fs.h, p + 2, ZL_PAYLOAD - 2);
			if (n < 0) {
				fs_refuse("reading the file failed on the other machine");
				return true;
			}
			if (n == 0) {
				uint8_t c[4];
				put32(c, fs.crc);
				oq_put(CH_RESP, 'E', fs.id, c, 4);
				fs_close_handle(fs.h);
				fs.h = -1;
				fs.op = FS_NONE;
				break;
			}
			p[0] = 'D';
			p[1] = fs.id;
			oq_commit(CH_RESP, (uint16_t)(n + 2));
			fs.crc = zl_crc32_update(fs.crc, p + 2, (uint32_t)n);
			any = true;
		}
	}
	return any;
}

// -- the shell: this machine's term to the other machine's shell -------------------

static struct {
	bool on;
	z_port_t port;			// term, connected to us
	inq_t in;				// its keys, for the link
} cs;

// A connection id per session, from 100 (the command line's are 1-3): a
// term that reconnects still acks the last session's output, and those
// acks must not free the new session's sends. That session's port is
// kept here while they come.
static uint32_t cs_next_id = 100;
static z_port_t cs_old;

static void cs_close(const char *why) {
	if (!cs.on) return;
	if (why) z_port_send(&cs.port, why, (uint32_t)strlen(why));
	z_port_close(&cs.port);
	inq_reset(&cs.in);
	cs.on = false;
}

// -- the shell: the other machine's term to a shell here -------------------------

enum { SS_NONE, SS_LOGIN, SS_LAUNCH, SS_CONNECT, SS_OPEN };

static struct {
	int state;
	char pw[64];
	uint8_t pw_n, tries;
	uint32_t pid, until;
	char target[8];
	z_port_t port;			// us, connected to posix or repl
	inq_t out;				// its output, for the link
	char ahead[256];		// typed while the shell was starting
	uint16_t ahead_n;
	z_obj_table_t id_tab;
	z_obj_t id_k[2], id_v[2];
} ss;

// Who this is, for the shell: zport.h, "Who is connecting". Static
// storage: the provider reads it after the CONNECT has gone.
static z_obj_t identity(void) {
	z_obj_t o;
	ss.id_k[0].type = ss.id_k[1].type = Z_STR;
	ss.id_k[0].val.str = (char *)Z_PORT_ID_TRANSPORT;
	ss.id_k[1].val.str = (char *)Z_PORT_ID_AUTH;
	ss.id_v[0].type = ss.id_v[1].type = Z_STR;
	ss.id_v[0].val.str = (char *)"zlink";
	ss.id_v[1].val.str = (char *)Z_PORT_AUTH_SYSTEM;
	ss.id_tab.len = 2;
	ss.id_tab.a = ss.id_k;
	ss.id_tab.b = ss.id_v;
	o.type = Z_MAP;
	o.val.ptr = &ss.id_tab;
	return o;
}

static void ss_say(const char *s) {
	oq_put(CH_SH_OUT, 'D', -1, s, (uint32_t)strlen(s));
}

// End the session here. `why` goes to the other machine's term (NULL:
// it already knows). The shell's port is closed unless it closed first.
static void ss_end(const char *why) {
	if (ss.state == SS_OPEN && ss.port.connected) z_port_close(&ss.port);
	inq_reset(&ss.out);
	if (ss.state != SS_NONE && why) oq_str(CH_SH_OUT, 'C', -1, why);
	memset(ss.pw, 0, sizeof ss.pw);
	ss.pw_n = 0;
	ss.ahead_n = 0;
	ss.state = SS_NONE;
}

static void ss_prompt(void) {
	ss_say("\r\nzlink: password: ");
}

static void ss_connect(void) {
	uint32_t pid;
	const char *t = NULL;
	if (z_pid_lookup("posix0", &pid)) t = "posix0";
	else if (z_pid_lookup("repl0", &pid)) t = "repl0";
	if (t) {
		strcpy(ss.target, t);
		ss.pid = pid;
		if (ss.port.pending_count) z_port_forget(&ss.port);
		memset(&ss.port, 0, sizeof ss.port);
		ss.state = SS_CONNECT;
		ss.until = now_ms() + 5000;
		z_msg_new_send(pid, Z_PORT_CONNECT, 0, identity());
		return;
	}
	// Nothing running: start a shell, posix first (4 MB), repl if
	// posix will not start.
	if (z_proc_run("posix")) strcpy(ss.target, "posix0");
	else if (z_proc_run("repl")) strcpy(ss.target, "repl0");
	else {
		ss_end("zlink: could not start a shell on the other machine\r\n");
		return;
	}
	ss.state = SS_LAUNCH;
	ss.until = now_ms() + 15000;
}

static void ss_login_key(uint8_t ch) {
	if (ch == '\r' || ch == '\n') {
		uint32_t wait = 0;
		if (!ss.pw_n) { ss_prompt(); return; }
		int rc = z_auth_check(ss.pw, ss.pw_n, &wait);
		memset(ss.pw, 0, sizeof ss.pw);
		ss.pw_n = 0;
		if (rc == Z_AUTH_OK) {
			ss_say("\r\n");
			logs("shell: logged in", NULL);
			ss_connect();
		} else if (++ss.tries >= 3) {
			logs("shell: three wrong passwords", NULL);
			ss_end("\r\nzlink: wrong password\r\n");
		} else if (rc == Z_AUTH_E_WAIT) {
			char b[12], m[80];
			strcpy(m, "\r\nzlink: too many tries -- wait ");
			strcat(m, u2s(wait / 1000 + 1, b));
			strcat(m, " s\r\nzlink: password: ");
			ss_say(m);
		} else {
			ss_say("\r\nzlink: wrong password\r\nzlink: password: ");
		}
	} else if (ch == 3 || ch == 4) {
		ss_end("\r\n");
	} else if (ch == 8 || ch == 127) {
		if (ss.pw_n) ss.pw_n--;
	} else if (ss.pw_n < sizeof ss.pw) {
		ss.pw[ss.pw_n++] = (char)ch;
	}
}

// Shell traffic in, on channels 3 (we serve) and 4 (we are the client).
static bool on_shell(uint8_t ch, const uint8_t *p, int n) {
	if (n < 1) return true;
	if (ch == CH_SH_OUT) {
		if (!cs.on) return true;
		if (p[0] == 'D') {
			if (cs.port.pending_count >= Z_PORT_MAX_PENDING_SENDS) return false;
			if (n > 1 && z_port_send(&cs.port, p + 1, (uint32_t)(n - 1)) != Z_OK) return false;
		} else if (p[0] == 'C') {
			char why[ZL_PAYLOAD];
			memcpy(why, p + 1, (size_t)(n - 1));
			why[n - 1] = 0;
			cs_close(why[0] ? why : "\r\n[zlink: the other machine closed the shell]\r\n");
		}
		return true;
	}
	// channel 3
	if (p[0] == 'O') {
		if (oq_free() < 2) return false;
		ss_end(NULL);
		if (changing()) {
			oq_str(CH_SH_OUT, 'C', -1, "zlink: busy -- changing to a faster link; "
			       "try again in a few seconds\r\n");
			return true;
		}
		if (!has_password()) {
			oq_str(CH_SH_OUT, 'C', -1, "zlink: the other machine has no password, so it "
			       "gives no shell -- set one there first (settings)\r\n");
			return true;
		}
		ss.state = SS_LOGIN;
		ss.tries = 0;
		ss_prompt();
	} else if (p[0] == 'C') {
		ss_end(NULL);			// the client left: nothing to tell it
	} else if (p[0] == 'D') {
		if (ss.state == SS_LOGIN) {
			if (oq_free() < 2) return false;
			int i;
			// each Enter answers with one message; a paste of many
			// is cut short rather than overrun the queue
			for (i = 1; i < n && ss.state == SS_LOGIN && oq_free() >= 2; i++) ss_login_key(p[i]);
			// what follows the password in the same message: typed ahead
			for (; i < n && ss.state != SS_NONE && ss.ahead_n < sizeof ss.ahead; i++)
				ss.ahead[ss.ahead_n++] = (char)p[i];
		} else if (ss.state == SS_OPEN) {
			if (ss.port.pending_count >= Z_PORT_MAX_PENDING_SENDS) return false;
			if (z_port_send(&ss.port, p + 1, (uint32_t)(n - 1)) != Z_OK) return false;
		} else if (ss.state == SS_LAUNCH || ss.state == SS_CONNECT) {
			// typed ahead of the shell: kept for when it answers
			for (int i = 1; i < n && ss.ahead_n < sizeof ss.ahead; i++) ss.ahead[ss.ahead_n++] = (char)p[i];
		}
	}
	return true;
}

// -- the upgrade --------------------------------------------------------------

enum { UP_IDLE, UP_ASKED, UP_ACCEPTED, UP_SWITCH, UP_TRY, UP_DONE, UP_QUIET };

// The changeover's times, ms -- upgrade_run() says why each is what it is
#define UP_ANSWER_BY   400		// primary: the answer must come by then
#define UP_FRESH       100		// secondary: an offer older than this is refused
#define UP_LET_GO      300		// secondary: wires released by then
#define UP_ENGINE_ON  1000		// both: engine on, after S or T
#define UP_TRY_MS     2500		// both: for the stream link to come up
#define UP_NO_ANSWER  5000		// primary: quiet after an unanswered offer
#define UP_BACK       (ZL_DEAD_MS + 1500)	// quiet on any way back to soft

static struct {
	int state;
	uint32_t t, next_try, stream_since;
	uint32_t quiet_ms;
	uint16_t rate;
	bool primary, crossed;
} up;

// A local or remote session is open, or a command is running: no
// changing wires under it.
static bool busy(void) {
	return fc.op != FC_NONE || fs.op != FS_NONE || cs.on || ss.state != SS_NONE || oq_n;
}

// Between an offer and the stream link: no new work, which could stall
// this loop past the changeover's margins (file I/O, a password check).
static bool changing(void) {
	return up.state == UP_ASKED || up.state == UP_ACCEPTED || up.state == UP_SWITCH ||
	       up.state == UP_TRY || up.state == UP_QUIET;
}

// The first rate in the ladder below `kbit`: where to try next.
static void cap_below(uint16_t kbit) {
	while (g_cap < RATES_N && rates[g_cap] >= kbit) g_cap++;
	if (g_cap == RATES_N) logs("the stream link failed at every rate: staying with soft", NULL);
}

// Let go of everything -- engine, both wires -- for `ms`, then start
// soft again.
static void go_quiet(uint32_t now, uint32_t ms) {
	engine_free();
	soft_start();			// wires released, link down; not polled until the end
	up.state = UP_QUIET;
	up.t = now;
	up.quiet_ms = ms;
}

static void revert(uint32_t now, bool slower) {
	char b[12];
	if (slower) cap_below(up.rate);
	logs("back to soft: no stream link at kbit/s ", u2s(up.rate, b));
	go_quiet(now, UP_BACK);
}

// Channel 0. `at`: when the message arrived.
static bool on_ctl(const uint8_t *p, int n, uint32_t at, uint32_t now) {
	if (n < 1) return true;
	if (oq_free() < 1) return false;
	if (p[0] == 'U' && n >= 3) {
		uint16_t r = (uint16_t)get16(p + 1);
		uint8_t b[2];
		int e;
		// We are the secondary. Only a FRESH offer, on a live soft
		// link, with nothing else going on: upgrade_run() needs S to be
		// close to the offer's own time.
		if (up.state != UP_IDLE || g_tr != T_SOFT || g_mode != M_AUTO || g_cap >= RATES_N ||
		    !zl_up(&L) || now - at > UP_FRESH || !stream_built() || busy() ||
		    (e = z_gs_claim(Z_GS_ANY)) < 0) {
			oq_put(CH_CTL, 'n', -1, NULL, 0);
			return true;
		}
		g_engine = e;
		if (r > rates[g_cap]) r = rates[g_cap];
		put16(b, r);
#ifdef ZLINK_HOST_TEST
		// tests/test_zlink_app.c: the answer is lost on the way
		if (zl_test_lose_answer > 0) zl_test_lose_answer--;
		else
#endif
		oq_put(CH_CTL, 'u', -1, b, 2);
		up.state = UP_ACCEPTED;
		up.t = now;				// S
		up.rate = r;
		up.primary = false;
		up.crossed = zl_soft_crossed(&S);
	} else if (p[0] == 'u' && n >= 3 && up.state == UP_ASKED) {
		// We are the primary, and the answer is in time (upgrade_run()
		// leaves UP_ASKED at P + UP_ANSWER_BY): let go of the wires now.
		up.rate = (uint16_t)get16(p + 1);
		up.primary = true;
		up.crossed = zl_soft_crossed(&S);
		zl_soft_stop(&S);
		up.state = UP_SWITCH;
		up.t = now;				// T
	} else if (p[0] == 'n' && up.state == UP_ASKED) {
		engine_free();
		up.state = UP_IDLE;
		up.next_try = now + 30000;
	}
	return true;
}

// The changeover, in time. The rule it keeps: a wire goes from pulled
// by soft, to released by both, to driven by ONE engine -- never two
// drivers at once, and never soft pulling a wire an engine drives.
// Times are each machine's own. P: the primary sends the offer. S: the
// secondary handles it. T: the primary handles the answer.
//
//   S >= P          the offer cannot arrive before it was sent
//   S <= P + 500    the primary sends nothing after P + 400 (it has
//                   the answer or is quiet), and the secondary refuses
//                   an offer more than UP_FRESH old
//   S <= T <= P + 400
//
// SECONDARY: lets go by S + 300; engine on at S + 1000; off by S + 3500
// if the link does not come up.
// PRIMARY with the answer: lets go at T; engine on at T + 1000.
// PRIMARY without one by P + 400: quiet -- wires released -- until
// P + 5400, in case the answer was lost and the secondary committed.
//
// So: the primary has let go by P + 400 < S + 1000 (600 ms of margin
// for a stalled loop); the secondary by S + 300 < T + 1000 (700 ms);
// a committed secondary's engine is off by P + 4000 < P + 5400. And
// every way back to soft is quiet for ZL_DEAD_MS + 1.5 s: the other
// machine, if still on its engine, hears the silence and lets go.
// tests/test_zlink_app.c fails on any moment that breaks the rule.
static void upgrade_run(uint32_t now) {
	switch (up.state) {
	case UP_IDLE:
		if (g_tr == T_SOFT && g_mode == M_AUTO && zl_up(&L) && L.primary && g_cap < RATES_N &&
		    (L.my_caps & ZL_CAP_STREAM) && (zl_peer_caps(&L) & ZL_CAP_STREAM) &&
		    (int32_t)(now - up.next_try) >= 0 && !busy() && zl_tx_empty(&L)) {
			uint8_t b[2];
			int e = z_gs_claim(Z_GS_ANY);
			if (e < 0) {
				up.next_try = now + 30000;
				return;
			}
			g_engine = e;
			put16(b, rates[g_cap]);
			oq_put(CH_CTL, 'U', -1, b, 2);
			up.state = UP_ASKED;
			up.t = now;			// P
		}
		break;
	case UP_ASKED:
		if (now - up.t > UP_ANSWER_BY) {
			logs("no answer to the stream offer: quiet for a moment", NULL);
			go_quiet(now, UP_NO_ANSWER);
		}
		break;
	case UP_ACCEPTED:
		if ((zl_tx_empty(&L) && !oq_n) || now - up.t > UP_LET_GO) {
			zl_soft_stop(&S);
			up.state = UP_SWITCH;
		}
		break;
	case UP_SWITCH:
		if (now - up.t >= UP_ENGINE_ON) {
			int tx = g_x, rx = g_y;
			// the primary sends on X; the secondary on the wire the
			// primary's X does not reach
			if (!up.primary && !up.crossed) { tx = g_y; rx = g_x; }
			if (!stream_start(tx, rx, up.rate)) {
				logs("the engine would not take the pins: staying with soft", NULL);
				g_cap = RATES_N;
				go_quiet(now, UP_BACK);
				return;
			}
			up.state = UP_TRY;
			up.t = now;
		}
		break;
	case UP_TRY:
		if (zl_up(&L)) {
			char b[12];
			logs("stream link up at kbit/s ", u2s(up.rate, b));
			up.state = UP_DONE;
			up.stream_since = now;
		} else if (now - up.t > UP_TRY_MS) {
			revert(now, true);
		}
		break;
	case UP_DONE:
		// The stream went quiet: the other machine stopped, or the
		// cable came out. Soft finds it again either way; a stream
		// that held only a little while gets a slower rate.
		if (!zl_up(&L)) revert(now, now - up.stream_since < 10000);
		break;
	case UP_QUIET:
		if (now - up.t >= up.quiet_ms) {
			soft_start();
			up.state = UP_IDLE;
			up.next_try = now + 1000;
		}
		break;
	}
}

// Whether the link layer may run now: not while the wires change hands.
static bool link_live(void) {
	return up.state != UP_SWITCH && up.state != UP_QUIET;
}

// -- status ---------------------------------------------------------------------

static void cmd_status(int c) {
	uint8_t pc = zl_peer_caps(&L);
	say(c, "zlink: ");
	say(c, zl_up(&L) ? "up" : "down");
	say(c, ", ");
	if (g_tr == T_STREAM) {
		say(c, "stream at ");
		sayn(c, g_rate);
		say(c, " kbit/s");
	} else {
		say(c, "soft");
	}
	say(c, ", GPIO port ");
	sayn(c, (uint32_t)g_port);
	say(c, g_mode == M_STREAM ? " TX pin " : " pins ");
	sayn(c, (uint32_t)g_x);
	say(c, g_mode == M_STREAM ? " RX pin " : " and ");
	sayn(c, (uint32_t)g_y);
	say(c, "\n");
	if (zl_up(&L)) {
		say(c, "  this machine is the ");
		say(c, L.primary ? "primary" : "secondary");
		if (g_tr == T_SOFT) say(c, zl_soft_crossed(&S) ? "; the cable is crossed" : "; the cable is straight");
		say(c, "\n  the other machine: ");
		say(c, pc & ZL_CAP_WRITE ? "files to read and write" :
		       pc & ZL_CAP_FILES ? "files to read" : "no files");
		say(c, pc & ZL_CAP_SHELL ? ", a shell" : ", no shell (no password set)");
		say(c, pc & ZL_CAP_STREAM ? ", a stream engine\n" : ", no stream engine\n");
	} else if (g_mode != M_STREAM) {
		say(c, "  waiting for the other machine: its zlink started, wires X-X and Y-Y "
		       "(or crossed) and ground\n");
	}
	say(c, "  this machine: ");
	say(c, g_files == 2 ? "files to read and write" : g_files ? "files to read" : "no files");
	say(c, has_password() ? ", a shell" : ", no shell (no password set)");
	say(c, stream_built() ? (g_cap < RATES_N || g_mode == M_STREAM ? ", a stream engine\n" :
	                         ", a stream engine (given up on)\n") : ", no stream engine\n");
	say(c, "  frames ");
	sayn(c, L.st.tx_frames);
	say(c, " out, ");
	sayn(c, L.st.rx_frames);
	say(c, " in; resent ");
	sayn(c, L.st.retransmits);
	say(c, "; bad ");
	sayn(c, L.st.crc_errors);
	say(c, "; went up ");
	sayn(c, L.st.ups);
	say(c, " times\n");
}

// -- messages from this machine ----------------------------------------------------

static int cli_find(const z_msg_t *m) {
	for (int c = 0; c < NCLI; c++)
		if (cli[c].used && cli[c].port.peer_pid == m->from && cli[c].port.conn_id == m->tag)
			return c;
	return -1;
}

static void on_connect(z_msg_t *m) {
	z_port_ident_t id;
	z_port_ident(m, &id);
	if (m->obj.type == Z_STR) {
		// a command from the zlink client
		static char line[256];
		int c;
		if (id.remote) {
			z_port_refuse(m, "zlink: commands are for this machine's own processes");
			return;
		}
		for (c = 0; c < NCLI && cli[c].used; c++)
			;
		if (c == NCLI) {
			z_port_refuse(m, "zlink: too many commands at once");
			return;
		}
		strncpy(line, m->obj.val.str ? m->obj.val.str : "", sizeof line - 1);
		line[sizeof line - 1] = 0;
		memset(&cli[c], 0, sizeof cli[c]);
		cli[c].used = true;
		z_port_accept(&cli[c].port, m, CLI_ID0 + (uint32_t)c);
		cmd(c, line);
		return;
	}
	// term: a shell on the other machine
	if (z_port_refuse_unauthenticated(m, "zlink")) return;
	if (cs.on) {
		z_port_refuse(m, "zlink: a shell over zlink is already open");
		return;
	}
	if (!zl_up(&L)) {
		z_port_refuse(m, "zlink: the link is down (zlink status)");
		return;
	}
	if (!(zl_peer_caps(&L) & ZL_CAP_SHELL)) {
		z_port_refuse(m, "zlink: the other machine gives no shell (it has no password)");
		return;
	}
	if (changing() || oq_free() < 2) {
		z_port_refuse(m, "zlink: busy -- changing to a faster link; try again in a few seconds");
		return;
	}
	// the last session's sends still out: their acks may yet come
	if (cs.port.pending_count) {
		if (cs_old.pending_count) z_port_forget(&cs_old);	// two sessions ago: given up on
		cs_old = cs.port;
	}
	memset(&cs, 0, sizeof cs);
	z_port_accept(&cs.port, m, cs_next_id++);
	cs.on = true;
	oq_put(CH_SH_IN, 'O', -1, NULL, 0);
}

static void on_msg(z_msg_t *m) {
	int c;
	switch (m->subject) {
	case Z_PORT_CONNECT:
		on_connect(m);
		break;
	case Z_PORT_CONNECTED:
		if (ss.state == SS_CONNECT && m->from == ss.pid && m->obj.type == Z_UINT32) {
			ss.port.peer_pid = m->from;
			ss.port.conn_id = m->obj.val.uint32;
			ss.port.connected = true;
			ss.state = SS_OPEN;
			if (ss.ahead_n) z_port_send(&ss.port, ss.ahead, ss.ahead_n);
			ss.ahead_n = 0;
		} else if (m->obj.type == Z_UINT32) {
			// an answer to a session that has gone: close it
			z_msg_new_send(m->from, Z_PORT_CLOSE, m->obj.val.uint32, z_obj_none());
		}
		break;
	case Z_PORT_REFUSED:
		if (ss.state == SS_CONNECT && m->from == ss.pid)
			ss_end("zlink: the shell on the other machine refused the connection\r\n");
		break;
	case Z_PORT_DATA:
		if (cs.on && m->from == cs.port.peer_pid && m->tag == cs.port.conn_id) {
			inq_in(&cs.in, m);
		} else if (ss.state == SS_OPEN && m->from == ss.port.peer_pid && m->tag == ss.port.conn_id) {
			inq_in(&ss.out, m);
		} else if (cli_find(m) >= 0) {
			z_port_send_ack(m);
		} else {
			z_port_reject_stranger(m);
		}
		break;
	case Z_PORT_DATA_ACK:
		if ((c = cli_find(m)) >= 0) z_port_handle_ack(&cli[c].port, m);
		else if (m->from == cs.port.peer_pid && m->tag == cs.port.conn_id)
			cs.on ? z_port_handle_ack(&cs.port, m) : z_port_handle_ack_closed(&cs.port, m);
		else if (m->from == cs_old.peer_pid && m->tag == cs_old.conn_id)
			z_port_handle_ack_closed(&cs_old, m);
		else if (m->from == ss.port.peer_pid && m->tag == ss.port.conn_id)
			ss.state == SS_OPEN ? z_port_handle_ack(&ss.port, m) : z_port_handle_ack_closed(&ss.port, m);
		break;
	case Z_PORT_CLOSE:
		if (cs.on && m->from == cs.port.peer_pid && m->tag == cs.port.conn_id) {
			// term may still be reading what we sent: its acks
			// still come (z_port_handle_ack_closed())
			cs.port.connected = false;
			inq_reset(&cs.in);
			cs.on = false;
			oq_put(CH_SH_IN, 'C', -1, NULL, 0);
		} else if (ss.state == SS_OPEN && m->from == ss.port.peer_pid && m->tag == ss.port.conn_id) {
			ss.port.connected = false;	// closed from its side
			ss_end("\r\n[zlink: the shell ended]\r\n");
		} else if ((c = cli_find(m)) >= 0) {
			// the client went away (Ctrl-C): drop its command
			if (fc.op != FC_NONE && fc.c == c) fc_abort();
			z_port_forget(&cli[c].port);
			cli[c].used = false;
		}
		break;
	}
}

// Once a second: processes that died without a CLOSE, a shell that is
// slow to start.
static void periodic(uint32_t now) {
	static uint32_t last;
	if (ss.state == SS_LAUNCH) {
		uint32_t pid;
		if (z_pid_lookup(ss.target, &pid)) {
			ss.pid = pid;
			if (ss.port.pending_count) z_port_forget(&ss.port);
			memset(&ss.port, 0, sizeof ss.port);
			ss.state = SS_CONNECT;
			ss.until = now + 5000;
			z_msg_new_send(pid, Z_PORT_CONNECT, 0, identity());
		} else if ((int32_t)(now - ss.until) > 0) {
			ss_end("zlink: the shell on the other machine did not start\r\n");
		}
	}
	if (ss.state == SS_CONNECT && (int32_t)(now - ss.until) > 0)
		ss_end("zlink: the shell on the other machine did not answer\r\n");
	if (now - last < 1000) return;
	last = now;
	for (int c = 0; c < NCLI; c++) {
		if (cli[c].used && z_port_peer_gone(&cli[c].port)) {
			if (fc.op != FC_NONE && fc.c == c) fc_abort();
			z_port_forget(&cli[c].port);
			cli[c].used = false;
		}
	}
	if (cs.on && z_port_peer_gone(&cs.port)) {
		z_port_forget(&cs.port);
		inq_reset(&cs.in);
		cs.on = false;
		oq_put(CH_SH_IN, 'C', -1, NULL, 0);
	}
	if (ss.state == SS_OPEN && z_port_peer_gone(&ss.port)) {
		z_port_forget(&ss.port);
		ss_end("\r\n[zlink: the shell ended]\r\n");
	}
}

// The message being handled: kept until its handler can take it.
static struct { bool full; uint8_t ch; int n; uint32_t at; uint8_t d[ZL_PAYLOAD]; } held;

// The link went down: everything riding on it is over -- including a
// message not yet handled (the link layer drops the rest, zlink.h).
static void link_lost(void) {
	if (fc.op != FC_NONE) fc_fail("the link went down");
	abort_owed = -1;
	fs_reset();
	bench_bytes = 0;
	cs_close("\r\n[zlink: the link went down]\r\n");
	ss_end(NULL);
	oq_h = oq_n = 0;
	held.full = false;
}

// Everything that moves. true if anything did.
static bool pump(uint32_t now) {
	bool any = false;
	int k;
	for (k = 0; k < 8; k++) {
		bool ok = true;
		if (!held.full) {
			held.n = zl_recv(&L, &held.ch, held.d, sizeof held.d);
			if (held.n < 0) break;
			held.at = zl_recv_time(&L);
			held.full = true;
		}
		switch (held.ch) {
		case CH_CTL: ok = on_ctl(held.d, held.n, held.at, now); break;
		case CH_REQ: ok = on_req(held.d, held.n); break;
		case CH_RESP: ok = on_resp(held.d, held.n); break;
		case CH_SH_IN: case CH_SH_OUT: ok = on_shell(held.ch, held.d, held.n); break;
		}
		if (!ok) break;
		held.full = false;
		any = true;
	}
	if (abort_owed >= 0 && oq_put(CH_REQ, 'A', abort_owed, NULL, 0)) abort_owed = -1;
	if (cs.on && inq_out(&cs.in, CH_SH_IN)) any = true;
	if (ss.state == SS_OPEN && inq_out(&ss.out, CH_SH_OUT)) any = true;
	if (fc_pump()) any = true;
	if (fs_pump()) any = true;
	if (oq_flush()) any = true;
	if (cli_flush()) any = true;
	return any;
}

// Data is moving: a transfer, or the link waiting on an ack.
static bool hot(void) {
	return fc.op != FC_NONE || fs.op == FS_GET || fs.op == FS_PUT || held.full ||
	       oq_n || !zl_tx_empty(&L) || cs.in.cnt || ss.out.cnt;
}

static int serve(int argc, char **argv) {
	char name[16];
	bool was_up = false;

	for (int a = 0; a < argc; a++) {
		if (!strcmp(argv[a], "-f")) g_files = 1;
		else if (!strcmp(argv[a], "-w")) g_files = 2;
		else if (!strcmp(argv[a], "-m") && a + 1 < argc) {
			a++;
			g_mode = !strcmp(argv[a], "soft") ? M_SOFT : !strcmp(argv[a], "stream") ? M_STREAM : M_AUTO;
		} else if (!strcmp(argv[a], "-r") && a + 1 < argc) {
			long r = num(argv[++a]);
			while (g_cap < RATES_N - 1 && rates[g_cap] > r) g_cap++;
		} else if (a + 2 < argc) {
			g_port = (int)num(argv[a]);
			g_x = (int)num(argv[a + 1]);
			g_y = (int)num(argv[a + 2]);
			a += 2;
		} else {
			g_port = (int)num(argv[a]);
		}
	}
	if (pins_bad(g_port, g_x, g_y)) {
		logs("bad pins: ", pins_bad(g_port, g_x, g_y));
		return 2;
	}
	if (!z_pid_register("zlink", name, sizeof name) || strcmp(name, "zlink0")) {
		logs("another zlink is running", NULL);
		return 1;
	}
	if (g_mode == M_STREAM) {
		if (!stream_built() || (g_engine = z_gs_claim(Z_GS_ANY)) < 0 ||
		    !stream_start(g_x, g_y, rates[g_cap])) {
			logs("no stream engine free for -m stream", NULL);
			engine_free();
			return 1;
		}
		up.state = UP_DONE;
		up.rate = rates[g_cap];
		up.stream_since = now_ms();
	} else {
		soft_start();
	}
	logs("started as zlink0", NULL);

	for (;;) {
		z_msg_t m;
		uint32_t now;
		bool active, open = false;
		for (int k = 0; k < 16 && z_msg_read(&m) == Z_OK; k++) on_msg(&m);
		now = now_ms();
		if (link_live()) zl_poll(&L, now);
		if (zl_up(&L) != was_up) {
			was_up = !was_up;
			if (!was_up) link_lost();
			logs(was_up ? "link up, " : "link down", was_up ? (g_tr == T_STREAM ? "stream" : "soft") : NULL);
		}
		if (g_mode == M_AUTO) upgrade_run(now);
		active = pump(now);
		periodic(now);
		if (g_stop) {
			// `zlink stop`: out once its answer has been read
			for (int c = 0; c < NCLI; c++) if (cli[c].used) open = true;
			if (!open) break;
		}
		// Soft: every bit waits for both ends, so while a frame is on
		// the wires, keep at it rather than sleep a tick per bit.
		if (g_tr == T_SOFT && link_live()) {
			uint32_t t0 = now_ms();
			while (zl_soft_busy(&S) && now_ms() - t0 < 8) zl_poll(&L, now_ms());
			if (zl_soft_busy(&S)) active = true;
		}
		// A transfer runs flat out; anything else sleeps a tick.
		if (!active && !hot()) z_proc_wait(1);
	}

	link_lost();
	if (g_tr == T_SOFT) zl_soft_stop(&S);
	engine_free();
	// What we sent term and the shell lives on our heap until they have
	// read it (zport.h): wait for their acks, a second at most.
	for (uint32_t t0 = now_ms(); now_ms() - t0 < 1000 &&
	     (cs.port.pending_count || cs_old.pending_count || ss.port.pending_count);) {
		z_msg_t m;
		while (z_msg_read(&m) == Z_OK) on_msg(&m);
		z_proc_wait(1);
	}
	logs("stopped", NULL);
	return 0;
}

// =========================================================================
// The client
// =========================================================================

static z_port_t out;
static bool out_on;

// To the shell's terminal, \n as \r\n.
static bool out_text(const char *s, uint32_t n) {
	char b[300];
	uint32_t k = 0;
	if (!out_on) {
		for (uint32_t i = 0; i < n; i++) {
			if (s[i] == '\n') uart_putc('\r');
			uart_putc(s[i]);
		}
		return true;
	}
	if (out.pending_count >= Z_PORT_MAX_PENDING_SENDS) return false;
	for (uint32_t i = 0; i < n && k < sizeof b - 1; i++) {
		if (s[i] == '\n') b[k++] = '\r';
		b[k++] = s[i];
	}
	return z_port_send(&out, b, k) == Z_OK;
}

// Wait for everything to be read, then go (zport.h, z_port_drain()).
static void out_close(void) {
	if (!out_on) return;
	z_port_drain(&out, Z_TICK_HZ * 2);
	z_port_close(&out);
	out_on = false;
}

static void print(const char *s) {
	uint32_t n = (uint32_t)strlen(s);
	for (int i = 0; i < 64 && !out_text(s, n); i++) {
		z_msg_t m;
		while (z_msg_read(&m) == Z_OK)
			if (m.subject == Z_PORT_DATA_ACK) z_port_handle_ack(&out, &m);
		z_proc_wait(1);
	}
}

// Hand the command to zlink0 and print what comes back. Messages from
// the service are copied into a ring and acked while it is less than
// half full -- so the service, which sends at most 256 bytes a time and
// stops at 8 unacked, can never overrun it -- and the ring drains to
// the terminal as fast as that takes it.
static int client(const char *line) {
	static char ring[4096];
	static struct { uint32_t from, tag; } owed[Z_PORT_MAX_PENDING_SENDS];
	uint32_t rh = 0, rn = 0, owed_n = 0, last = z_uptime_ticks();
	z_port_t svc;
	uint32_t pid;
	int rc = 1;
	bool closed = false, marker = false;

	memset(&svc, 0, sizeof svc);
	if (!z_pid_lookup("zlink0", &pid)) {
		print("zlink: not running -- zlink start [PORT [X Y]]\n");
		return 1;
	}
	if (z_port_connect_arg(&svc, pid, z_obj_str(line)) != Z_OK) {
		// refused (too many commands at once) or no answer: the
		// console has zport's reason
		print("zlink: zlink0 refused the command or did not answer (the console says which)\n");
		return 1;
	}
	while (!closed || rn) {
		z_msg_t m;
		while (z_msg_read(&m) == Z_OK) {
			if (m.subject == Z_PORT_DATA_ACK) {
				if (m.from == out.peer_pid) z_port_handle_ack(&out, &m);
			} else if (m.subject == Z_PORT_DATA && m.from == pid) {
				uint32_t n = z_blob_len(&m.obj);
				const char *d = z_blob_data(&m.obj);
				for (uint32_t i = 0; d && i < n; i++) {
					// 0x01 and the exit status: the service's last word
					if (marker) { rc = d[i] - '0'; marker = false; }
					else if (d[i] == 1) marker = true;
					else if (rn < sizeof ring) ring[(rh + rn++) % sizeof ring] = d[i];
				}
				if (rn > sizeof ring / 2 && owed_n < Z_PORT_MAX_PENDING_SENDS) {
					owed[owed_n].from = m.from;
					owed[owed_n].tag = m.tag;
					owed_n++;
				} else {
					z_port_send_ack(&m);
				}
			} else if (m.subject == Z_PORT_CLOSE && m.from == pid) {
				closed = true;
			} else if (m.subject == Z_PORT_DATA) {
				z_port_reject_stranger(&m);
			}
		}
		while (rn) {
			char b[128];
			uint32_t k = 0;
			while (k < sizeof b && k < rn) {
				b[k] = ring[(rh + k) % sizeof ring];
				k++;
			}
			if (!out_text(b, k)) break;
			rh = (rh + k) % sizeof ring;
			rn -= k;
		}
		if (rn <= sizeof ring / 2) {
			for (uint32_t i = 0; i < owed_n; i++) ack_late(owed[i].from, owed[i].tag);
			owed_n = 0;
		}
		if (!closed && z_uptime_ticks() - last > Z_TICK_HZ) {
			last = z_uptime_ticks();
			if (z_port_peer_gone(&svc)) {
				print("zlink: the zlink service stopped\n");
				return 1;
			}
		}
		z_proc_wait(1);
	}
	return rc;
}

// `zlink start`: check what can be checked here, where the user sees
// the answer, then launch the service.
static int start(int argc, char **argv) {
	char line[160];
	char *o = line;
	int port = 0, x = 0, y = 1, mode = M_AUTO;
	uint32_t pid;
	const char *bad;

	for (int a = 0; a < argc; a++) {
		if (!strcmp(argv[a], "-f") || !strcmp(argv[a], "-w")) continue;
		if (!strcmp(argv[a], "-m") && a + 1 < argc) {
			a++;
			if (!strcmp(argv[a], "soft")) mode = M_SOFT;
			else if (!strcmp(argv[a], "stream")) mode = M_STREAM;
			else if (strcmp(argv[a], "auto")) { print("zlink: -m is auto, soft or stream\n"); return 2; }
		} else if (!strcmp(argv[a], "-r") && a + 1 < argc) {
			long r = num(argv[++a]);
			if (r < rates[RATES_N - 1] || r > rates[0]) {
				print("zlink: -r is a rate in kbit/s, 750-12000\n");
				return 2;
			}
		} else if (argv[a][0] != '-' && (argc - a == 1 || argc - a == 3)) {
			port = (int)num(argv[a]);
			if (argc - a == 3) {
				x = (int)num(argv[a + 1]);
				y = (int)num(argv[a + 2]);
			}
			break;
		} else {
			print("usage: zlink start [-f | -w] [-m auto|soft|stream] [-r KBIT] [PORT [X Y]]\n");
			return 2;
		}
	}
	if ((bad = pins_bad(port, x, y))) {
		print("zlink: ");
		print(bad);
		print("\n");
		return 2;
	}
	if (mode == M_STREAM && !stream_built()) {
		print("zlink: -m stream needs a stream engine, and this bitstream has none\n");
		return 2;
	}
	if (z_pid_lookup("zlink0", &pid)) {
		print("zlink: already running (zlink stop first)\n");
		return 1;
	}
	o += strlen(strcpy(o, "serve"));
	for (int a = 0; a < argc; a++) {
		size_t k = strlen(argv[a]);
		if ((size_t)(o - line) + k + 2 > sizeof line) {
			print("zlink: too many arguments\n");
			return 2;
		}
		*o++ = ' ';
		memcpy(o, argv[a], k + 1);
		o += k;
	}
	z_launch_arg_set(line);
	if (!z_proc_run("zlink")) {
		print("zlink: could not start the service\n");
		return 1;
	}
	for (int i = 0; i < 3 * 60; i++) {
		if (z_pid_lookup("zlink0", &pid)) {
			print("zlink: started; zlink status shows the link\n");
			return 0;
		}
		z_proc_wait(Z_TICK_HZ / 60);
	}
	print("zlink: the service did not start (see the console)\n");
	return 1;
}

// `zlink loop`: one board, a jumper from TX to RX (or -i, inside the
// engine), and a stream of bytes checked on the way back. The bit-error
// test docs/zlink.md promises for bringing up wires.
static int loop(int argc, char **argv) {
	long kbit = 12000;
	bool inside = false;
	int a = 0, e;
	char b[12];
	while (a < argc && argv[a][0] == '-') {
		if (!strcmp(argv[a], "-i")) inside = true;
		else if (!strcmp(argv[a], "-r") && a + 1 < argc) kbit = num(argv[++a]);
		else break;
		a++;
	}
	if (argc - a != 3 || kbit < 100 || kbit > 12000) {
		print("usage: zlink loop [-i] [-r KBIT] PORT TX RX   (100-12000 kbit/s)\n");
		return 2;
	}
	int port = (int)num(argv[a]), tx = (int)num(argv[a + 1]), rx = (int)num(argv[a + 2]);
	const char *bad = pins_bad(port, tx, rx);
	if (bad) {
		print("zlink: ");
		print(bad);
		print("\n");
		return 2;
	}
	if (!stream_built() || (e = z_gs_claim(Z_GS_ANY)) < 0) {
		print("zlink: no stream engine free\n");
		return 1;
	}
	z_gs_cfg_t c = Z_GS_CFG_INIT;
	c.mode = Z_GS_ZLINK;
	c.flags = Z_GS_RXEN | (inside ? Z_GS_LOOP : 0);
	c.tx = Z_GS_PIN(port, tx);
	c.rx = Z_GS_PIN(port, rx);
	c.div = z_gs_div_for(Z_GS_ZLINK, (uint32_t)kbit * 1000u, NULL);
	if (z_gs_config(e, &c) != Z_GS_OK) {
		z_gs_release(e);
		print("zlink: the engine would not take those pins\n");
		return 1;
	}
	// let the receiver find a comma first
	for (uint32_t t = z_uptime_ticks(); z_uptime_ticks() - t < 3;) z_proc_wait(1);
	z_gs_flush(e, false, true);
	z_gs_clear_errors(e);

	const uint32_t total = 65536;
	uint32_t txs = 1, rxs = 1, sent = 0, got = 0, bad_bytes = 0;
	uint32_t t0 = z_uptime_ticks(), idle = t0;
	while (got < total && z_uptime_ticks() - idle < Z_TICK_HZ) {
		uint8_t buf[64];
		uint32_t n = z_gs_tx_free(e);
		if (n > sizeof buf) n = sizeof buf;
		if (n > total - sent) n = total - sent;
		for (uint32_t i = 0; i < n; i++) {
			txs ^= txs << 13; txs ^= txs >> 17; txs ^= txs << 5;
			buf[i] = (uint8_t)txs;
		}
		sent += z_gs_write(e, buf, n);
		n = z_gs_read(e, buf, sizeof buf);
		if (!n && z_gs_read_sym(e) >= 0) n = 0;		// a stray control symbol
		for (uint32_t i = 0; i < n; i++) {
			rxs ^= rxs << 13; rxs ^= rxs >> 17; rxs ^= rxs << 5;
			if (buf[i] != (uint8_t)rxs) bad_bytes++;
		}
		got += n;
		if (n) idle = z_uptime_ticks();
	}
	uint32_t ms = (z_uptime_ticks() - t0) * 1000u / Z_TICK_HZ;
	uint32_t serr = z_gs_errors(e);
	z_gs_release(e);

	print("zlink: ");
	print(u2s(got, b));
	print(" of ");
	print(u2s(total, b));
	print(" bytes back, ");
	print(u2s(bad_bytes, b));
	print(" wrong, ");
	print(u2s(serr, b));
	print(" code errors, at ");
	print(u2s(kbit, b));
	print(" kbit/s in ");
	print(u2s(ms, b));
	print(" ms\n");
	if (got < total) print("zlink: bytes went missing -- check the jumper, or try a lower -r\n");
	return (got == total && !bad_bytes && !serr) ? 0 : 1;
}

int main(void) {
	static char line[256], buf[256];
	char *argv[16];
	uint8_t flags[16];
	int argc = 0, rc;
	uint32_t pid = 0;

	if (z_launch_arg_take(line, sizeof line) && line[0])
		argc = z_args_split(line, buf, sizeof buf, argv, flags, 16);
	if (argc > 0 && !strcmp(argv[0], "serve")) return serve(argc - 1, argv + 1);

	if (z_pid_lookup("posix0", &pid) && pid &&
	    z_port_connect_arg(&out, pid, z_obj_str(PX_STDOUT_TAG)) == Z_OK)
		out_on = true;
	if (argc > 0 && !strcmp(argv[0], "start")) rc = start(argc - 1, argv + 1);
	else if (argc > 0 && !strcmp(argv[0], "loop")) rc = loop(argc - 1, argv + 1);
	else rc = client(argc ? line : "status");
	out_close();
	return rc;
}
