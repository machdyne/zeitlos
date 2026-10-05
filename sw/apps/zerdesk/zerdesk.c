/*
 * Zeitlos -- zerdesk: the remote desktop, served by Zeitlos itself.
 *
 * An app of its own. net accepts the TCP connections (Z_NET_LISTEN,
 * sw/common/znet.h) and relays them here as zport connections; this
 * serves the viewer page and the WebSocket, and puts the pointer and
 * the keyboard back.
 *
 *   GET /      the viewer page, read from the card: PAGE_PATH. The
 *              card image puts the ESP32's index.html there (release/),
 *              and it can be edited in place: every request reads the
 *              file again. Without it there is no remote desktop, and
 *              the browser is told why in plain text.
 *   GET /ws    WebSocket. One stripe is [idx][len16 LE][PackBits +
 *              trailer], the bytes the ESP32 forwards today. 3-byte
 *              keys and 5-byte mouse packets come back.
 *
 * At most apps.zerdesk.viewers viewers at once (6 unless the file says
 * otherwise, and never more than 6: that is how many inbound relays
 * net has). Whoever asks for / or /ws past that, while a relay is
 * still free, gets a plain-text 503, "Too many viewers connected".
 * The page is not touched. A seventh connection does not reach this
 * app: net has no seventh relay, and the TCP connection is refused.
 *
 * apps.zerdesk.allow is subnet or any, the same choice netserve makes.
 * Anything else, and a missing key, is subnet.
 *
 * One path or the other. On a bitstream with the ESP32 link the ESP32
 * serves the desktop and net scans the screen; two readers of DIRTY
 * would clear each other's bits. This refuses to start there.
 *
 * Started by hand: `run zerdesk`, from term.
 *
 * MEMORY: this executable asks for the medium tier, 32 KB of stack
 * and heap (APP_TIER in the Makefile). Everything big is static. The
 * heap holds only zport's copies of what is in flight to net, and
 * that is capped here by BUDGET.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zport.h"
#include "../../common/znet.h"
#include "../../common/zcfg.h"
#include "../../common/zfsapp.h"
#include "../../common/zscreen.h"
#include "../../common/zinput.h"
#include "../../common/zws.h"
#include "deskcfg.h"

#define TICKS_PER_SEC  Z_TICK_HZ

#define MAX_CONN       6		/* net has 6 inbound relays */
#define REFUSE_LOG_TICKS (5 * TICKS_PER_SEC)
#define REFUSE_TEXT    "Too many viewers connected"
#define PAGE_PATH      "/zerdesk/index.html"
#define NO_PAGE_TEXT   "No remote desktop: the viewer page " PAGE_PATH \
	" is not on the card. The card image carries it; its source is " \
	"esp32/zeitlos-nic/web/index.html."
#define RX_SZ          1024
#define OUT_SZ         2560
#ifndef CHUNK
#define CHUNK          1024
#endif
#ifndef BUDGET
#define BUDGET         (6 * 1024)
#endif
#ifndef PER_CONN
#define PER_CONN       (4 * 1024)
#endif
#define HTTP_TICKS     (10 * TICKS_PER_SEC)
#define STUCK_TICKS    (10 * TICKS_PER_SEC)
#define BEAT_TICKS     (5 * TICKS_PER_SEC)
#define DRAIN_TICKS    (5 * TICKS_PER_SEC)
#define STATS_TICKS    (10 * TICKS_PER_SEC)

#define BUSY_STRIPES   10
#define SCAN_TICKS     (TICKS_PER_SEC / 20)
#define BUSY_SCAN_TICKS (TICKS_PER_SEC / 15)
#define VERIFY_TICKS   (5 * TICKS_PER_SEC)
/* one stripe on the wire: WS header 4 + [idx,len] 3 + data + trailer */
#define STRIPE_MSG_MAX (4 + 3 + ZSCREEN_LITERAL_LEN + ZSCREEN_TRAILER)
#define WS_IN_MAX      125		/* longer client frames are not input */

enum { C_FREE = 0, C_HTTP, C_BODY, C_WS, C_DRAIN };

typedef struct {
	uint8_t state;
	bool viewer;
	bool need_all;
	bool skip;			/* WS: ignore until the message ends */
	z_port_t port;
	z_net_accept_t info;
	z_ws_parser wsp;
	uint8_t rx[RX_SZ];
	uint16_t rx_len;
	uint8_t acc[8];			/* one mouse or key message */
	uint8_t acc_n;
	uint8_t out[OUT_SZ];
	uint16_t out_len;
	int page_fd;			/* -1, or the page being sent */
	uint32_t body_left;		/* bytes of it still to read */
	uint32_t todo;
	uint16_t lens[Z_PORT_MAX_PENDING_SENDS];
	uint8_t lens_head, lens_n;
	uint32_t opened, last_tx, last_progress, deadline;
	uint32_t stripes, bytes;
	uint32_t first_seq;
	uint32_t first_bytes, first_ticks;
	bool first_done;
} conn_t;

