/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * mesh -- a Meshtastic client. See docs/mesh_app.md.
 *
 *   run mesh                  the window (mesh_ui.c)
 *
 * and, for scripts (automate, cron) and for testing, without a window:
 *
 *   run mesh nodes            the node list on the console, then exit
 *   run mesh send TEXT        to the primary channel; waits until the
 *                             mesh has heard it, then exits
 *   run mesh send #CH TEXT    to channel CH (index or name)
 *   run mesh dm NODE TEXT     to one node (short name, long name or
 *                             !a1b2c3d4); waits for delivery
 *
 * -- the transport --
 *
 * The node is reached through `serial`'s port, serial0, as a USB serial
 * client -- the same CONNECT `term`'s `usbserial` sends -- so `serial`
 * must be running. mesh never touches the USB syscalls itself
 * (sw/common/zusbcdc.h, "One owner").
 *
 * One rule matters more than any other: every DATA is ACKED THE MOMENT
 * ITS BYTES ARE COPIED, before parsing, before drawing. serial stops
 * reading the device when eight sends to us are unacked, and the
 * Heltec's CP2102 has no flow control from the ESP32 -- bytes it cannot
 * hand on are lost on the far side of the bridge (docs/mesh_app.md,
 * "Risks", 1). Drawing a whole window can take longer than that.
 *
 * No printf anywhere: docs/app_runtime.md. The console output below is
 * built by hand and written with puts().
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zport.h"
#include "zconnect.h"
#include "zrng.h"
#include "zsoc.h"
#include "zwin.h"
#include "zwm.h"

#include "mesh_pb.h"
#include "mesh_model.h"
#include "mesh_proto.h"
#include "mesh_session.h"
#include "mesh_ui.h"

#define RETRY_MS		3000u
#define SEND_WAIT_MS	30000u
#define DM_WAIT_MS		60000u

static mesh_model_t model;
static mesh_session_t sess;
static z_port_t port;
static uint32_t serial_pid;
static uint32_t t_try;
static bool gui;
static bool connecting;			// CONNECT sent, no answer yet
static uint32_t t_connect;

// How long to wait for serial to answer a CONNECT before deciding it is
// not there: the same two seconds z_port_connect() would have waited.
#define CONNECT_WAIT_MS	2000u

// Bytes from the port, copied out of each DATA before it is acked.
static uint8_t ring[8192];
static uint32_t ring_head, ring_tail, ring_drops;

static uint32_t now_ms(void) {
	return (uint32_t)((uint64_t)z_uptime_ticks() * 1000u / Z_TICK_HZ);
}

// -- console output, for the command-line modes --

static char ob[256];
static int on;

static void o(const char *s) {
	while (*s && on < (int)sizeof(ob) - 1) ob[on++] = *s++;
	ob[on] = 0;
}

static void ou(uint32_t v) {
	char t[12];
	int n = 0;
	if (!v) t[n++] = '0';
	while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n && on < (int)sizeof(ob) - 1) ob[on++] = t[--n];
	ob[on] = 0;
}

static void oend(void) {
	puts(ob);
	on = 0;
	ob[0] = 0;
}

// -- transport --

static int t_send(void *ctx, const uint8_t *p, uint32_t n) {
	(void)ctx;
	if (!port.connected) return 1;
	return z_port_send(&port, p, n) == Z_OK ? 0 : 1;
}

static uint32_t t_rand(void *ctx) {
	uint32_t v;
	(void)ctx;
	z_rng_bytes(&v, sizeof(v));
	return v;
}

static void ring_put(const uint8_t *p, uint32_t n) {
	uint32_t i;
	for (i = 0; i < n; i++) {
		uint32_t next = (ring_head + 1) % sizeof(ring);
		if (next == ring_tail) { ring_drops += n - i; return; }
		ring[ring_head] = p[i];
		ring_head = next;
	}
}

static bool ring_drain(void) {
	bool any = false;
	while (ring_tail != ring_head) {
		uint32_t end = ring_head > ring_tail ? ring_head : sizeof(ring);
		mesh_session_rx(&sess, ring + ring_tail, end - ring_tail);
		ring_tail = end % sizeof(ring);
		any = true;
	}
	return any;
}

