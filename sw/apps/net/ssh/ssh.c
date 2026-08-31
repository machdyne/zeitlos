/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SSH client transport binding. See ssh.h for the split between this
 * file and ssh_proto.c, and for why prompts happen in band.
 */

#include <stdio.h>
#include <string.h>

#include "ssh.h"
#include "ssh_proto.h"
#include "../tcp.h"
#include "../../../common/zrng.h"
#include "../../../ext/monocypher/monocypher.h"

// Outbound queue. tcp.c sends one unacknowledged segment at a time
// (tcp.h), so the engine's writes have to be buffered somewhere --
// same reasoning as telnet.c's tx_queue, but larger, because an SSH
// handshake emits packets of a few hundred bytes back to back where
// telnet emits three-byte negotiation replies.
#define SSH_TX_QUEUE_LEN 2048

// What the user is currently being asked for. NONE means keystrokes
// go to the shell.
typedef enum {
	COLLECT_NONE,
	COLLECT_USER,
	COLLECT_HOSTKEY,	// "yes" or "no"
	COLLECT_PASSWORD	// not echoed
} collect_t;

static ssh_proto_t proto;
static bool active;

static ssh_out_handler_t app_on_out;
static ssh_ready_handler_t app_on_ready;
static ssh_closed_handler_t app_on_closed;

static uint8_t tx_queue[SSH_TX_QUEUE_LEN];
static uint16_t tx_queue_len;

static collect_t collecting;
static char line[SSH_MAX_PASS];
static uint16_t line_len;

static uint32_t target_ip;
static uint16_t target_port;
static char pending_user[SSH_MAX_USER];

static void out_str(const char *s) {
	if (app_on_out) app_on_out((const uint8_t *)s, (uint16_t)strlen(s));
}

// -- the engine's callbacks --

static bool proto_write(void *u, const uint8_t *data, uint32_t len) {

	(void)u;

	if ((uint32_t)tx_queue_len + len > SSH_TX_QUEUE_LEN) {
		// Not recoverable. Unlike telnet, where a dropped byte is a
		// glitched character, an SSH stream with a hole in it can
		// never resynchronise: every subsequent packet fails its tag.
		// Better to fail loudly here than to produce that.
		printf("ssh: outbound queue overflow (%d + %ld)\n",
			(int)tx_queue_len, (long)len);
		return false;
	}

	memcpy(tx_queue + tx_queue_len, data, len);
	tx_queue_len += (uint16_t)len;
	return true;

}

static void proto_random(void *u, uint8_t *out, uint32_t len) {
	(void)u;
	z_rng_bytes(out, len);
}

static void start_collect(collect_t what, const char *prompt) {
	collecting = what;
	line_len = 0;
	memset(line, 0, sizeof(line));
	if (prompt) out_str(prompt);
}

static void proto_event(void *u, ssh_event_t ev, const uint8_t *data,
	uint32_t len, const char *text) {

	(void)u;

	switch (ev) {

	case SSH_EV_STATUS:
		// To BOTH the window and the console. The window is where a
		// user wants it; the console is where it survives the window
		// being disconnected, which is exactly what happens when a
		// handshake fails partway.
		if (text) { out_str(text); out_str("\r\n"); printf("%s\n", text); }
		break;

	case SSH_EV_HOSTKEY:
		printf("ssh: host key %s\n", text ? text : "?");
		// Trust on first use, asked explicitly. There is no
		// known_hosts yet (ssh_hostkey.c is not written), so this is
		// the entire host identity check and it is worth showing
		// plainly rather than burying: the fingerprint is the only
		// thing standing between this session and a machine in the
		// middle.
		out_str("\r\nHost key fingerprint:\r\n  ");
		if (text) out_str(text);
		out_str("\r\nCompare this against the server before accepting.\r\n");
		start_collect(COLLECT_HOSTKEY, "Accept this host key (yes/no)? ");
		break;

	case SSH_EV_NEED_PASSWORD:
		if (text) { out_str("\r\n"); out_str(text); }
		out_str("\r\n");
		start_collect(COLLECT_PASSWORD, "Password: ");
		break;

	case SSH_EV_READY:
		out_str("\r\n");
		collecting = COLLECT_NONE;
		if (app_on_ready) app_on_ready();
		break;

	case SSH_EV_DATA:
	case SSH_EV_BANNER:
		if (app_on_out && data && len)
			app_on_out(data, (uint16_t)len);
		break;

	case SSH_EV_CLOSED:
		if (text) { out_str("\r\n"); out_str(text); out_str("\r\n"); }
		active = false;
		collecting = COLLECT_NONE;
		crypto_wipe((uint8_t *)line, sizeof(line));
		// tcp_abort() rather than tcp_close(): the SSH layer has
		// already said everything it intends to, and waiting out a
		// FIN exchange only delays freeing the single TCB.
		tcp_abort();
		if (app_on_closed) app_on_closed(text);
		break;

	}

}

// -- tcp events --

static void on_tcp_event(tcp_event_t ev, const uint8_t *data, uint16_t len) {

	switch (ev) {

	case TCP_EVENT_ESTABLISHED:
		printf("ssh: tcp established\n");
		out_str("Connected. Negotiating SSH...\r\n");
		// Starting the engine here, not in ssh_connect(), because
		// ssh_proto_init() immediately writes the client
		// identification string and there is nowhere to send it until
		// the handshake completes.
		ssh_proto_init(&proto, NULL, proto_write, proto_event,
			proto_random, pending_user);
		if (!pending_user[0])
			start_collect(COLLECT_USER, "login as: ");
		break;

	case TCP_EVENT_DATA:
		if (active) ssh_proto_feed(&proto, data, len);
		break;

	case TCP_EVENT_CLOSED:
		printf("ssh: tcp closed (active=%d)\n", (int)active);
		if (active) {
			active = false;
			collecting = COLLECT_NONE;
			if (app_on_closed) app_on_closed("ssh: connection closed");
		}
		break;

	}

}