static conn_t conns[MAX_CONN];
static uint32_t net_pid;
static bool listening;
static uint32_t listen_port = ZD_PORT_DEFAULT;
static int max_viewers = ZD_VIEWERS_DEFAULT;
static int allow_any;
static uint32_t refuse_log_tick;
static uint32_t next_conn_id;

static zscreen_t scr;
static int full_next = ZSCREEN_FULL_RESET;
static uint32_t last_scan_tick, last_verify_tick;
static uint32_t scan_gap = SCAN_TICKS;
static uint16_t frame_seq;
static uint8_t enc[ZSCREEN_STRIPES][ZSCREEN_LITERAL_LEN];
static uint16_t enc_len[ZSCREEN_STRIPES];
static uint32_t enc_valid;
static uint8_t pack[ZSCREEN_PACK_WORST];

static struct {
	uint32_t scans, frames, stripes_enc, stripes_sent, msgs, bytes;
	uint32_t zport_full, send_fail, budget_wait, keys, mice, refused;
	uint32_t loops, cyc_snap, cyc_enc, cyc_send;
} st;

static inline uint32_t rdcycle(void)
{
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

static uint32_t pend_bytes, pend_bytes_hw, pend_n_hw, heap_hw;
static uint32_t stats_tick;

extern char _end;
extern void *_sbrk(int incr);
static uintptr_t sp0;
#define PAINT 0xA5C3E1F0u

static void stack_paint(void)
{
	register uintptr_t sp asm("sp");
	uintptr_t lo = ((uintptr_t)_sbrk(0) + 1024) & ~3u;
	uintptr_t hi = (sp - 512) & ~3u;
	for (uintptr_t a = lo; a < hi; a += 4)
		*(volatile uint32_t *)a = PAINT;
}

static uint32_t stack_used(void)
{
	uintptr_t a = ((uintptr_t)_sbrk(0) + 1024) & ~3u;
	while (a < sp0 && *(volatile uint32_t *)a == PAINT)
		a += 4;
	return (uint32_t)(sp0 - a);
}

static uint32_t inuse_hw;

static void heap_note(void)
{
	uint32_t h = (uint32_t)((uintptr_t)_sbrk(0) - (uintptr_t)&_end);
	if (h > heap_hw)
		heap_hw = h;
	struct mallinfo mi = mallinfo();
	if ((uint32_t)mi.uordblks > inuse_hw)
		inuse_hw = (uint32_t)mi.uordblks;
}

static int viewers(void)
{
	int n = 0;
	for (int i = 0; i < MAX_CONN; i++)
		if (conns[i].state == C_WS && conns[i].viewer)
			n++;
	return n;
}

static conn_t *by_id(uint32_t id)
{
	for (int i = 0; i < MAX_CONN; i++)
		if (conns[i].state && conns[i].port.conn_id == id)
			return &conns[i];
	return NULL;
}

static void print_ip(uint32_t ip)
{
	printf("%lu.%lu.%lu.%lu", (unsigned long)(ip >> 24),
		(unsigned long)((ip >> 16) & 0xff),
		(unsigned long)((ip >> 8) & 0xff), (unsigned long)(ip & 0xff));
}

static bool out_put(conn_t *c, const void *p, uint32_t n)
{
	if (n > (uint32_t)(OUT_SZ - c->out_len))
		return false;
	memcpy(c->out + c->out_len, p, n);
	c->out_len = (uint16_t)(c->out_len + n);
	return true;
}

static void out_str(conn_t *c, const char *s)
{
	out_put(c, s, (uint32_t)strlen(s));
}

static void verify_report(void)
{
	if (scr.dirty_hw == 1)
		printf("screen: %lu full check(s), %lu stripe(s) changed "
			"with no DIRTY bit\n",
			(unsigned long)scr.verify_passes,
			(unsigned long)scr.verify_missed);
}

static void page_close(conn_t *c)
{
	if (c->page_fd >= 0)
		fs_close_handle(c->page_fd);
	c->page_fd = -1;
	c->body_left = 0;
}

static void conn_free(conn_t *c)
{
	page_close(c);
	for (int k = 0; k < c->lens_n; k++)
		pend_bytes -= c->lens[(c->lens_head + k) % Z_PORT_MAX_PENDING_SENDS];
	memset(c, 0, sizeof(*c));
}

static void conn_end(conn_t *c, const char *why)
{
	if (c->state == C_FREE || c->state == C_DRAIN)
		return;
	page_close(c);
	if (c->viewer) {
		c->viewer = false;
		c->state = C_DRAIN;
		printf("zerdesk: viewer %d gone (%s), n=%d; %lu stripes, %lu bytes\n",
			(int)(c - conns) + 1, why, viewers(),
			(unsigned long)c->stripes, (unsigned long)c->bytes);
		if (!viewers()) {
			verify_report();
			if (zinput_held())
				zinput_release();
		}
	}
	if (c->port.connected)
		z_port_close(&c->port);
	c->state = C_DRAIN;
	c->deadline = z_uptime_ticks() + DRAIN_TICKS;
}

static void acked(conn_t *c, uint32_t before)
{
	uint32_t after = c->port.pending_count;
	while (before > after && c->lens_n) {
		pend_bytes -= c->lens[c->lens_head];
		c->lens_head = (uint8_t)((c->lens_head + 1) % Z_PORT_MAX_PENDING_SENDS);
		c->lens_n--;
		before--;
	}
}

static int popcount30(uint32_t m)
{
	int n = 0;
	for (; m; m &= m - 1)
		n++;
	return n;
}

static void encode(int idx)
{
	const uint8_t *raw;
	int clen;

	if (enc_valid & (1u << idx))
		return;
	raw = (const uint8_t *)(scr.snap + idx * ZSCREEN_STRIPE_WORDS);
	clen = zscreen_pack(raw, pack);
	memcpy(enc[idx], pack, (size_t)clen);
	enc_len[idx] = (uint16_t)clen;
	enc_valid |= 1u << idx;
	st.stripes_enc++;
}

static bool frame_drained(void)
{
	for (int i = 0; i < MAX_CONN; i++)
		if (conns[i].state == C_WS && conns[i].viewer && conns[i].todo)
			return false;
	return true;
}

static bool want_all(void)
{
	for (int i = 0; i < MAX_CONN; i++)
		if (conns[i].state == C_WS && conns[i].viewer && conns[i].need_all)
			return true;
	return false;
}

/* One frame is one instant: do not copy again until every viewer has
 * been handed the stripes of the current one. DIRTY bits accumulate
 * across that wait. A new viewer is marked need_all and gets the
 * whole frame from the copy. */
static void scan(uint32_t now)
{
	int full, sends, saw, buttons;
	uint32_t missed = 0, changed, t0;
	bool all;

	if (!viewers() || !frame_drained())
		return;
	all = want_all();
	if (!all && (now - last_scan_tick) < scan_gap)
		return;
	last_scan_tick = now;
	st.scans++;

	full = full_next;
	full_next = ZSCREEN_FULL_NONE;
	t0 = rdcycle();
	changed = zscreen_scan(&scr, full, &missed);
	st.cyc_snap += rdcycle() - t0;
	if (missed)
		printf("screen: stripes %08lx changed with no DIRTY bit\n",
			(unsigned long)missed);

	if (changed || all) {
		frame_seq++;
		enc_valid &= ~changed;
		if (changed)
			st.frames++;
		for (int i = 0; i < MAX_CONN; i++) {
			conn_t *c = &conns[i];
			if (c->state != C_WS || !c->viewer)
				continue;
			if (c->need_all) {
				c->need_all = false;
				c->todo = ZSCREEN_ALL;
				c->first_seq = frame_seq;
			} else {
				c->todo |= changed;
			}
		}
	}

	sends = popcount30(changed);
	saw = zinput_take_motion();
	buttons = zinput_buttons();
	if (saw && buttons)
		scan_gap = 1;
	else
		scan_gap = sends > BUSY_STRIPES ? BUSY_SCAN_TICKS : SCAN_TICKS;

	if (now - last_verify_tick >= VERIFY_TICKS && !buttons &&
			sends <= BUSY_STRIPES) {
		last_verify_tick = now;
		full_next = ZSCREEN_FULL_VERIFY;
	}
}

static void frame_stripes(conn_t *c)
{
	while (c->todo && OUT_SZ - c->out_len >= STRIPE_MSG_MAX) {
		int idx = __builtin_ctz(c->todo);
		uint32_t dlen, plen, t0;
		uint8_t *o;
		size_t h;

		c->todo &= ~(1u << idx);
		t0 = rdcycle();
		encode(idx);
		st.cyc_enc += rdcycle() - t0;
		dlen = enc_len[idx] + (uint32_t)ZSCREEN_TRAILER;
		plen = 3u + dlen;
		o = c->out + c->out_len;
		h = z_ws_header(o, Z_WS_BINARY, plen);
		o[h++] = (uint8_t)idx;
		o[h++] = (uint8_t)(dlen & 0xff);
		o[h++] = (uint8_t)(dlen >> 8);
		memcpy(o + h, enc[idx], enc_len[idx]);
		h += enc_len[idx];
		zscreen_trailer(o + h, frame_seq, c->todo ? 0 : 1);
		h += ZSCREEN_TRAILER;
		c->out_len = (uint16_t)(c->out_len + h);
		c->stripes++;
		st.stripes_sent++;
		if (!c->first_done && frame_seq == c->first_seq) {
			c->first_bytes += (uint32_t)h;
			if (!c->todo) {
				c->first_done = true;
				c->first_ticks = z_uptime_ticks() - c->opened;
				printf("zerdesk: viewer %d first frame %lu bytes, framed in %lu ms\n",
					(int)(c - conns) + 1, (unsigned long)c->first_bytes,
					(unsigned long)(c->first_ticks * 1000u / TICKS_PER_SEC));
			}
		}
	}
}

static uint32_t conn_pend(const conn_t *c)
{
	uint32_t b = 0;
	for (int k = 0; k < c->lens_n; k++)
		b += c->lens[(c->lens_head + k) % Z_PORT_MAX_PENDING_SENDS];
	return b;
}

static uint32_t share(void)
{
	uint32_t n = 0, s;
	for (int i = 0; i < MAX_CONN; i++) {
		const conn_t *c = &conns[i];
		if ((c->state == C_WS || c->state == C_BODY) &&
				(c->out_len || c->body_left || c->todo || c->lens_n))
			n++;
	}
	s = n ? BUDGET / n : BUDGET;
	if (s > PER_CONN)
		s = PER_CONN;
	if (s < CHUNK)
		s = CHUNK;
	return s;
}

/* The next piece of the page, into out. A file that comes up short
 * (cut while it was being sent) ends the connection there. */
static bool page_fill(conn_t *c)
{
	uint32_t want = c->body_left < CHUNK ? c->body_left : CHUNK;
	int got = fs_read_chunk(c->page_fd, c->out, (int)want);

	if (got <= 0) {
		printf("zerdesk: %s: read failed with %lu bytes still to send\n",
			PAGE_PATH, (unsigned long)c->body_left);
		conn_end(c, "page read failed");
		return false;
	}
	c->out_len = (uint16_t)got;
	c->body_left -= (uint32_t)got;
	return true;
}

static void pump(conn_t *c, uint32_t now)
{
	if (c->state == C_WS && c->viewer)
		frame_stripes(c);

	for (;;) {
		const uint8_t *p;
		uint32_t n;
		uint32_t t0;
		z_rv rv;

		if (!c->out_len && c->body_left && c->port.connected &&
				!page_fill(c))
			break;
		if (!c->out_len)
			break;
		p = c->out;
		n = c->out_len;
		if (n > CHUNK)
			n = CHUNK;
		if (!c->port.connected)
			break;
		if (c->port.pending_count >= Z_PORT_MAX_PENDING_SENDS) {
			st.zport_full++;
			break;
		}
		if (pend_bytes + n > BUDGET || conn_pend(c) + n > share()) {
			st.budget_wait++;
			break;
		}
		t0 = rdcycle();
		rv = z_port_send(&c->port, p, n);
		st.cyc_send += rdcycle() - t0;
		if (rv != Z_OK) {
			st.send_fail++;
			break;
		}
		heap_note();
		c->lens[(c->lens_head + c->lens_n) % Z_PORT_MAX_PENDING_SENDS] = (uint16_t)n;
		c->lens_n++;
		pend_bytes += n;
		if (pend_bytes > pend_bytes_hw)
			pend_bytes_hw = pend_bytes;
		{
			uint32_t pn = 0;
			for (int i = 0; i < MAX_CONN; i++)
				pn += conns[i].port.pending_count;
			if (pn > pend_n_hw)
				pend_n_hw = pn;
		}
		st.msgs++;
		st.bytes += n;
		c->bytes += n;
		c->last_tx = now;
		c->last_progress = now;
		memmove(c->out, c->out + n, c->out_len - n);
		c->out_len = (uint16_t)(c->out_len - n);
		if (c->state == C_WS && c->viewer)
			frame_stripes(c);
	}

	if (c->state == C_BODY && !c->out_len && !c->body_left) {
		if (c->page_fd >= 0) {
			printf("zerdesk: page to ");
			print_ip(c->info.ip);
			printf(", %lu ms\n", (unsigned long)((now - c->opened) *
				1000u / TICKS_PER_SEC));
		}
		conn_end(c, "served");
	}
}

static int ci_prefix(const char *s, const char *p)
{
	for (; *p; s++, p++) {
		char a = *s, b = *p;
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
		if (a != b)
			return 0;
	}
	return 1;
}

static const char *header_val(const char *req, const char *name, int *len)
{
	const char *l = strstr(req, "\r\n");
	while (l && l[2] && !(l[2] == '\r' && l[3] == '\n')) {
		l += 2;
		if (ci_prefix(l, name)) {
			const char *v = l + strlen(name);
			const char *e;
			while (*v == ' ' || *v == '\t')
				v++;
			e = strstr(v, "\r\n");
			if (!e)
				return NULL;
			while (e > v && (e[-1] == ' ' || e[-1] == '\t'))
				e--;
			*len = (int)(e - v);
			return v;
		}
		l = strstr(l, "\r\n");
	}
	return NULL;
}

static void http_text(conn_t *c, const char *status, const char *text)
{
	char h[320];
	snprintf(h, sizeof h, "HTTP/1.0 %s\r\nContent-Type: text/plain\r\n"
		"Content-Length: %u\r\nCache-Control: no-store\r\n"
		"Connection: close\r\n\r\n%s\n",
		status, (unsigned)strlen(text) + 1, text);
	out_str(c, h);
	c->state = C_BODY;
}

static void http_simple(conn_t *c, const char *status)
{
	http_text(c, status, status);
}

static bool viewers_full(conn_t *c)
{
	if (!zd_viewers_full(viewers(), max_viewers))
		return false;
	uint32_t now = z_uptime_ticks();
	st.refused++;
	if (!refuse_log_tick || (int32_t)(now - refuse_log_tick) >= (int32_t)REFUSE_LOG_TICKS) {
		refuse_log_tick = now;
		printf("zerdesk: refused ");
		print_ip(c->info.ip);
		printf(": %d viewers already (limit %d)\n", viewers(), max_viewers);
	}
	http_text(c, "503 Service Unavailable", REFUSE_TEXT);
	return true;
}

static void http_request(conn_t *c)
{
	char *req = (char *)c->rx;
	char path[32];
	int i = 0;
	const char *p;

	if (strncmp(req, "GET ", 4)) {
		http_simple(c, "405 Method Not Allowed");
		return;
	}
	p = req + 4;
	while (*p && *p != ' ' && i < (int)sizeof(path) - 1)
		path[i++] = *p++;
	path[i] = 0;

	if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		char h[200];
		int size;
		if (viewers_full(c))
			return;
		/* Read from the card on every request, so an edited page
		 * is served at once. No copy is kept: without the file,
		 * the browser is told why. */
		size = fs_size(PAGE_PATH);
		if (size > 0)
			c->page_fd = fs_open_read(PAGE_PATH);
		if (size <= 0 || c->page_fd < 0) {
			c->page_fd = -1;
			printf("zerdesk: page to ");
			print_ip(c->info.ip);
			printf(": %s %s; sent the reason instead\n", PAGE_PATH,
				size <= 0 ? "is not on the card" : "would not open");
			http_text(c, "503 Service Unavailable", size <= 0 ?
				NO_PAGE_TEXT : "The viewer page " PAGE_PATH
				" is on the card but would not open.");
			return;
		}
		snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
			"Content-Length: %u\r\nCache-Control: no-store\r\n"
			"Connection: close\r\n\r\n", (unsigned)size);
		out_str(c, h);
		c->body_left = (uint32_t)size;
		c->state = C_BODY;
		return;
	}
	if (!strcmp(path, "/ws")) {
		int kl = 0;
		const char *key;
		char reply[Z_WS_RESPONSE_MAX];
		size_t rn;
		if (viewers_full(c))
			return;
		key = header_val(req, "sec-websocket-key:", &kl);
		rn = key ? z_ws_response(reply, sizeof reply, key, (size_t)kl) : 0;
		if (!rn) {
			http_simple(c, "400 Bad Request");
			return;
		}
		out_str(c, reply);
		z_ws_parser_init(&c->wsp, WS_IN_MAX);
		c->acc_n = 0;
		c->skip = false;
		c->state = C_WS;
		c->viewer = true;
		c->need_all = true;
		c->opened = z_uptime_ticks();
		c->last_progress = c->opened;
		printf("zerdesk: viewer %d connected from ", (int)(c - conns) + 1);
		print_ip(c->info.ip);
		printf(" (n=%d)\n", viewers());
		if (viewers() == 1) {
			full_next = ZSCREEN_FULL_RESET;
			zscreen_forget(&scr);
			enc_valid = 0;
		}
		return;
	}
	http_simple(c, "404 Not Found");
}

