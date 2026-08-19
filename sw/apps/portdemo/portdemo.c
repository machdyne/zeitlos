/*
 * portdemo -- a virtual port provider with no real hardware behind
 * it: sends a banner on connect, then echoes back whatever it
 * receives. This is the test harness for sw/common/zport.h/.c and
 * for term's port-client code (sw/apps/term/term.c) -- see
 * docs/ports.md, "Planned phases". Single connection at a time; a
 * second CONNECT while already connected to someone else gets
 * refused.
 *
 * Started automatically at boot (sw/os/sh.c's init()). Registers as
 * "portdemo0" (see sw/os/pidreg.h) -- term.c looks this up now
 * instead of only using the fixed Z_PID_PORTDEMO constant (zport.h),
 * same migration wm/net already went through (docs/networking.md).
 * Z_PID_PORTDEMO is still there as a fallback if lookup ever fails
 * (registration failed, or an old term build that predates the
 * registry).
 *
 * Being a dumb byte-for-byte echo (not a real terminal/pty), it
 * doesn't interpret backspace (0x7f) specially -- term.c only does
 * its own "visual erase" trick in local-echo fallback mode, not
 * against a real connected port, so backspace against this demo
 * specifically has no visible effect. That's an honest limitation of
 * a minimal test harness, not a bug -- a real port (a UART, a pty)
 * would have its own remote doing something sensible with 0x7f.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zport.h"

static z_port_t conn;

static const char *BANNER =
	"portdemo: connected -- type something, it'll echo back.\r\n";

int main(void) {

	char name[24];
	if (z_pid_register("portdemo", name, sizeof(name)))
		printf("portdemo: starting as pid %ld, registered as '%s'.\n",
			(long)z_getpid(), name);
	else
		printf("portdemo: starting as pid %ld (name registration failed).\n",
			(long)z_getpid());

	conn.connected = false;

	while (1) {

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {

			if (msg.subject == Z_PORT_CONNECT) {

				if (conn.connected) {
					z_port_refuse(&msg, "portdemo: already connected to another client");
					continue;
				}

				z_port_accept(&conn, &msg, 1);
				printf("portdemo: client connected (pid %ld)\n", (long)msg.from);
				z_port_send(&conn, BANNER, (uint32_t)strlen(BANNER));

			} else if (msg.subject == Z_PORT_DATA) {

				if (!conn.connected || msg.tag != conn.conn_id) continue;

				uint32_t len = z_blob_len(&msg.obj);
				void *data = z_blob_data(&msg.obj);
				if (data && len) z_port_send(&conn, data, len);

			} else if (msg.subject == Z_PORT_CLOSE) {

				if (conn.connected && msg.tag == conn.conn_id) {
					conn.connected = false;
					printf("portdemo: client disconnected\n");
				}

			}

		}

	}

	return 0;

}