// -- line collection --

// One character of a line being collected. Returns true when the line
// is complete.
static bool collect_char(uint8_t c) {

	if (c == '\r' || c == '\n') {
		line[line_len] = 0;
		if (collecting != COLLECT_PASSWORD) out_str("\r\n");
		else out_str("\r\n");
		return true;
	}

	// Backspace and DEL both, because a real terminal sends either
	// depending on how it is configured and getting this wrong makes
	// a mistyped password unfixable.
	if (c == 0x08 || c == 0x7f) {
		if (line_len) {
			line_len--;
			// Only echo the erase for fields that echo at all.
			if (collecting != COLLECT_PASSWORD) out_str("\b \b");
		}
		return false;
	}

	if (c < 0x20) return false;			// ignore other control bytes

	if (line_len < sizeof(line) - 1) {
		line[line_len++] = (char)c;
		if (collecting != COLLECT_PASSWORD) {
			uint8_t e = c;
			if (app_on_out) app_on_out(&e, 1);
		}
	}

	return false;

}

static void line_complete(void) {

	collect_t what = collecting;
	collecting = COLLECT_NONE;

	switch (what) {

	case COLLECT_USER:
		if (!line_len) {
			start_collect(COLLECT_USER, "login as: ");
			return;
		}
		ssh_proto_set_user(&proto, line);
		break;

	case COLLECT_HOSTKEY:
		if (!strcmp(line, "yes")) {
			out_str("Host key accepted for this session.\r\n");
			ssh_proto_accept_host(&proto);
		} else if (!strcmp(line, "no")) {
			ssh_proto_disconnect(&proto, "ssh: host key rejected");
		} else {
			// Deliberately not accepting "y". A one-keystroke answer
			// to "is this the right machine" is how people say yes to
			// a question they did not read.
			start_collect(COLLECT_HOSTKEY,
				"Please type exactly 'yes' or 'no': ");
		}
		break;

	case COLLECT_PASSWORD:
		ssh_proto_password(&proto, line);
		crypto_wipe((uint8_t *)line, sizeof(line));
		line_len = 0;
		break;

	default:
		break;

	}

}

// -- public API --

bool ssh_connect(uint32_t ip, uint16_t port, const char *user,
	ssh_out_handler_t on_out, ssh_ready_handler_t on_ready,
	ssh_closed_handler_t on_closed) {

	if (active) return false;

	// The gate that matters. See ssh.h.
	if (!z_rng_secure()) {
		printf("ssh: refusing to connect -- no trustworthy entropy source\n");
		return false;
	}

	app_on_out = on_out;
	app_on_ready = on_ready;
	app_on_closed = on_closed;

	tx_queue_len = 0;
	collecting = COLLECT_NONE;
	line_len = 0;
	target_ip = ip;
	target_port = port ? port : 22;

	memset(pending_user, 0, sizeof(pending_user));
	if (user) {
		strncpy(pending_user, user, sizeof(pending_user) - 1);
		pending_user[sizeof(pending_user) - 1] = 0;
	}

	printf("ssh: tcp connect to %ld.%ld.%ld.%ld:%d\n",
		(long)((ip >> 24) & 0xFF), (long)((ip >> 16) & 0xFF),
		(long)((ip >> 8) & 0xFF), (long)(ip & 0xFF), (int)target_port);

	if (!tcp_connect(ip, target_port, on_tcp_event)) {
		printf("ssh: tcp_connect refused -- another connection is open\n");
		return false;
	}

	active = true;
	return true;

}

void ssh_input(const uint8_t *data, uint16_t len) {

	uint16_t i;

	if (!active) return;

	if (collecting != COLLECT_NONE) {
		for (i = 0; i < len; i++) {
			if (collect_char(data[i])) {
				line_complete();
				// Anything after the newline belongs to whatever comes
				// next -- another prompt, or the shell. Recursing with
				// the remainder keeps a paste of "user\npassword\n"
				// working rather than silently eating the second line.
				if (i + 1 < len) ssh_input(data + i + 1, len - i - 1);
				return;
			}
		}
		return;
	}

	// Channel data. ssh_proto_send() caps at one channel packet, so a
	// large paste is split here rather than rejected.
	while (len) {
		uint16_t n = len > 512 ? 512 : len;
		if (!ssh_proto_send(&proto, data, n)) {
			// Server window exhausted, or the queue is full. Same
			// accepted fire-and-forget gap docs/ports.md documents:
			// dropping keystrokes is bad but recoverable, and there is
			// no backpressure path to the terminal to use instead.
			return;
		}
		data += n;
		len -= n;
	}

}

void ssh_close(void) {
	if (!active) return;
	ssh_proto_disconnect(&proto, "ssh: closed by user");
}

void ssh_abort(void) {
	if (!active) return;
	active = false;
	collecting = COLLECT_NONE;
	tx_queue_len = 0;
	crypto_wipe((uint8_t *)line, sizeof(line));
	tcp_abort();
}

void ssh_poll(void) {

	uint16_t n;

	if (!tcp_is_connected()) return;
	if (tx_queue_len == 0) return;

	n = tx_queue_len > TCP_MAX_PAYLOAD ? TCP_MAX_PAYLOAD : tx_queue_len;

	if (tcp_send(tx_queue, n)) {
		memmove(tx_queue, tx_queue + n, tx_queue_len - n);
		tx_queue_len -= n;
	}

}

bool ssh_is_active(void) {
	return active;
}