static void ws_input(conn_t *c, const uint8_t *p, uint32_t n)
{
	(void)c;
	if (n >= 5) {
		st.mice++;
		zinput_mouse(p, n);
	} else if (n >= 3) {
		if (zinput_key(p, n))
			st.keys++;
	}
}

static void ws_rx(conn_t *c)
{
	uint8_t *p = c->rx;
	uint32_t n = c->rx_len;

	while (n) {
		z_ws_event ev;
		size_t used = z_ws_feed(&c->wsp, p, n, &ev);
		uint8_t frame[Z_WS_CONTROL_MAX];
		size_t fn;

		p += used;
		n -= (uint32_t)used;
		if (ev.type == Z_WS_EV_NONE)
			break;
		switch (ev.type) {
		case Z_WS_EV_DATA:
			if (c->skip) {
				if (ev.fin)
					c->skip = false;
				break;
			}
			if (ev.opcode != Z_WS_BINARY ||
					c->acc_n + ev.len > sizeof c->acc) {
				c->acc_n = 0;
				if (!ev.fin)
					c->skip = true;
				break;
			}
			if (ev.len) {
				memcpy(c->acc + c->acc_n, ev.data, ev.len);
				c->acc_n = (uint8_t)(c->acc_n + ev.len);
			}
			if (ev.fin) {
				ws_input(c, c->acc, c->acc_n);
				c->acc_n = 0;
			}
			break;
		case Z_WS_EV_PING:
			fn = z_ws_control(frame, Z_WS_PONG, ev.data, ev.len);
			out_put(c, frame, (uint32_t)fn);
			break;
		case Z_WS_EV_CLOSE:
			fn = z_ws_control(frame, Z_WS_CLOSE, ev.data,
				ev.len >= 2 ? 2 : 0);
			out_put(c, frame, (uint32_t)fn);
			if (c->viewer) {
				c->viewer = false;
				c->todo = 0;
				printf("zerdesk: viewer %d closed (n=%d)\n",
					(int)(c - conns) + 1, viewers());
				if (!viewers()) {
					verify_report();
					if (zinput_held())
						zinput_release();
				}
			}
			c->state = C_BODY;
			n = 0;
			break;
		case Z_WS_EV_ERROR:
			fn = z_ws_close(frame, ev.code ? ev.code : Z_WS_CLOSE_PROTOCOL);
			out_put(c, frame, (uint32_t)fn);
			if (c->viewer) {
				c->viewer = false;
				printf("zerdesk: viewer %d sent a frame we do not take; closing (n=%d)\n",
					(int)(c - conns) + 1, viewers());
			}
			c->state = C_BODY;
			n = 0;
			break;
		default:
			break;
		}
		if (c->state != C_WS)
			break;
	}
	if (n && p != c->rx)
		memmove(c->rx, p, n);
	c->rx_len = (uint16_t)n;
}