static void set_link(int l) {
	if (gui) mesh_ui_link(l);
}

// Connecting is ASYNCHRONOUS: send the CONNECT, and take CONNECTED or
// REFUSED as ordinary messages in the loop (port_msg()).
//
// NOT z_port_connect_arg(). That blocks for up to two seconds and
// DISCARDS every other message while it waits (zport.h) -- and the
// first thing mesh did after opening its window was connect, so wm's
// first region and redraw were thrown away and the window stayed blank
// until it was dragged. Found on hardware. While no node is plugged in
// it was worse: every retry swallowed keys and redraw requests.
static void try_connect(void) {
	uint32_t p;
	// serial may have gone and come back as another pid: look it up
	// every time rather than trusting the last answer.
	if (!z_pid_lookup("serial0", &p)) {
		serial_pid = 0;
		set_link(LINK_NO_SERIAL);
		return;
	}
	serial_pid = p;
	port.peer_pid = p;
	port.conn_id = 0;
	port.connected = false;
	if (z_msg_new_send(p, Z_PORT_CONNECT, 0,
		z_obj_uint32(Z_CONN_USBSERIAL_ARG)) != Z_OK) {
		set_link(LINK_NO_SERIAL);
		return;
	}
	connecting = true;
	t_connect = now_ms();
}

static void connected(uint32_t conn_id) {
	connecting = false;
	port.conn_id = conn_id;
	port.connected = true;
	ring_head = ring_tail = 0;
	set_link(LINK_UP);
	mesh_session_up(&sess, now_ms());
}

// A port message, or false if it is not one.
static bool port_msg(z_msg_t *msg) {
	switch (msg->subject) {
	case Z_PORT_CONNECTED:
		if (connecting && msg->tag == 0 && msg->from == serial_pid)
			connected(msg->obj.val.uint32);
		return true;
	case Z_PORT_REFUSED:
		// No USB serial device, or another client has it.
		if (connecting && msg->tag == 0 && msg->from == serial_pid) {
			connecting = false;
			set_link(LINK_NO_DEVICE);
			t_try = now_ms();
		}
		return true;
	case Z_PORT_DATA:
		if (port.connected && msg->tag == port.conn_id) {
			uint8_t *d = z_blob_data(&msg->obj);
			uint32_t n = z_blob_len(&msg->obj);
			if (d && n) ring_put(d, n);
		}
		z_port_send_ack(msg);		// at once: see the header
		return true;
	case Z_PORT_DATA_ACK:
		z_port_handle_ack(&port, msg);
		return true;
	case Z_PORT_CLOSE:
		if (port.connected && msg->tag == port.conn_id &&
			msg->from == port.peer_pid) {
			port.connected = false;
			mesh_session_down(&sess);
			set_link(LINK_NO_DEVICE);
			if (!gui) {
				o("mesh: the node went away");
				oend();
			}
			t_try = now_ms();
		}
		return true;
	default:
		return false;
	}
}

// Everything that is not the window: reconnect, feed, tick.
static bool transport_step(void) {
	bool busy = ring_drain();
	uint32_t t = now_ms();
	if (connecting && t - t_connect >= CONNECT_WAIT_MS) {
		// No answer at all: serial is not running (or is stuck).
		connecting = false;
		serial_pid = 0;
		set_link(LINK_NO_SERIAL);
		t_try = t;
	}
	if (!port.connected && !connecting && t - t_try >= RETRY_MS) {
		t_try = t;
		try_connect();
	}
	mesh_session_tick(&sess, t);
	return busy;
}

static void goodbye(void) {
	if (sess.state != MESH_ST_DOWN) mesh_session_bye(&sess);
	// Let the DISCONNECT reach serial before the port goes.
	z_proc_wait(Z_TICK_HZ / 10);
	z_port_close(&port);
}

// -- the window --

static void ui_event(void *ctx, const mesh_ev_t *ev) { (void)ctx; mesh_ui_event(ev); }
static void ui_line(void *ctx, const char *s) { (void)ctx; mesh_ui_line(s); }

