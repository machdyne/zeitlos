/*
 * netserve -- network servers: SSH, telnet, HTTP (static files), and echo.
 * docs/netserve.md.
 *
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net accepts the TCP connections (Z_NET_LISTEN, sw/common/znet.h) and
 * relays each one here as a port connection, with netserve as the
 * provider. For a telnet session netserve asks for the password, then
 * connects to a port -- repl0, posix0, console0, serial0 -- as its
 * client and relays between the two, speaking telnet to one side and
 * plain bytes to the other. It is the same thing `term` does, with a
 * TCP connection where term has a window.
 *
 *   peer --TCP-- net --port-- netserve --port-- repl0
 *
 * One process, many sessions, and a message loop that never blocks
 * on one of them. That is why every connect here is asynchronous:
 * zport's z_port_connect() waits for the answer and DISCARDS every
 * other message meanwhile, which would lose another session's data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zeitlos.h"
#include "../../../common/zobj.h"
#include "../../../common/zport.h"
#include "../../../common/znet.h"
#include "../../../common/zcfg.h"
#include "../../../common/zauth.h"
#include "../../../common/zsoc.h"
#include "../../../common/zterm.h"	// Z_TERM_SET_PORT: posix handing the terminal to a child
#include "../../../common/zfsapp.h"	// HTTP: the files it serves
#include "../../../common/zkv.h"		// SSH: the host key, in the flash key/value store
#include "../../../common/zrng.h"		// SSH: ephemeral keys, padding
#include "../ssh/ssh_server.h"		// SSH: the protocol engine (docs/netserve.md, "SSH")

#define MAX_SESS        6           // net relays at most TCP_MAX_CONN - 2
#define TO_NET_MAX      512
#define TO_APP_MAX      256
#define HOLD_MAX        Z_PORT_MAX_PENDING_SENDS
#define LOGIN_TICKS     (60u * Z_TICK_HZ)
#define LAUNCH_TICKS    (15u * Z_TICK_HZ)
#define LOGIN_TRIES     3
#define AWAY_TICKS      (10u * Z_TICK_HZ)
#define HTTP_TICKS      (10u * Z_TICK_HZ)   // to send a whole request
#define HTTP_REQ_MAX    1024                // request line and headers
#define HTTP_PATH_MAX   160

// telnet (RFC 854, 857, 858)
#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define SE    240
#define OPT_ECHO 1
#define OPT_SGA  3

enum { K_TELNET = 1, K_ECHO, K_HTTP, K_SSH };

// SSH sessions: an engine each (about 6 KB), from a small pool.
#define SSH_MAX 2
#define SSH_OUT_MAX 2048        // engine output waiting for room in to_net
typedef struct {
	bool used;
	ssh_server_t eng;
	uint8_t out[SSH_OUT_MAX];   // everything the engine writes, in order
	uint16_t out_len;
	uint8_t in[SSHS_LOCAL_WINDOW];  // the client's channel data, for the port
	uint16_t in_len;
	uint16_t app_sent;          // bytes of to_app given to the port, to credit the window
} ssh_slot_t;
static ssh_slot_t ssh_pool[SSH_MAX];
static uint8_t host_seed[32];
#define HOSTKEY_KEY "apps.netserve.hostkey"

enum { H_REQ = 0, H_SEND };
enum { S_FREE = 0, S_LOGIN, S_LAUNCH, S_CONNECT, S_OPEN, S_AWAY, S_CLOSING, S_DRAIN };
enum { TN_DATA = 0, TN_IAC, TN_OPT, TN_SB, TN_SB_IAC };

// A DATA message not yet fully consumed, and not yet acked: its payload
// stays valid in the sender's memory until the ack (zport.h). `off` is
// how much of it has been used. One message can be far larger than any
// buffer here -- posix sends up to 4 KB at once -- so a message is
// consumed in pieces as room appears, never all-or-nothing: waiting for
// a 4 KB message to fit a 512-byte buffer is waiting forever, and that
// hung `help` and `vi` in a telnet session on hardware.
typedef struct { const uint8_t *data; uint32_t len, off, tag; } held_t;

typedef struct {
	uint8_t state, kind;
	uint32_t relay;             // net's relay id, the tag of its CONNECT
	z_port_t net;               // to net; conn_id is ours: index + 1
	z_port_t app;               // to the backend port
	uint32_t app_pid;
	char target[32];            // the port name `app` is (or is going) to

	// The terminal handoff, as term does it (docs/netserve.md): a
	// full-screen child of posix -- vi -- takes the session over, and
	// posix calls it back when the child exits.
	z_port_t old_app;           // the port handed away: its acks still come
	uint32_t prev_pid;          // who handed it away (posix), 0 if nobody
	char prev_target[32];
	uint32_t last_input;        // tick of the peer's last input
	uint32_t stall_since;       // tick output toward the peer last moved
	uint32_t stall_left;        // bytes still to go then
	uint32_t connect_seq;       // order of our CONNECTs to one provider
	z_net_accept_t info;
	uint32_t deadline;

	uint8_t tn, tn_cmd;
	bool last_cr;
	uint32_t answered;          // telnet options already answered, by bit

	char pw[Z_AUTH_PW_MAX + 1];
	uint8_t pw_len, tries;

	// HTTP: the request as it arrives, then the file as it goes out
	uint8_t http;               // H_*
	bool peer_eof;              // Z_NET_EOF: the peer has finished sending
	char req[HTTP_REQ_MAX];
	uint16_t req_len;
	int fh;                     // open file, or -1
	uint32_t left;              // bytes of it still to send

	ssh_slot_t *ssh;            // K_SSH: its engine

	uint8_t to_net[TO_NET_MAX];
	uint16_t to_net_len;
	uint8_t to_app[TO_APP_MAX];
	uint16_t to_app_len;
	held_t held_net[HOLD_MAX];  // DATA from net, not yet processed
	uint8_t held_net_n;
	held_t held_app[HOLD_MAX];  // DATA from the backend
	uint8_t held_app_n;
} sess_t;

static sess_t sess[MAX_SESS];

static uint32_t net_pid;
static uint32_t connect_seq;

static struct {
	bool on;
	uint16_t port;
	char target[32];            // telnet: the port name to connect to
	bool listening;
} svc_telnet, svc_echo, svc_http, svc_ssh;

static char http_root[64];      // the directory HTTP serves, no trailing slash

static bool allow_any;

// -- small helpers --

static void wipe(void *p, uint32_t n) {
	volatile uint8_t *v = p;
	while (n--) *v++ = 0;
}

static void print_ip(uint32_t ip) {
	printf("%lu.%lu.%lu.%lu", (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 0xFF),
		(unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF));
}

static void ack(uint32_t from, uint32_t tag) {
	z_msg_t m;
	m.from = from;
	m.tag = tag;
	m.obj.type = Z_BLOB;
	z_port_send_ack(&m);
}

static sess_t *by_net(uint32_t tag) {
	if (tag < 1 || tag > MAX_SESS) return NULL;
	sess_t *s = &sess[tag - 1];
	return s->state ? s : NULL;
}

static sess_t *by_app(uint32_t from, uint32_t tag) {
	for (int i = 0; i < MAX_SESS; i++)
		if (sess[i].state && sess[i].app.connected && sess[i].app_pid == from &&
				sess[i].app.conn_id == tag)
			return &sess[i];
	return NULL;
}

// Room in to_net for n bytes.
static bool net_room(sess_t *s, uint32_t n) {
	return (uint32_t)(TO_NET_MAX - s->to_net_len) >= n;
}

static void net_out(sess_t *s, const void *d, uint32_t n) {
	if (!net_room(s, n)) n = (uint32_t)(TO_NET_MAX - s->to_net_len);
	memcpy(s->to_net + s->to_net_len, d, n);
	s->to_net_len = (uint16_t)(s->to_net_len + n);
}

static void net_str(sess_t *s, const char *t) {
	net_out(s, t, (uint32_t)strlen(t));
}

// Ends a session: tells whichever sides are still connected, then waits
// (S_DRAIN) until every DATA it sent has been acked -- zport frees a
// send's memory on its ack, so freeing the session sooner would leak it.
static void end(sess_t *s, bool close_net) {
	if (s->fh >= 0) { fs_close_handle(s->fh); s->fh = -1; }
	for (int i = 0; i < s->held_net_n; i++) ack(net_pid, s->held_net[i].tag);
	for (int i = 0; i < s->held_app_n; i++) ack(s->app_pid, s->held_app[i].tag);
	s->held_net_n = s->held_app_n = 0;
	if (s->app.connected) z_port_close(&s->app);
	if (close_net && s->net.connected) z_port_close(&s->net);
	s->app.connected = s->net.connected = false;
	wipe(s->pw, sizeof(s->pw));
	s->state = S_DRAIN;
	// A peer that died never acks: free the slot anyway, after a while,
	// and let its last few sends leak rather than the session.
	s->deadline = z_uptime_ticks() + 5u * Z_TICK_HZ;
}

// Closes the TCP side after what is queued for it has gone out.
static void close_after_flush(sess_t *s) {
	s->state = S_CLOSING;
}

// -- the backend --

static const char *program_for(const char *name) {
	if (!strcmp(name, "repl0")) return "repl";
	if (!strcmp(name, "posix0")) return "posix";
	return NULL;
}

static void app_connect(sess_t *s) {
	uint32_t pid;
	if (!z_pid_lookup(s->target, &pid)) {
		const char *prog = program_for(s->target);
		if (!prog) {
			net_str(s, "\r\nnetserve: no such port: ");
			net_str(s, s->target);
			net_str(s, "\r\n");
			close_after_flush(s);
			return;
		}
		// Started once for all sessions waiting on it: a second
		// z_proc_run() while it loads would start a second copy.
		bool starting = false;
		for (int i = 0; i < MAX_SESS; i++) if (sess[i].state == S_LAUNCH) starting = true;
		if (!starting && !z_proc_run(prog)) {
			net_str(s, "\r\nnetserve: could not start ");
			net_str(s, prog);
			net_str(s, "\r\n");
			close_after_flush(s);
			return;
		}
		s->state = S_LAUNCH;
		s->deadline = z_uptime_ticks() + LAUNCH_TICKS;
		return;
	}
	s->app_pid = pid;
	s->app.connected = false;
	s->connect_seq = ++connect_seq;
	s->state = S_CONNECT;
	s->deadline = z_uptime_ticks() + LAUNCH_TICKS;
	// Tag 0, like every zport CONNECT: the providers answer with tag 0,
	// in order, so the answer is matched to our oldest open CONNECT to
	// that provider (app_answered()).
	z_msg_new_send(pid, Z_PORT_CONNECT, 0, z_obj_none());
}

static sess_t *oldest_connecting(uint32_t pid) {
	sess_t *best = NULL;
	for (int i = 0; i < MAX_SESS; i++) {
		sess_t *s = &sess[i];
		if (s->state == S_CONNECT && s->app_pid == pid &&
				(!best || (int32_t)(s->connect_seq - best->connect_seq) < 0))
			best = s;
	}
	return best;
}

// -- telnet: decoding what the peer sends --

static void tn_reply(sess_t *s, uint8_t cmd, uint8_t opt) {
	uint8_t b[3] = { IAC, cmd, opt };
	net_out(s, b, 3);
}

// One option command from the peer. We offered WILL ECHO and WILL SGA
// at the start; everything else is refused, once. RFC 1143's lesson,
// kept simple: never answer an option twice, so negotiation cannot
// loop.
static void tn_option(sess_t *s, uint8_t cmd, uint8_t opt) {
	uint32_t bit = (opt < 32) ? (1u << opt) : 0;
	if (bit && (s->answered & bit)) return;
	if (cmd == DO) {
		if (opt == OPT_ECHO || opt == OPT_SGA) return;      // the ack of our WILL
		tn_reply(s, WONT, opt);
	} else if (cmd == WILL) {
		tn_reply(s, DONT, opt);
	} else if (cmd == DONT && opt == OPT_ECHO) {
		// The peer will echo for itself -- including the password.
		// Nothing to do but agree; the banner has said this is not a
		// private line anyway.
		tn_reply(s, WONT, opt);
	}
	s->answered |= bit;
}

// Decodes telnet into plain bytes: options handled, IAC IAC unescaped,
// and every Enter -- CR LF, CR NUL, or a bare LF -- delivered as one CR,
// which is what the ports (zline.h) take as Enter. Returns bytes in out.
static uint32_t tn_decode(sess_t *s, const uint8_t *in, uint32_t n, uint8_t *out) {
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t c = in[i];
		switch (s->tn) {
		case TN_DATA:
			if (c == IAC) { s->tn = TN_IAC; break; }
			if (s->last_cr && (c == '\n' || c == 0)) { s->last_cr = false; break; }
			s->last_cr = (c == '\r');
			out[k++] = (c == '\n') ? '\r' : c;
			break;
		case TN_IAC:
			s->tn = TN_DATA;
			if (c == IAC) { s->last_cr = false; out[k++] = IAC; }
			else if (c == DO || c == DONT || c == WILL || c == WONT) { s->tn_cmd = c; s->tn = TN_OPT; }
			else if (c == SB) s->tn = TN_SB;
			break;
		case TN_OPT:
			tn_option(s, s->tn_cmd, c);
			s->tn = TN_DATA;
			break;
		case TN_SB:
			if (c == IAC) s->tn = TN_SB_IAC;
			break;
		case TN_SB_IAC:
			s->tn = (c == SE) ? TN_DATA : TN_SB;
			break;
		}
	}
	return k;
}

// -- logging in --

static void prompt(sess_t *s) {
	net_str(s, "password: ");
}

static void login_char(sess_t *s, uint8_t c) {
	uint32_t w = 0;
	int rc;

	if (c == 3 || c == 4) {                    // Ctrl+C, Ctrl+D
		net_str(s, "\r\n");
		close_after_flush(s);
		return;
	}
	if (c == 0x7f || c == 0x08) { if (s->pw_len) s->pw[--s->pw_len] = 0; return; }
	if (c != '\r') {
		if (c >= 0x20 && c < 0x7f && s->pw_len < Z_AUTH_PW_MAX) s->pw[s->pw_len++] = (char)c;
		return;
	}

	net_str(s, "\r\n");
	// About half a second inside the kernel, during which every other
	// session waits too. The kernel runs one check at a time anyway.
	rc = z_auth_check(s->pw, s->pw_len, &w);
	wipe(s->pw, sizeof(s->pw));
	s->pw_len = 0;

	printf("netserve: telnet login from ");
	print_ip(s->info.ip);
	printf(" %s\n", rc == Z_AUTH_OK ? "accepted" : "refused");

	if (rc == Z_AUTH_OK) {
		net_str(s, "connecting to ");
		net_str(s, s->target);
		net_str(s, "...\r\n");
		app_connect(s);
		return;
	}
	if (rc == Z_AUTH_E_WAIT) {
		char m[64];
		snprintf(m, sizeof(m), "Too many attempts -- try again in %lu s.\r\n",
			(unsigned long)((w + 999) / 1000));
		net_str(s, m);
		close_after_flush(s);
		return;
	}
	if (rc != Z_AUTH_E_BAD) {
		net_str(s, "This machine has no password set; telnet is off.\r\n");
		close_after_flush(s);
		return;
	}
	net_str(s, "Login incorrect.\r\n");
	if (++s->tries >= LOGIN_TRIES) { close_after_flush(s); return; }
	prompt(s);
}

// -- data --

// -- HTTP: static files (docs/netserve.md, "HTTP") --
//
// HTTP/1.0 in effect: one request per connection, answered, then
// closed (Connection: close). GET and HEAD. The file goes out as room
// appears in to_net, straight from the card, a chunk at a time.

static const char *http_type(const char *path) {
	static const struct { const char *ext, *type; } types[] = {
		{ ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" },
		{ ".txt", "text/plain; charset=utf-8" }, { ".md", "text/plain; charset=utf-8" },
		{ ".css", "text/css" }, { ".js", "text/javascript" }, { ".json", "application/json" },
		{ ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
		{ ".gif", "image/gif" }, { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" },
		{ ".pdf", "application/pdf" }, { ".wasm", "application/wasm" },
	};
	const char *dot = strrchr(path, '.');
	if (dot && !strchr(dot, '/'))
		for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
			const char *a = dot, *b = types[i].ext;
			while (*a && *b && (*a | 0x20) == *b) { a++; b++; }
			if (!*a && !*b) return types[i].type;
		}
	return "application/octet-stream";
}

static void http_log(sess_t *s, int code, uint32_t bytes) {
	printf("netserve: http ");
	print_ip(s->info.ip);
	printf(" %.40s -> %d, %lu bytes\n", s->req, code, (unsigned long)bytes);
}

// An error: a status line, a one-line page, and close. The request line
// is cut at its first CR for the log.
static void http_error(sess_t *s, int code, const char *reason) {
	char body[96], head[200];
	int bl = snprintf(body, sizeof(body), "<h1>%d %s</h1>\n", code, reason);
	int hl = snprintf(head, sizeof(head), "HTTP/1.0 %d %s\r\nServer: Zeitlos netserve\r\n"
		"Content-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n%s\r\n",
		code, reason, bl, code == 405 ? "Allow: GET, HEAD\r\n" : "");
	char *cr = strchr(s->req, '\r');
	if (cr) *cr = 0;
	net_out(s, head, (uint32_t)hl);
	net_out(s, body, (uint32_t)bl);
	http_log(s, code, (uint32_t)bl);
	s->http = H_SEND;
	s->left = 0;
	close_after_flush(s);
}

// %XX decoding, in place. False for a malformed escape or a NUL.
static bool url_decode(char *p) {
	char *o = p;
	for (; *p; p++) {
		if (*p != '%') { *o++ = *p; continue; }
		int v = 0;
		for (int k = 1; k <= 2; k++) {
			char c = p[k];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') v |= (c | 0x20) - 'a' + 10;
			else return false;
		}
		if (!v) return false;
		*o++ = (char)v;
		p += 2;
	}
	*o = 0;
	return true;
}

// Nothing that could leave the root: no ".." segment, no backslash, no
// control character. The root itself comes from the configuration.
static bool path_safe(const char *p) {
	if (p[0] != '/') return false;
	for (const char *q = p; *q; q++) {
		if (*q == '\\' || (unsigned char)*q < 0x20) return false;
		if (q[0] == '/' && q[1] == '.' && q[2] == '.' && (q[3] == '/' || !q[3])) return false;
	}
	return true;
}

static void http_respond(sess_t *s) {
	char method[8], target[HTTP_PATH_MAX], path[HTTP_PATH_MAX + 80], head[256];
	int n = 0, sp = 0;
	bool head_only;

	// "METHOD SP target SP HTTP/x.y"
	while (s->req[n] && s->req[n] != ' ' && n < (int)sizeof(method) - 1) { method[n] = s->req[n]; n++; }
	method[n] = 0;
	if (s->req[n] != ' ') { http_error(s, 400, "Bad Request"); return; }
	n++;
	while (s->req[n] && s->req[n] != ' ' && s->req[n] != '\r' && s->req[n] != '\n') {
		if (sp >= (int)sizeof(target) - 1) { http_error(s, 414, "URI Too Long"); return; }
		target[sp++] = s->req[n++];
	}
	target[sp] = 0;

	if (!strcmp(method, "GET")) head_only = false;
	else if (!strcmp(method, "HEAD")) head_only = true;
	else { http_error(s, 405, "Method Not Allowed"); return; }

	char *q = strpbrk(target, "?#");        // the query and fragment are not files
	if (q) *q = 0;
	if (!url_decode(target) || !path_safe(target)) { http_error(s, 400, "Bad Request"); return; }

	// the file, or a directory's index.html (then index.htm)
	snprintf(path, sizeof(path), "%s%s", http_root, target);
	size_t pl = strlen(path);
	int fh = -1;
	if (pl && path[pl - 1] != '/') fh = fs_open_read(path);
	if (fh < 0) {
		snprintf(path + pl, sizeof(path) - pl, "%sindex.html", (pl && path[pl - 1] == '/') ? "" : "/");
		fh = fs_open_read(path);
	}
	if (fh < 0) {
		pl = strlen(path);
		path[pl - 1] = 0;                   // index.html -> index.htm
		fh = fs_open_read(path);
	}
	if (fh < 0) { http_error(s, 404, "Not Found"); return; }

	uint32_t size = (uint32_t)fs_size(path);
	int hl = snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nServer: Zeitlos netserve\r\n"
		"Content-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
		http_type(path), (unsigned long)size);
	net_out(s, head, (uint32_t)hl);

	char *cr = strchr(s->req, '\r');
	if (cr) *cr = 0;
	http_log(s, 200, head_only ? 0 : size);

	s->http = H_SEND;
	if (head_only) {
		fs_close_handle(fh);
		s->left = 0;
		close_after_flush(s);
	} else {
		s->fh = fh;
		s->left = size;
	}
}

// Bytes of a request. The request ends at a blank line; what follows
// it -- a body, pipelined requests -- is not read (Connection: close).
static uint32_t http_in(sess_t *s, const uint8_t *d, uint32_t n) {
	if (s->http != H_REQ || s->state != S_OPEN) return n;       // a request is being answered
	for (uint32_t i = 0; i < n; i++) {
		if (s->req_len >= HTTP_REQ_MAX - 1) {
			s->req[s->req_len] = 0;
			http_error(s, 431, "Request Header Fields Too Large");
			return n;
		}
		s->req[s->req_len++] = (char)d[i];
		s->req[s->req_len] = 0;
		uint16_t L = s->req_len;
		if ((L >= 4 && !memcmp(s->req + L - 4, "\r\n\r\n", 4)) ||
				(L >= 2 && !memcmp(s->req + L - 2, "\n\n", 2))) {
			http_respond(s);
			return n;
		}
	}
	return n;
}

// The file, as room appears. Straight into to_net: no copy of it here.
static void http_pump(sess_t *s) {
	while (s->fh >= 0 && s->left && s->to_net_len < TO_NET_MAX) {
		uint32_t room = (uint32_t)(TO_NET_MAX - s->to_net_len);
		int got = fs_read_chunk(s->fh, s->to_net + s->to_net_len, (int)(room < s->left ? room : s->left));
		if (got <= 0) {
			// The file ended early, or the card failed: the client has
			// Content-Length and will see the connection end short.
			printf("netserve: http read failed with %lu bytes still to send\n",
				(unsigned long)s->left);
			s->left = 0;
			break;
		}
		s->to_net_len = (uint16_t)(s->to_net_len + got);
		s->left -= (uint32_t)got;
	}
	if (!s->left) {
		if (s->fh >= 0) { fs_close_handle(s->fh); s->fh = -1; }
		close_after_flush(s);
	}
}

// -- SSH (docs/netserve.md, "SSH") --

static bool ssh_write(void *user, const uint8_t *d, uint32_t n) {
	sess_t *s = user;
	ssh_slot_t *x = s->ssh;
	if (n > (uint32_t)(SSH_OUT_MAX - x->out_len)) return false;     // fatal to the session
	memcpy(x->out + x->out_len, d, n);
	x->out_len = (uint16_t)(x->out_len + n);
	return true;
}

static void ssh_random(void *user, uint8_t *out, uint32_t n) {
	(void)user;
	z_rng_bytes(out, n);
}

// The machine's password, through the kernel: its one tally of
// failures and its delays apply to SSH as to everything else.
static int ssh_auth(void *user, const char *username, const uint8_t *pw, uint32_t len) {
	sess_t *s = user;
	uint32_t w = 0;
	int rc = z_auth_check((const char *)pw, len, &w);
	printf("netserve: ssh login as '%s' from ", username);
	print_ip(s->info.ip);
	printf(" %s\n", rc == Z_AUTH_OK ? "accepted" : "refused");
	if (rc == Z_AUTH_OK) return SSHS_AUTH_OK;
	if (rc == Z_AUTH_E_BAD) return SSHS_AUTH_BAD;
	return SSHS_AUTH_REFUSE;
}

static void ssh_event(void *user, sshs_event_t ev, const uint8_t *d, uint32_t n, const char *text) {
	sess_t *s = user;
	switch (ev) {
	case SSHS_EV_LOG:
		printf("netserve: session %lu: %s\n", (unsigned long)((uint32_t)(s - sess) + 1), text);
		break;
	case SSHS_EV_SHELL:
		app_connect(s);
		break;
	case SSHS_EV_DATA:
		// Never more than the window, and the window is only re-opened
		// as bytes reach the port -- so in[] cannot overflow.
		if (n > (uint32_t)(SSHS_LOCAL_WINDOW - s->ssh->in_len)) n = (uint32_t)(SSHS_LOCAL_WINDOW - s->ssh->in_len);
		memcpy(s->ssh->in + s->ssh->in_len, d, n);
		s->ssh->in_len = (uint16_t)(s->ssh->in_len + n);
		s->last_input = z_uptime_ticks();
		break;
	case SSHS_EV_EOF:
		s->peer_eof = true;
		break;
	case SSHS_EV_CLOSED:
		printf("netserve: session %lu: %s\n", (unsigned long)((uint32_t)(s - sess) + 1),
			text ? text : "ssh: closed");
		if (s->state != S_CLOSING && s->state != S_DRAIN) close_after_flush(s);
		break;
	}
}

// Bytes from the client: all of them, into the engine -- unless its
// output has too little room left for what it may answer with, in
// which case they wait, held, until it has drained.
static uint32_t ssh_in(sess_t *s, const uint8_t *d, uint32_t n) {
	if (!s->ssh || s->state == S_CLOSING || s->state == S_DRAIN) return n;
	if (SSH_OUT_MAX - s->ssh->out_len < 600) return 0;
	sshs_feed(&s->ssh->eng, d, n);
	return n;
}

// Moves the engine's output on, and the client's data toward the port.
static void ssh_pump(sess_t *s) {
	ssh_slot_t *x = s->ssh;
	if (!x) return;
	if (x->out_len) {
		uint32_t room = (uint32_t)(TO_NET_MAX - s->to_net_len), k = x->out_len < room ? x->out_len : room;
		memcpy(s->to_net + s->to_net_len, x->out, k);
		s->to_net_len = (uint16_t)(s->to_net_len + k);
		memmove(x->out, x->out + k, x->out_len - k);
		x->out_len = (uint16_t)(x->out_len - k);
	}
	if (x->in_len && s->state != S_LOGIN) {
		uint32_t room = (uint32_t)(TO_APP_MAX - s->to_app_len), k = x->in_len < room ? x->in_len : room;
		memcpy(s->to_app + s->to_app_len, x->in, k);
		s->to_app_len = (uint16_t)(s->to_app_len + k);
		memmove(x->in, x->in + k, x->in_len - k);
		x->in_len = (uint16_t)(x->in_len - k);
	}
}

static void ssh_release(sess_t *s) {
	if (!s->ssh) return;
	memset(s->ssh, 0, sizeof(*s->ssh));         // keys, buffers, all of it
	s->ssh = NULL;
}

// Bytes from the peer: as many as there is room for. Returns how many
// were consumed; the caller holds the rest.
static uint32_t from_peer(sess_t *s, const uint8_t *d, uint32_t n) {
	uint8_t buf[TO_APP_MAX];
	uint32_t room, take, k;

	// A session on its way out takes everything, and uses none of it.
	if (s->state == S_CLOSING || s->state == S_DRAIN) return n;

	if (s->kind == K_HTTP) return http_in(s, d, n);
	if (s->kind == K_SSH) return ssh_in(s, d, n);

	if (s->kind == K_ECHO) {
		room = (uint32_t)(TO_NET_MAX - s->to_net_len);
		take = n < room ? n : room;
		net_out(s, d, take);
		return take;
	}

	// Telnet. Decoding writes at most one byte out per byte in, and an
	// option reply is three bytes for three in: so a chunk no larger
	// than the room left toward the peer cannot overflow it with
	// replies, and no larger than the room toward the backend cannot
	// overflow that.
	take = n;
	if (take > sizeof(buf)) take = sizeof(buf);
	room = (uint32_t)(TO_NET_MAX - s->to_net_len);
	if (take > room) take = room;
	if (s->state != S_LOGIN) {
		room = (uint32_t)(TO_APP_MAX - s->to_app_len);
		if (take > room) take = room;
	}
	if (take < 3 && take < n) return 0;     // room for an option reply first

	k = tn_decode(s, d, take, buf);
	for (uint32_t i = 0; i < k; i++) {
		if (s->state == S_LOGIN) {
			login_char(s, buf[i]);
			if (s->state != S_LOGIN) {
				// logged in: the rest is typeahead for the backend
				if (s->state == S_LAUNCH || s->state == S_CONNECT) {
					uint32_t rest = k - i - 1;
					if (rest > (uint32_t)(TO_APP_MAX - s->to_app_len))
						rest = (uint32_t)(TO_APP_MAX - s->to_app_len);
					memcpy(s->to_app + s->to_app_len, buf + i + 1, rest);
					s->to_app_len = (uint16_t)(s->to_app_len + rest);
				}
				break;
			}
		} else if (s->state == S_OPEN || s->state == S_LAUNCH || s->state == S_CONNECT ||
				s->state == S_AWAY) {
			s->to_app[s->to_app_len++] = buf[i];
		}
	}
	return take;
}

// Bytes from the backend, to the peer: as many as fit, a literal 0xFF
// going as IAC IAC. Returns how many were consumed.
static uint32_t from_app(sess_t *s, const uint8_t *d, uint32_t n) {
	uint32_t i;
	if (s->kind == K_SSH) {
		// The engine takes what the client's window, its maximum
		// packet and the room in out[] allow; the rest waits, held.
		if (!s->ssh || sshs_is_closed(&s->ssh->eng)) return n;      // nowhere to go
		return sshs_send(&s->ssh->eng, d, n, (uint32_t)(SSH_OUT_MAX - s->ssh->out_len));
	}
	for (i = 0; i < n; i++) {
		uint32_t need = (d[i] == IAC) ? 2 : 1;
		if ((uint32_t)(TO_NET_MAX - s->to_net_len) < need) break;
		s->to_net[s->to_net_len++] = d[i];
		if (d[i] == IAC) s->to_net[s->to_net_len++] = IAC;
	}
	return i;
}

// A DATA message arriving: consumed now as far as room allows, the rest
// held -- behind anything already held, so order is kept.
typedef uint32_t (*consume_t)(sess_t *, const uint8_t *, uint32_t);

static void take(sess_t *s, held_t *h, uint8_t *hn, const z_msg_t *m, consume_t f) {
	const uint8_t *d = z_blob_data(&m->obj);
	uint32_t len = z_blob_len(&m->obj), used = 0;
	if (!*hn) used = f(s, d, len);
	if (used == len) { z_port_send_ack(m); return; }
	if (*hn >= HOLD_MAX) { z_port_send_ack(m); return; }     // over zport's limit
	h[*hn].data = d;
	h[*hn].len = len;
	h[*hn].off = used;
	h[*hn].tag = m->tag;
	(*hn)++;
}

// Held messages, oldest first, as far as room allows; each acked when
// its last byte is used.
static void drain(sess_t *s, held_t *h, uint8_t *hn, consume_t f, uint32_t from) {
	while (*hn) {
		h[0].off += f(s, h[0].data + h[0].off, h[0].len - h[0].off);
		if (h[0].off < h[0].len) return;
		ack(from, h[0].tag);
		memmove(&h[0], &h[1], sizeof(held_t) * (size_t)(*hn - 1));
		(*hn)--;
	}
}

// -- a new connection --

static uint32_t cfg_port(const char *v, uint32_t dflt) {
	uint32_t p = 0;
	while (*v >= '0' && *v <= '9') p = p * 10 + (uint32_t)(*v++ - '0');
	return (p && p <= 65535) ? p : dflt;
}

static void refuse(const z_msg_t *m, const char *why) {
	z_msg_new_send(m->from, Z_PORT_REFUSED, m->tag, z_obj_str(why));
}

static void on_connect(const z_msg_t *m) {
	z_net_accept_t info;
	sess_t *s = NULL;
	int kind;

	if (m->obj.type != Z_BLOB || z_blob_len(&m->obj) < sizeof(info)) {
		refuse(m, "netserve: malformed");
		return;
	}
	memcpy(&info, z_blob_data(&m->obj), sizeof(info));

	if (svc_telnet.listening && info.lport == svc_telnet.port) kind = K_TELNET;
	else if (svc_echo.listening && info.lport == svc_echo.port) kind = K_ECHO;
	else if (svc_http.listening && info.lport == svc_http.port) kind = K_HTTP;
	else if (svc_ssh.listening && info.lport == svc_ssh.port) kind = K_SSH;
	else { refuse(m, "netserve: no service on that port"); return; }

	if (!allow_any && !(info.flags & Z_NET_ACCEPT_LOCAL)) {
		printf("netserve: refused ");
		print_ip(info.ip);
		printf(" -- not on this subnet (apps.netserve.allow)\n");
		refuse(m, "netserve: not from this subnet");
		return;
	}
	for (int i = 0; i < MAX_SESS; i++) if (!sess[i].state) { s = &sess[i]; break; }
	if (!s) { refuse(m, "netserve: too many sessions"); return; }
	ssh_slot_t *slot = NULL;
	if (kind == K_SSH) {
		for (int i = 0; i < SSH_MAX; i++) if (!ssh_pool[i].used) { slot = &ssh_pool[i]; break; }
		if (!slot) { refuse(m, "netserve: too many ssh sessions"); return; }
	}

	memset(s, 0, sizeof(*s));
	s->kind = (uint8_t)kind;
	s->relay = m->tag;
	s->info = info;
	s->net.peer_pid = m->from;
	s->net.conn_id = (uint32_t)(s - sess) + 1;
	s->net.connected = true;
	s->state = kind == K_TELNET ? S_LOGIN : S_OPEN;
	s->fh = -1;
	if (kind == K_SSH) {
		memset(slot, 0, sizeof(*slot));
		slot->used = true;
		s->ssh = slot;
		snprintf(s->target, sizeof(s->target), "%s", svc_ssh.target);
	}
	if (kind != K_SSH) snprintf(s->target, sizeof(s->target), "%s", svc_telnet.target);
	s->deadline = z_uptime_ticks() + LOGIN_TICKS;
	z_msg_new_send(m->from, Z_PORT_CONNECTED, m->tag, z_obj_uint32(s->net.conn_id));
	if (kind == K_SSH)
		sshs_init(&s->ssh->eng, host_seed, s, ssh_write, ssh_event, ssh_random, ssh_auth);

	if (kind == K_HTTP) s->deadline = z_uptime_ticks() + HTTP_TICKS;
	printf("netserve: %s session %lu from ",
		kind == K_TELNET ? "telnet" : kind == K_HTTP ? "http" : kind == K_SSH ? "ssh" : "echo",
		(unsigned long)s->net.conn_id);
	print_ip(info.ip);
	printf("\n");

	if (kind == K_TELNET) {
		static const uint8_t offer[] = { IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA };
		net_out(s, offer, sizeof(offer));
		net_str(s, "\r\nZeitlos -- telnet is not encrypted: use it on a network you trust.\r\n\r\n");
		prompt(s);
	}
}

// -- messages --

static void listen_on(uint16_t port) {
	z_msg_new_send(net_pid, Z_NET_LISTEN, port, z_obj_uint32(port));
}

// Z_TERM_SET_PORT from a backend: connect this session somewhere else,
// exactly as term does. posix sends it to hand the terminal to a child
// (vi), and again, naming itself, to take it back when the child exits.
//
// The message names no session -- it goes to the terminal's pid, and
// every session here shares netserve's -- so it is taken to mean the
// session that typed most recently among those it could be for. Two
// telnet sessions running vi under the same posix at the same moment
// could be confused; posix itself matches a returning terminal by pid,
// and has the same limit.
static void handoff(const z_msg_t *m) {
	const char *name = NULL;
	sess_t *s = NULL;

	if (m->obj.type == Z_STR) name = m->obj.val.str;
	else if (m->obj.type == Z_MAP) {
		z_obj_t *n = z_map_find((z_obj_t *)&m->obj, "name");
		if (n && n->type == Z_STR) name = n->val.str;
	}
	if (!name || !*name) return;

	// The backend handing its session on (posix -> vi), or the
	// session's PARENT calling it back. The parent is obeyed in any
	// state, as term obeys it: a child that exits without closing its
	// port -- vi does exactly that, found on hardware -- leaves the
	// session still attached to it, and waiting for its CLOSE first
	// would wait forever.
	bool parent = false;
	for (int i = 0; i < MAX_SESS; i++) {
		sess_t *c = &sess[i];
		bool from_backend = c->state == S_OPEN && c->app.connected && c->app_pid == m->from;
		bool from_parent = c->prev_pid && c->prev_pid == m->from &&
			c->state != S_LOGIN && c->state != S_CLOSING && c->state != S_DRAIN;
		if ((from_backend || from_parent) &&
				(!s || (int32_t)(c->last_input - s->last_input) > 0)) {
			s = c;
			parent = from_parent;
		}
	}
	if (!s) return;

	printf("netserve: session %lu handed to %s\n",
		(unsigned long)((uint32_t)(s - sess) + 1), name);

	if (parent) {
		// The child is gone (the parent only calls back when it is):
		// drop its connection without a word, and whatever it never
		// acked with it.
		if (s->app.connected && s->app_pid != m->from) z_port_forget(&s->app);
		if (!strcmp(name, s->prev_target)) s->prev_pid = 0;     // back home
	} else if (s->app.connected) {
		s->prev_pid = s->app_pid;
		snprintf(s->prev_target, sizeof(s->prev_target), "%s", s->target);
		z_port_close(&s->app);      // as term does: the parent expects it
		// Keep receiving acks for what was sent to it, so their
		// memory is freed, not leaked.
		s->old_app = s->app;
		s->old_app.connected = true;
	}
	s->app.connected = false;
	s->app.pending_count = 0;
	s->app.pending_head = 0;
	snprintf(s->target, sizeof(s->target), "%.31s", name);
	app_connect(s);
}

static void on_msg(const z_msg_t *m) {
	sess_t *s;

	if (m->from == net_pid) {
		switch (m->subject) {
		case Z_NET_LISTEN_REPLY: {
			uint32_t port = m->tag, err = m->obj.val.uint32;
			if (port == svc_telnet.port && svc_telnet.on) svc_telnet.listening = !err;
			if (port == svc_echo.port && svc_echo.on) svc_echo.listening = !err;
			if (port == svc_http.port && svc_http.on) svc_http.listening = !err;
			if (port == svc_ssh.port && svc_ssh.on) svc_ssh.listening = !err;
			if (err) printf("netserve: net refused port %lu (error %lu)\n",
				(unsigned long)port, (unsigned long)err);
			else printf("netserve: listening on port %lu\n", (unsigned long)port);
			return;
		}
		case Z_PORT_CONNECT:
			on_connect(m);
			return;
		case Z_NET_EOF:
			// The peer finished sending; what it sent is all here
			// (DATA before this, in order). Each kind finishes its own
			// way: poll_session().
			s = by_net(m->tag);
			if (s) s->peer_eof = true;
			return;
		case Z_PORT_DATA:
			s = by_net(m->tag);
			if (!s || m->obj.type != Z_BLOB) {
				if (m->obj.type == Z_BLOB) z_port_send_ack(m);
				return;
			}
			s->last_input = z_uptime_ticks();
			take(s, s->held_net, &s->held_net_n, m, from_peer);
			return;
		case Z_PORT_DATA_ACK:
			// ..._closed: a session that has ended waits in S_DRAIN for
			// exactly these, and its port to net is closed by then.
			s = by_net(m->tag);
			if (s) z_port_handle_ack_closed(&s->net, m);
			return;
		case Z_PORT_CLOSE:
			s = by_net(m->tag);
			if (s && s->state != S_DRAIN) {
				printf("netserve: session %lu closed by the peer\n", (unsigned long)m->tag);
				s->net.connected = false;
				end(s, false);
			}
			return;
		}
		return;
	}

	// From a backend port.
	switch (m->subject) {
	case Z_TERM_SET_PORT:
		handoff(m);
		return;
	case Z_PORT_CONNECTED:
		s = oldest_connecting(m->from);
		if (!s || m->obj.type != Z_UINT32) return;
		s->app.peer_pid = m->from;
		s->app.conn_id = m->obj.val.uint32;
		s->app.connected = true;
		s->state = S_OPEN;
		return;
	case Z_PORT_REFUSED:
		s = oldest_connecting(m->from);
		if (!s) return;
		net_str(s, "netserve: the port refused: ");
		if (m->obj.type == Z_STR && m->obj.val.str) net_str(s, m->obj.val.str);
		net_str(s, "\r\n");
		close_after_flush(s);
		return;
	case Z_PORT_DATA:
		s = by_app(m->from, m->tag);
		if (!s || m->obj.type != Z_BLOB) {
			// not one of ours: a peer that died left someone sending
			// to its pid (zport.h, z_port_reject_stranger())
			z_port_reject_stranger(m);
			return;
		}
		take(s, s->held_app, &s->held_app_n, m, from_app);
		return;
	case Z_PORT_DATA_ACK:
		for (int i = 0; i < MAX_SESS; i++) {
			if (!sess[i].state) continue;
			if (sess[i].app_pid == m->from) z_port_handle_ack_closed(&sess[i].app, m);
			if (sess[i].old_app.peer_pid == m->from) z_port_handle_ack_closed(&sess[i].old_app, m);
		}
		return;
	case Z_PORT_CLOSE:
		s = by_app(m->from, m->tag);
		if (s) {
			s->app.connected = false;
			if (s->prev_pid) {
				// A child that had the terminal has finished: its
				// parent calls the session back (Z_TERM_SET_PORT). If it
				// does not, poll_session() goes back on its own.
				s->state = S_AWAY;
				s->deadline = z_uptime_ticks() + AWAY_TICKS;
			} else if (s->kind == K_SSH && s->ssh) {
				// exit-status, EOF and CLOSE to the client first; the
				// engine's CLOSED then closes the connection.
				sshs_end(&s->ssh->eng, 0);
			} else {
				close_after_flush(s);    // what it printed last still goes out
			}
		}
		return;
	}
}

// -- the loop's work --

static void poll_session(sess_t *s) {
	uint32_t now = z_uptime_ticks();

	// held input, in order, as room allows
	drain(s, s->held_net, &s->held_net_n, from_peer, net_pid);
	drain(s, s->held_app, &s->held_app_n, from_app, s->app_pid);

	// the backend, if we are waiting for it to start
	if (s->state == S_LAUNCH) {
		uint32_t pid;
		if (z_pid_lookup(s->target, &pid)) app_connect(s);
		else if ((int32_t)(now - s->deadline) >= 0) {
			net_str(s, "netserve: the port did not start\r\n");
			close_after_flush(s);
		}
	}
	if (s->state == S_AWAY && (int32_t)(now - s->deadline) >= 0) {
		// never called back: go back to where the session came from
		snprintf(s->target, sizeof(s->target), "%s", s->prev_target);
		s->prev_pid = 0;
		app_connect(s);
	}
	if (s->state == S_CONNECT && (int32_t)(now - s->deadline) >= 0) {
		net_str(s, "netserve: the port did not answer\r\n");
		close_after_flush(s);
	}
	if (s->kind == K_SSH && s->ssh) {
		ssh_pump(s);
		// 60 s to log in, as for telnet.
		if (!s->ssh->eng.authed && s->state == S_OPEN && (int32_t)(now - s->deadline) >= 0)
			sshs_disconnect(&s->ssh->eng, "login timed out");
	}
	if (s->kind == K_HTTP && s->state == S_OPEN) {
		if (s->http == H_REQ && (int32_t)(now - s->deadline) >= 0)
			http_error(s, 408, "Request Timeout");
		else if (s->http == H_REQ && s->peer_eof && !s->held_net_n)
			http_error(s, 400, "Bad Request");     // it stopped before the request ended
		else if (s->http == H_SEND)
			http_pump(s);
	}
	// A half-close ends echo once everything it was sent has been
	// echoed, and a telnet session once its input has reached the port.
	if (s->peer_eof && s->state != S_CLOSING && s->state != S_DRAIN && !s->held_net_n &&
			(s->kind == K_ECHO || (s->kind == K_TELNET && !s->to_app_len)))
		close_after_flush(s);
	if (s->kind == K_SSH && s->peer_eof && s->ssh && !s->ssh->in_len && !s->to_app_len &&
			s->state != S_CLOSING && s->state != S_DRAIN)
		sshs_end(&s->ssh->eng, 0);               // its CLOSED closes the connection
	if (s->state == S_LOGIN && (int32_t)(now - s->deadline) >= 0) {
		net_str(s, "\r\ntimed out\r\n");
		close_after_flush(s);
	}

	// A stall report: something waiting, either way, and nothing moved
	// for 3 s. The whole state in one line.
	{
		uint32_t left = s->to_net_len + s->to_app_len;
		for (int i = 0; i < s->held_app_n; i++) left += s->held_app[i].len - s->held_app[i].off;
		for (int i = 0; i < s->held_net_n; i++) left += s->held_net[i].len - s->held_net[i].off;
		left += s->net.pending_count + s->app.pending_count;
		if (!left) s->stall_since = now;
		else if (left != s->stall_left) { s->stall_since = now; s->stall_left = left; }
		else if (now - s->stall_since > 3u * Z_TICK_HZ) {
			printf("netserve: session %lu stalled (state %u): to peer %u buffered, %u msgs "
				"held; to port %u buffered, %u msgs held; unacked: %u to net, %u to the port\n",
				(unsigned long)((uint32_t)(s - sess) + 1), (unsigned)s->state,
				(unsigned)s->to_net_len, (unsigned)s->held_app_n, (unsigned)s->to_app_len,
				(unsigned)s->held_net_n, (unsigned)s->net.pending_count,
				(unsigned)s->app.pending_count);
			s->stall_since = now;
		}
	}

	// out to the peer, and in to the backend (never once ended)
	if (s->state != S_DRAIN && s->to_net_len && s->net.connected &&
			z_port_send(&s->net, s->to_net, s->to_net_len) == Z_OK)
		s->to_net_len = 0;
	if (s->state == S_OPEN && s->to_app_len && s->app.connected &&
			z_port_send(&s->app, s->to_app, s->to_app_len) == Z_OK) {
		// SSH: those bytes have reached the port, so the client may
		// send as many again.
		if (s->kind == K_SSH && s->ssh) sshs_consumed(&s->ssh->eng, s->to_app_len);
		s->to_app_len = 0;
	}

	if (s->state == S_CLOSING && !s->to_net_len && !s->held_app_n &&
			!(s->ssh && s->ssh->out_len))
		end(s, true);

	if (s->state == S_DRAIN && ((!s->net.pending_count && !s->app.pending_count &&
			!s->old_app.pending_count) ||
			(int32_t)(now - s->deadline) >= 0)) {
		printf("netserve: session %lu ended\n", (unsigned long)((uint32_t)(s - sess) + 1));
		ssh_release(s);
		memset(s, 0, sizeof(*s));
	}
}

// -- configuration --

// Whether SSH can be offered, and the host key if so. The same password
// rule as telnet -- it hands out a shell -- and two of its own: a
// seeded random source (an ephemeral key from a weak one gives the
// session to anyone who recorded it), and a host key that lasts. The
// key is a 32-byte seed in the flash key/value store, made on the first
// start; its fingerprint is printed every start, for a user to compare
// on their first connection.
static bool ssh_ready(z_auth_status_t *st) {
	uint32_t len = 0;
	uint8_t pub[32];
	char fp[64];

	if (z_auth_status(st) != Z_AUTH_OK || !(st->flags & Z_AUTH_HAS_PASSWORD)) {
		printf("netserve: ssh stays off -- no password is set (passwd)\n");
		return false;
	}
	if (!(st->flags & Z_AUTH_NET_OK)) {
		printf("netserve: ssh stays off -- the password is under %d characters\n", Z_AUTH_NET_MIN);
		return false;
	}
	if (!z_rng_secure()) {
		printf("netserve: ssh stays off -- no seeded random source (docs/trng.md)\n");
		return false;
	}
	if (z_kv_get(HOSTKEY_KEY, host_seed, sizeof(host_seed), &len) != Z_KV_OK || len != 32) {
		z_rng_bytes(host_seed, sizeof(host_seed));
		int rc = z_kv_set(HOSTKEY_KEY, host_seed, sizeof(host_seed));
		if (rc != Z_KV_OK) {
			printf("netserve: ssh stays off -- the host key cannot be stored (kv: %d); "
				"a new key every start would train users to ignore the warning\n", rc);
			return false;
		}
		printf("netserve: ssh host key created\n");
	}
	sshs_host_public(host_seed, pub, fp, sizeof(fp));
	printf("netserve: ssh host key %s\n", fp);
	return true;
}

static void read_config(void) {
	char v[Z_CFG_VAL_MAX];
	z_auth_status_t st;

	svc_telnet.on = false;
	svc_echo.on = false;
	svc_http.on = false;
	svc_ssh.on = false;
	allow_any = false;

	if (z_cfg_get("apps.netserve.allow", v, sizeof(v)) && !strcmp(v, "any")) allow_any = true;

	if (z_cfg_get("apps.netserve.echo", v, sizeof(v)) && strcmp(v, "off") && strcmp(v, "no")) {
		svc_echo.on = true;
		svc_echo.port = (uint16_t)cfg_port(v, 7);
	}

	// HTTP: a port, then the directory to serve -- "80 /www".
	if (z_cfg_get("apps.netserve.http", v, sizeof(v)) && strcmp(v, "off") && strcmp(v, "no")) {
		const char *t = v;
		svc_http.port = (uint16_t)cfg_port(v, 80);
		while (*t >= '0' && *t <= '9') t++;
		while (*t == ' ') t++;
		snprintf(http_root, sizeof(http_root), "%.63s", *t ? t : "/www");
		size_t rl = strlen(http_root);
		while (rl > 1 && http_root[rl - 1] == '/') http_root[--rl] = 0;
		if (rl == 1 && http_root[0] == '/') http_root[0] = 0;   // the card's root
		svc_http.on = true;
	}

	// SSH: a port, then the port name sessions connect to -- "22 posix0".
	if (z_cfg_get("apps.netserve.ssh", v, sizeof(v)) && strcmp(v, "off") && strcmp(v, "no")) {
		const char *t = v;
		svc_ssh.port = (uint16_t)cfg_port(v, 22);
		while (*t >= '0' && *t <= '9') t++;
		while (*t == ' ') t++;
		snprintf(svc_ssh.target, sizeof(svc_ssh.target), "%.31s", *t ? t : "posix0");
		svc_ssh.on = ssh_ready(&st);
	}

	if (z_cfg_get("apps.netserve.telnet", v, sizeof(v)) && strcmp(v, "off") && strcmp(v, "no")) {
		const char *t = v;
		svc_telnet.port = (uint16_t)cfg_port(v, 23);
		while (*t >= '0' && *t <= '9') t++;
		while (*t == ' ') t++;
		snprintf(svc_telnet.target, sizeof(svc_telnet.target), "%.31s", *t ? t : "repl0");
		svc_telnet.on = true;
		// A telnet password crosses the network in the clear, and
		// telnet hands out a shell: no password, or a short one, and
		// telnet stays off. docs/security.md.
		if (z_auth_status(&st) != Z_AUTH_OK || !(st.flags & Z_AUTH_HAS_PASSWORD)) {
			printf("netserve: telnet stays off -- no password is set (passwd)\n");
			svc_telnet.on = false;
		} else if (!(st.flags & Z_AUTH_NET_OK)) {
			printf("netserve: telnet stays off -- the password is under %d "
				"characters\n", Z_AUTH_NET_MIN);
			svc_telnet.on = false;
		}
	}
}

int main(void) {
	char name[24];
	uint32_t next_listen = 0;

	if (!z_pid_register("netserve", name, sizeof(name))) {
		printf("netserve: could not register -- already running?\n");
		return 1;
	}
	printf("netserve: starting as %s\n", name);
	memset(sess, 0, sizeof(sess));

	read_config();
	if (!svc_telnet.on && !svc_echo.on && !svc_http.on && !svc_ssh.on) {
		printf("netserve: nothing to serve -- see apps.netserve.* in docs/netserve.md\n");
		return 0;
	}
	if (svc_telnet.on)
		printf("netserve: telnet on port %u to %s, %s\n", (unsigned)svc_telnet.port,
			svc_telnet.target, allow_any ? "from anywhere" : "from this subnet only");
	if (svc_echo.on) printf("netserve: echo on port %u\n", (unsigned)svc_echo.port);
	if (svc_http.on) printf("netserve: http on port %u, serving %s\n", (unsigned)svc_http.port,
		http_root[0] ? http_root : "/");
	if (svc_ssh.on) printf("netserve: ssh on port %u to %s, %s\n", (unsigned)svc_ssh.port,
		svc_ssh.target, allow_any ? "from anywhere" : "from this subnet only");

	for (;;) {
		z_msg_t m;
		uint32_t now = z_uptime_ticks();

		// net, and a listen on each port: retried every two seconds
		// until net is up and has an address (DHCP can take a while).
		if ((int32_t)(now - next_listen) >= 0 &&
				((svc_telnet.on && !svc_telnet.listening) || (svc_echo.on && !svc_echo.listening) ||
				 (svc_http.on && !svc_http.listening) || (svc_ssh.on && !svc_ssh.listening))) {
			next_listen = now + 2u * Z_TICK_HZ;
			if (net_pid || z_pid_lookup("net0", &net_pid)) {
				if (svc_telnet.on && !svc_telnet.listening) listen_on(svc_telnet.port);
				if (svc_echo.on && !svc_echo.listening) listen_on(svc_echo.port);
				if (svc_http.on && !svc_http.listening) listen_on(svc_http.port);
				if (svc_ssh.on && !svc_ssh.listening) listen_on(svc_ssh.port);
			}
		}

		while (z_msg_read(&m) == Z_OK) on_msg(&m);

		bool busy = false;
		for (int i = 0; i < MAX_SESS; i++)
			if (sess[i].state) { poll_session(&sess[i]); busy = true; }

		// Messages wake this at once; the timeout is for the deadlines,
		// the listen retry and anything held waiting for room.
		z_proc_wait(busy ? 2 : Z_TICK_HZ / 5);
	}
}