static void on_data(conn_t *c, const z_msg_t *m)
{
	uint32_t len = z_blob_len(&m->obj);
	const uint8_t *d = (const uint8_t *)z_blob_data(&m->obj);

	if (c->state == C_HTTP || c->state == C_WS) {
		uint32_t room = RX_SZ - 1 - c->rx_len;
		if (len > room) {
			printf("zerdesk: conn %d: %lu bytes past rx dropped\n",
				(int)(c - conns) + 1, (unsigned long)(len - room));
			len = room;
		}
		if (len)
			memcpy(c->rx + c->rx_len, d, len);
		c->rx_len = (uint16_t)(c->rx_len + len);
		c->rx[c->rx_len] = 0;
	}
	z_port_send_ack(m);

	if (c->state == C_HTTP) {
		char *e = strstr((char *)c->rx, "\r\n\r\n");
		if (e) {
			uint16_t used;
			e[4] = 0;
			used = (uint16_t)(e + 4 - (char *)c->rx);
			http_request(c);
			memmove(c->rx, c->rx + used, c->rx_len - used);
			c->rx_len = (uint16_t)(c->rx_len - used);
		} else if (c->rx_len >= RX_SZ - 1) {
			http_simple(c, "431 Request Header Fields Too Large");
		}
	}
	if (c->state == C_WS)
		ws_rx(c);
}