static int run_window(void) {
	z_msg_t msg;
	bool running = true;

	gui = true;
	sess.event = ui_event;
	sess.line = ui_line;
	if (!mesh_ui_open(&model, &sess)) {
		puts("mesh: no window (is wm running?)");
		return 1;
	}
	try_connect();
	t_try = now_ms();

	while (running) {
		bool busy = false;
		while (z_msg_read(&msg) == Z_OK) {
			if (port_msg(&msg)) { busy = true; continue; }
			if (!mesh_ui_wm(&msg)) running = false;
		}
		if (transport_step()) busy = true;
		mesh_ui_flush();
		z_proc_wait(busy ? 1 : Z_TICK_HZ / 50);
	}
	goodbye();
	mesh_ui_close();
	return 0;
}

// -- the command line --

#define MODE_NODES	1
#define MODE_SEND	2
#define MODE_DM		3

static int cli_mode;
static bool cli_done;

static void cli_event(void *ctx, const mesh_ev_t *ev) {
	(void)ctx;
	if (ev->kind == MESH_EV_CONFIG_DONE) cli_done = true;
	if (ev->kind == MESH_EV_NOTIFY) { o("mesh: node says: "); o(ev->text); oend(); }
}

static void print_msg(const mesh_msg_t *g) {
	char nm[MESH_LONG_MAX];
	o("mesh: ");
	if (g->dm) { o("[DM "); o(mesh_name(&model, g->to, nm, sizeof(nm))); o("] "); }
	else { o("[#"); o(mesh_chan_name(&model, g->channel)); o("] "); }
	o(g->text);
	if (g->status != MSG_RX) { o("  ["); o(mesh_status_name(g)); o("]"); }
	oend();
}

static void print_nodes(void) {
	int i;
	char id[12];
	o("mesh: node ");
	mesh_node_id(model.my_num, id);
	o(id);
	if (model.firmware[0]) { o(", firmware "); o(model.firmware); }
	o(", ");
	ou((uint32_t)mesh_node_count(&model));
	o(" node(s)");
	oend();
	for (i = 0; i < MESH_MAX_CHANNELS; i++) {
		if (model.chan[i].role == CH_ROLE_DISABLED) continue;
		o("mesh:   channel ");
		ou((uint32_t)i);
		o(": #");
		o(mesh_chan_name(&model, i));
		if (model.chan[i].role == CH_ROLE_PRIMARY) o(" (primary)");
		oend();
	}
	for (i = 0; i < MESH_MAX_NODES; i++) {
		mesh_node_t *n = &model.node[i];
		if (!n->used) continue;
		mesh_node_id(n->num, id);
		o("mesh:   ");
		o(id);
		o("  ");
		o(n->has_user ? n->short_name : "?");
		o("  ");
		o(n->has_user ? n->long_name : "");
		if (n->num == model.my_num) o("  (this node)");
		if (n->hops_away >= 0) { o("  hops "); ou((uint32_t)n->hops_away); }
		if (n->battery == 101) o("  powered");
		else if (n->battery <= 100) { o("  batt "); ou(n->battery); o("%"); }
		oend();
	}
	if (model.region == 0) {
		o("mesh: WARNING: the node's LoRa region is not set; it will not transmit");
		oend();
	}
}

static const char *word(const char *s, char *w, int cap) {
	int n = 0;
	while (*s == ' ') s++;
	while (*s && *s != ' ') { if (n < cap - 1) w[n++] = *s; s++; }
	w[n] = 0;
	while (*s == ' ') s++;
	return s;
}

static int find_channel(const char *s) {
	int i;
	if (s[0] >= '0' && s[0] <= '7' && !s[1])
		return model.chan[s[0] - '0'].role != CH_ROLE_DISABLED ? s[0] - '0' : -1;
	for (i = 0; i < MESH_MAX_CHANNELS; i++) {
		const char *a = mesh_chan_name(&model, i), *b = s;
		if (model.chan[i].role == CH_ROLE_DISABLED) continue;
		while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
		if (!*a && !*b) return i;
	}
	return -1;
}

