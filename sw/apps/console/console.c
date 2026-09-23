/*
 * console -- the kernel console as a port (docs/console.md)
 *
 * Serves "console0" to term (its CONSOLE button) and any other port
 * client. On connect it replays everything the kernel still holds in
 * its console log (boot messages, crash reports, the shell's output),
 * then streams new output as it appears. Keystrokes from the client go
 * to the kernel shell, as if typed on the serial console -- so the
 * prompt is shared, and its echo shows up on both.
 *
 * The log is the kernel's (sw/os/uart.c), read through Z_SYS_KLOG_READ
 * (sw/common/zconsole.h). Each client has its own position in it, so
 * several term windows can be connected at once, and a slow one only
 * lags: output is taken from the log only when a send succeeds, and
 * z_port_send() refuses once too many sends are unacknowledged.
 *
 * Deliberately no printf: this is resident on every machine, in flash
 * (a core app), and stdio would be most of its size.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../../common/zport.h"
#include "../../common/zconsole.h"

#define MAX_CLIENTS 4
#define CHUNK 128			// z_klog_read()'s per-call maximum

typedef struct {
	z_port_t port;			// port.connected: slot in use
	uint32_t pos;			// next byte of the log to send
} client_t;

static client_t clients[MAX_CLIENTS];
static uint8_t buf[CHUNK];

static const char LOST[] = "\r\n[... earlier console output was overwritten ...]\r\n";

static client_t *find(const z_msg_t *msg) {
	uint32_t slot = msg->tag - 1;	// conn_id is slot + 1
	if (slot >= MAX_CLIENTS) return NULL;
	client_t *c = &clients[slot];
	if (!c->port.connected || c->port.peer_pid != msg->from) return NULL;
	return c;
}

static void on_connect(z_msg_t *msg) {
	int slot = -1;
	// A client that reconnects without having sent CLOSE (a term that
	// was killed) reuses its old slot rather than leaking it.
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].port.connected && clients[i].port.peer_pid == msg->from)
			slot = i;
	for (int i = 0; slot < 0 && i < MAX_CLIENTS; i++)
		if (!clients[i].port.connected) slot = i;
	if (slot < 0) {
		z_port_refuse(msg, "console: too many connections");
		return;
	}
	memset(&clients[slot], 0, sizeof(clients[slot]));
	z_port_accept(&clients[slot].port, msg, (uint32_t)slot + 1);
	clients[slot].pos = 0;		// everything the kernel still holds
}

// Send whatever this client has not seen yet, until the port pushes
// back or the log is drained.
static void pump(client_t *c) {
	for (;;) {
		uint32_t pos = c->pos, lost = 0;
		uint32_t n = z_klog_read(&pos, buf, CHUNK, &lost);
		if (lost) {
			if (z_port_send(&c->port, LOST, sizeof(LOST) - 1) != Z_OK) return;
			c->pos = pos - n;	// the oldest byte still held
			continue;
		}
		if (!n) return;
		if (z_port_send(&c->port, buf, n) != Z_OK) return;
		c->pos = pos;
	}
}

int main(void) {
	char name[16];
	z_pid_register(Z_CONSOLE_NAME, name, sizeof(name));

	for (;;) {
		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			client_t *c;
			switch (msg.subject) {
			case Z_PORT_CONNECT:
				on_connect(&msg);
				break;
			case Z_PORT_DATA:
				if ((c = find(&msg)) != NULL) {
					uint32_t len = z_blob_len(&msg.obj);
					const uint8_t *data = z_blob_data(&msg.obj);
					if (data && len) z_console_input(data, len);
				}
				z_port_send_ack(&msg);
				break;
			case Z_PORT_DATA_ACK:
				if ((c = find(&msg)) != NULL) z_port_handle_ack(&c->port, &msg);
				break;
			case Z_PORT_CLOSE:
				if ((c = find(&msg)) != NULL) c->port.connected = false;
				break;
			default:
				break;
			}
		}
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].port.connected) pump(&clients[i]);

		// ~30 times a second: fast enough that typing at the prompt
		// feels immediate, and asleep otherwise.
		z_proc_wait(Z_TICK_HZ / 30);
	}
	return 0;
}