static void refuse_connect(const z_msg_t *m)
{
	z_msg_new_send(m->from, Z_PORT_REFUSED, m->tag, z_obj_none());
}

static void on_connect(const z_msg_t *m)
{
	z_net_accept_t info;
	conn_t *c = NULL;

	if (m->obj.type != Z_BLOB || z_blob_len(&m->obj) < sizeof(info)) {
		refuse_connect(m);
		return;
	}
	memcpy(&info, z_blob_data(&m->obj), sizeof(info));
	if (!zd_take_peer(allow_any, (info.flags & Z_NET_ACCEPT_LOCAL) != 0)) {
		refuse_connect(m);
		printf("zerdesk: refused ");
		print_ip(info.ip);
		printf(" -- not on this subnet (apps.zerdesk.allow)\n");
		return;
	}
	for (int i = 0; i < MAX_CONN; i++)
		if (!conns[i].state) { c = &conns[i]; break; }
	if (!c) {
		refuse_connect(m);
		printf("zerdesk: refused a connection: all %d slots busy\n", MAX_CONN);
		return;
	}
	memset(c, 0, sizeof(*c));
	c->page_fd = -1;
	c->info = info;
	c->state = C_HTTP;
	c->port.peer_pid = m->from;
	next_conn_id = zd_next_conn(next_conn_id);
	c->port.conn_id = next_conn_id;
	c->port.connected = true;
	c->opened = z_uptime_ticks();
	c->deadline = c->opened + HTTP_TICKS;
	z_msg_new_send(m->from, Z_PORT_CONNECTED, m->tag, z_obj_uint32(c->port.conn_id));
}