static int run_cli(const char *arg) {
	char w[16], dest[48];
	const char *text;
	mesh_msg_t *out = NULL;
	uint32_t t_sent = 0, t0;
	z_msg_t msg;
	int rc = 0;

	dest[0] = 0;
	text = word(arg, w, sizeof(w));
	if (!strcmp(w, "nodes")) cli_mode = MODE_NODES;
	else if (!strcmp(w, "send")) {
		cli_mode = MODE_SEND;
		if (*text == '#') text = word(text + 1, dest, sizeof(dest));
	} else if (!strcmp(w, "dm")) {
		cli_mode = MODE_DM;
		text = word(text, dest, sizeof(dest));
	}
	if (!cli_mode || (cli_mode != MODE_NODES && !*text) ||
		(cli_mode == MODE_DM && !dest[0])) {
		puts("usage: mesh [nodes | send [#CH] TEXT | dm NODE TEXT]");
		puts("       (no argument: the window)");
		return 1;
	}
	if (strlen(text) > MESH_PAYLOAD_MAX) {
		o("mesh: the message is too long; the limit is ");
		ou(MESH_PAYLOAD_MAX);
		o(" bytes");
		oend();
		return 1;
	}

	sess.event = cli_event;
	try_connect();
	t0 = t_try = now_ms();
	// Wait for serial's answer here, where nothing else is going on.
	while (connecting) {
		while (z_msg_read(&msg) == Z_OK) port_msg(&msg);
		if (now_ms() - t_connect >= CONNECT_WAIT_MS) connecting = false;
		else z_proc_wait(1);
	}
	if (!port.connected) {
		puts("mesh: cannot reach the node: is `serial` running and the node plugged in?");
		return 1;
	}

	for (;;) {
		uint32_t t;
		while (z_msg_read(&msg) == Z_OK) port_msg(&msg);
		transport_step();
		t = now_ms();

		if (!cli_done && t - t0 > 45000u) {
			puts("mesh: the node did not send its configuration");
			rc = 1;
			break;
		}
		if (cli_done && cli_mode == MODE_NODES) { print_nodes(); break; }
		if (cli_done && !out) {
			uint32_t to = MESH_BROADCAST;
			int ch = 0;
			if (cli_mode == MODE_DM) {
				mesh_node_t *n = mesh_node_lookup(&model, dest);
				if (!n) {
					o("mesh: no node called '"); o(dest);
					o("' -- `run mesh nodes` lists them");
					oend();
					rc = 1;
					break;
				}
				to = n->num;
			} else if (dest[0]) {
				ch = find_channel(dest);
				if (ch < 0) { o("mesh: no channel '"); o(dest); o("'"); oend(); rc = 1; break; }
			}
			out = mesh_session_send_text(&sess, to, (uint8_t)ch, text,
				(uint32_t)strlen(text));
			if (!out) { puts("mesh: the node would not take the message"); rc = 1; break; }
			t_sent = t;
			print_msg(out);
		}
		if (out) {
			uint32_t wait = cli_mode == MODE_DM ? DM_WAIT_MS : SEND_WAIT_MS;
			bool done = out->status == MSG_FAILED || out->status == MSG_DELIVERED ||
				(cli_mode == MODE_SEND && (out->status == MSG_SENT ||
					out->status == MSG_UNHEARD));
			if (done || t - t_sent >= wait) {
				if (!done) puts("mesh: no word back yet; it may still arrive");
				print_msg(out);
				if (out->status == MSG_FAILED) rc = 1;
				break;
			}
		}
		z_proc_wait(Z_TICK_HZ / 50);
	}
	goodbye();
	return rc;
}

int main(void) {
	char arg[Z_WM_ARG_MAX];
	bool have_arg;

	mesh_model_init(&model);
	mesh_session_init(&sess, &model);
	sess.send = t_send;
	sess.rand32 = t_rand;

	have_arg = z_launch_arg_take(arg, sizeof(arg)) && arg[0];
	return have_arg ? run_cli(arg) : run_window();
}
