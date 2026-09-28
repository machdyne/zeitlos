/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * `zetta FILE` on Zeitlos: the terminal, handed over by posix, and the
 * loop. docs/zetta.md; the session itself is edit.c.
 *
 * The handoff is vi's (sw/apps/vi/vi_zeitlos.c, docs/posix.md 5.1-5.2):
 * register a name, ask posix0 for the terminal, accept term's
 * connection, run, and exit -- posix takes the terminal back when this
 * process ends. Unlike vi there is no serial-console fallback: a
 * full-screen editor on a raw UART is not a recovery tool worth having.
 */
#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zport.h"
#include "../../common/zobj.h"
#include "../../common/zwin.h"			// z_launch_arg_take()
#include "../../common/zplat.h"
#include "edit.h"

#define PX_TTY_TAG  "tty:"				// as sw/apps/posix/posix.h has it

static z_port_t conn, ask;
static bool term_gone;

// -- output: batched; the port takes 8 unacked sends at most --

#define OUT_BUF 1024
static char obuf[OUT_BUF];
static uint32_t olen;

static void pump(void);

static void flush(void) {
	if (!olen || !conn.connected) { olen = 0; return; }
	for (int t = 0; t < 64; t++) {				// bounded: a gone terminal must not hang us
		if (z_port_send(&conn, obuf, olen) == Z_OK) break;
		pump();
		z_proc_wait(1);
	}
	olen = 0;
}

static void out(void *ctx, const char *b, uint32_t n) {
	(void)ctx;
	while (n) {
		uint32_t take = OUT_BUF - olen < n ? OUT_BUF - olen : n;
		memcpy(obuf + olen, b, take);
		olen += take; b += take; n -= take;
		if (olen == OUT_BUF) flush();
	}
}

// -- input --

static bool done;

// Messages, without blocking: term arriving, keys, acks, term leaving.
// It never writes to the port itself (flush() calls it).
static uint8_t keys[512];
static uint32_t nkeys;

static void pump(void) {
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) {
		if (m.subject == Z_PORT_CONNECT) {
			if (conn.connected) z_port_refuse(&m, "zetta: already in use");
			else z_port_accept(&conn, &m, 1);
			continue;
		}
		if (!conn.connected || m.from != conn.peer_pid) continue;		// by sender (docs/posix.md 5.2)
		if (m.subject == Z_PORT_DATA) {
			uint32_t n = z_blob_len(&m.obj);
			const uint8_t *d = z_blob_data(&m.obj);
			z_port_send_ack(&m);
			if (n > sizeof(keys) - nkeys) n = (uint32_t)sizeof(keys) - nkeys;
			memcpy(keys + nkeys, d, n);
			nkeys += n;
		} else if (m.subject == Z_PORT_DATA_ACK) {
			z_port_handle_ack(&conn, &m);
		} else if (m.subject == Z_PORT_CLOSE) {
			z_port_close(&conn);
			term_gone = true;
		}
	}
}

int main(void) {
	char name[24], arg[40], file[128], why[200];
	uint32_t posix_pid = 0;

	if (!z_pid_register("zetta", name, sizeof(name))) { printf("zetta: cannot register a name\n"); return 1; }
	file[0] = 0;
	if (z_launch_arg_take(file, sizeof(file))) {
		char *sp = strchr(file, ' ');
		if (sp) *sp = 0;						// one file
	}
	if (!z_pid_lookup("posix0", &posix_pid) || !posix_pid) {
		printf("zetta: run it from posix -- it needs a terminal to draw on\n");
		return 1;
	}
	snprintf(arg, sizeof(arg), "%s%s", PX_TTY_TAG, name);
	if (z_port_connect_arg(&ask, posix_pid, z_obj_str(arg)) != Z_OK) {
		printf("zetta: posix0 would not hand over the terminal\n");
		return 1;
	}
	z_port_close(&ask);
	for (int i = 0; i < 200 && !conn.connected; i++) { pump(); if (!conn.connected) z_proc_wait(Z_TICK_HZ / 100); }
	if (!conn.connected) { printf("zetta: term never connected\n"); return 1; }

	if (!ze_open(file, 25, 80, out, NULL, why, sizeof(why))) {
		out(NULL, "\r\n", 2);
		out(NULL, why, (uint32_t)strlen(why));
		out(NULL, "\r\n", 2);
		flush();
		return 1;
	}
	flush();
	while (!done && !term_gone) {
		pump();
		if (nkeys) {
			uint32_t n = nkeys;
			nkeys = 0;
			done = ze_input(keys, n, plat_ms());
		} else done = ze_tick(plat_ms());
		flush();
		if (!done && !nkeys) z_proc_wait(Z_TICK_HZ / 50);		// ~20 ms: a lone Esc, held input
	}
	flush();
	return 0;
}