static void on_msg(const z_msg_t *m)
{
	conn_t *c;

	if (m->from != net_pid || !net_pid) {
		if (m->obj.type == Z_BLOB && m->subject == Z_PORT_DATA)
			z_port_send_ack(m);
		return;
	}
	switch (m->subject) {
	case Z_NET_LISTEN_REPLY:
		if (m->obj.val.uint32) {
			printf("zerdesk: net refused port %lu (error %lu)\n",
				(unsigned long)listen_port, (unsigned long)m->obj.val.uint32);
		} else {
			listening = true;
			printf("zerdesk: serving the remote desktop on port %lu "
				"(http://<ip>:%lu/)\n", (unsigned long)listen_port,
				(unsigned long)listen_port);
		}
		return;
	case Z_PORT_CONNECT:
		on_connect(m);
		return;
	case Z_PORT_DATA:
		c = by_id(m->tag);
		if (!c || m->obj.type != Z_BLOB || c->state == C_DRAIN) {
			if (m->obj.type == Z_BLOB)
				z_port_send_ack(m);
			return;
		}
		on_data(c, m);
		return;
	case Z_PORT_DATA_ACK:
		c = by_id(m->tag);
		if (c) {
			uint32_t before = c->port.pending_count;
			z_port_handle_ack_closed(&c->port, m);
			acked(c, before);
		}
		return;
	case Z_NET_EOF:
		c = by_id(m->tag);
		if (c && c->state == C_WS)
			conn_end(c, "peer finished");
		return;
	case Z_PORT_CLOSE:
		c = by_id(m->tag);
		if (c && c->state != C_DRAIN) {
			c->port.connected = false;
			conn_end(c, "peer closed");
		}
		return;
	}
}

static void stats(uint32_t now)
{
	uint32_t dt = now - stats_tick;
	int n;
	uint32_t ms;
	struct mallinfo mi;

	if (dt < STATS_TICKS)
		return;
	stats_tick = now;
	n = viewers();
	if (!n && !st.msgs)
		return;
	ms = dt * 1000u / TICKS_PER_SEC;
	mi = mallinfo();
	printf("zerdesk: %lu ms, viewers %d: scans %lu, frames %lu, stripes enc %lu sent %lu, "
		"msgs %lu, bytes %lu (%lu B/s); zport-full %lu, budget-wait %lu, send-fail %lu; "
		"in key %lu mouse %lu, refused %lu; pend hw %lu msgs %lu B; heap hw %lu (arena %lu, in use %lu, max in use %lu), "
		"stack %lu of %lu; loops %lu, Mcyc snap %lu enc %lu send %lu\n",
		(unsigned long)ms, n, (unsigned long)st.scans, (unsigned long)st.frames,
		(unsigned long)st.stripes_enc, (unsigned long)st.stripes_sent,
		(unsigned long)st.msgs, (unsigned long)st.bytes,
		(unsigned long)(ms ? (uint64_t)st.bytes * 1000u / ms : 0),
		(unsigned long)st.zport_full, (unsigned long)st.budget_wait,
		(unsigned long)st.send_fail, (unsigned long)st.keys, (unsigned long)st.mice,
		(unsigned long)st.refused, (unsigned long)pend_n_hw, (unsigned long)pend_bytes_hw,
		(unsigned long)heap_hw, (unsigned long)mi.arena, (unsigned long)mi.uordblks,
		(unsigned long)inuse_hw, (unsigned long)stack_used(),
		(unsigned long)(sp0 - (uintptr_t)&_end), (unsigned long)st.loops,
		(unsigned long)(st.cyc_snap >> 20), (unsigned long)(st.cyc_enc >> 20),
		(unsigned long)(st.cyc_send >> 20));
	memset(&st, 0, sizeof(st));
}

static const char *from_file(bool file)
{
	return file ? "from the file" : "default";
}

int main(void)
{
	char name[24];
	char val[32];
	bool port_file, allow_file, view_file;
	register uintptr_t sp asm("sp");

	sp0 = (sp + 4095) & ~4095u;

	if (z_soc_has_feature(Z_FEATURE_ESP32_LINK)) {
		printf("zerdesk: this bitstream has the ESP32 link -- the ESP32 serves "
			"the remote desktop there, and net scans the screen. Not starting.\n");
		return 1;
	}
	if (!z_pid_register("zerdesk", name, sizeof(name))) {
		printf("zerdesk: could not register -- already running?\n");
		return 1;
	}

	port_file = z_cfg_get("apps.zerdesk.port", val, sizeof val);
	listen_port = (uint32_t)zd_clamp_port(z_cfg_get_int("apps.zerdesk.port", ZD_PORT_DEFAULT));
	view_file = z_cfg_get("apps.zerdesk.viewers", val, sizeof val);
	max_viewers = zd_clamp_viewers(z_cfg_get_int("apps.zerdesk.viewers", ZD_VIEWERS_DEFAULT));
	allow_file = z_cfg_get("apps.zerdesk.allow", val, sizeof val);
	allow_any = zd_allow_any(val);

	memset(conns, 0, sizeof(conns));
	heap_note();
	stack_paint();
	printf("zerdesk: starting as %s; chunk %u, budget %u (%u per conn), "
		"%lu B of stack+heap\n", name,
		(unsigned)CHUNK, (unsigned)BUDGET, (unsigned)PER_CONN,
		(unsigned long)(sp0 - (uintptr_t)&_end));
	{
		int size = fs_size(PAGE_PATH);
		if (size > 0)
			printf("zerdesk: page %s, %d B\n", PAGE_PATH, size);
		else
			printf("zerdesk: NO PAGE: %s is not on the card. Browsers "
				"get a plain-text error until it is there; the card "
				"image carries it.\n", PAGE_PATH);
	}
	printf("zerdesk: port %lu (%s), allow %s (%s), viewers %d (%s)\n",
		(unsigned long)listen_port, from_file(port_file),
		allow_any ? "any" : "subnet", from_file(allow_file),
		max_viewers, from_file(view_file));
	heap_note();
	printf("zerdesk: heap after start %lu B (arena %lu, in use %lu)\n",
		(unsigned long)heap_hw, (unsigned long)mallinfo().arena,
		(unsigned long)mallinfo().uordblks);

	{
		uint32_t next_listen = 0;
		stats_tick = z_uptime_ticks();
		for (;;) {
			z_msg_t m;
			uint32_t now = z_uptime_ticks();
			bool backlog = false;

			if (!listening && (int32_t)(now - next_listen) >= 0) {
				next_listen = now + 2u * TICKS_PER_SEC;
				if (net_pid || z_pid_lookup("net0", &net_pid))
					z_msg_new_send(net_pid, Z_NET_LISTEN, listen_port,
						z_obj_uint32(listen_port));
			}

			st.loops++;
			/* Copy the screen before taking input. Input wakes the
			 * wm, and a copy taken while it repaints a drag band
			 * catches the band in two places. */
			scan(z_uptime_ticks());
			while (z_msg_read(&m) == Z_OK)
				on_msg(&m);

			now = z_uptime_ticks();
			zinput_tick();

			for (int i = 0; i < MAX_CONN; i++) {
				conn_t *c = &conns[i];
				switch (c->state) {
				case C_FREE:
					continue;
				case C_DRAIN:
					if (!c->port.pending_count)
						conn_free(c);
					else if ((int32_t)(now - c->deadline) >= 0) {
						z_port_forget(&c->port);
						conn_free(c);
					}
					continue;
				case C_HTTP:
					if ((int32_t)(now - c->deadline) >= 0)
						conn_end(c, "request timeout");
					continue;
				default:
					break;
				}
				pump(c, now);
				if (c->state == C_WS && c->viewer) {
					if (!c->out_len && !c->todo && now - c->last_tx >= BEAT_TICKS) {
						uint8_t beat[Z_WS_HEADER_MAX + 1];
						size_t hn = z_ws_header(beat, Z_WS_BINARY, 1);
						beat[hn] = 0x00;
						out_put(c, beat, (uint32_t)hn + 1);
						pump(c, now);
					}
					if (c->todo && now - c->last_progress >= STUCK_TICKS)
						conn_end(c, "stuck 10 s");
				}
				if (c->state != C_FREE && c->state != C_DRAIN &&
						(c->out_len || c->body_left || c->todo))
					backlog = true;
			}

			stats(now);

			{
				uint32_t w = TICKS_PER_SEC / 5;
				if (viewers()) {
					uint32_t gone = now - last_scan_tick;
					uint32_t s = gone >= scan_gap ? 1 : scan_gap - gone;
					if (s < w)
						w = s;
				}
				if (zinput_held() && w > TICKS_PER_SEC / 10)
					w = TICKS_PER_SEC / 10;
				if (backlog && w > 20)
					w = 20;
				z_proc_wait(w);
			}
		}
	}
}
