/*
 * serial -- UART1 as a port, so `term` can talk to it.
 *
 * This is the process that OWNS UART1 (sw/common/zuart.h). Everything
 * else reaches the serial port by connecting to this one as a port
 * provider (sw/common/zport.h, docs/ports.md), exactly the way
 * everything reaches the network through `net` rather than touching
 * the MAC itself.
 *
 * From a term window:
 *
 *     > serial            (in repl -- 115200)
 *     > serial 9600
 *
 * repl sends term a Z_TERM_SET_PORT naming "serial0" with the baud
 * rate as the CONNECT argument, which is the same mechanism `telnet`
 * already uses to hand a term window to `net`. F12 comes back.
 *
 * -- Why a whole process rather than a library --
 *
 * Because UART1 is one piece of hardware and nothing arbitrates MMIO.
 * Two apps calling z_uart1_open() with different baud rates would each
 * believe they had configured the port, and the bytes would go out at
 * whichever rate was written last. A single owner is the only thing
 * that makes "what baud rate is the port at" a question with an
 * answer.
 *
 * It also gets `term` for free. term already knows how to be a port
 * client; a serial terminal is then a port provider that happens to
 * put bytes on a wire, and none of term's code cares which.
 *
 * -- One connection at a time --
 *
 * Same as portdemo, and here it is not a phase-1 limitation but the
 * point: there is one wire. Two term windows on one serial port would
 * interleave their keystrokes into one byte stream and split the
 * replies between them at random. A second CONNECT is refused with a
 * message saying so.
 *
 * -- Flow control does not exist --
 *
 * rtl/sysctl.v ties the 16550's CTS/DSR/RI/DCD inputs to 1, and
 * release/hw/pmods/usbuart1.spec only wires TX and RX. So there is no
 * hardware flow control in either direction, and this app does not
 * implement software flow control either.
 *
 * WHAT THAT MEANS IN PRACTICE: a far end that sends faster than this
 * process is scheduled will overrun the 16550's 16-byte receive FIFO
 * and bytes will be LOST. Not corrupted -- lost, silently, from the
 * middle of the stream. z_uart1_status()'s overrun bit is how this app
 * finds out, and it says so on the connection rather than hiding it:
 * a terminal session that quietly drops every 17th byte is worse than
 * one that tells you it did.
 *
 * At 115200 with a 1.365ms slice, one slice is 15.7 bytes of arrival
 * against a 16-byte FIFO -- which is why this polls at Z_TICK_HZ/60
 * rather than once per slice, and why the numbers in docs/uart1.md are
 * worth reading before choosing a baud rate.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../../common/zport.h"
#include "../../common/zuart.h"

// Default when a connection carries no baud rate. 115200 rather than
// the 1000000 the console uses: this port is for talking to somebody
// else's hardware, and 115200 is what somebody else's hardware is
// almost always set to.
#define DEFAULT_BAUD 115200

// One RX poll's worth. Sized to the 16550's FIFO -- there is never
// more than 16 bytes to collect, so a bigger buffer would only make
// the code look like it was doing something it is not.
#define RX_CHUNK 16

static z_port_t conn;
static uint32_t cur_baud;

static void say(const char *s) {
	if (conn.connected) z_port_send(&conn, s, (uint32_t)strlen(s));
}

int main(void) {

	char name[24];
	char msg_buf[96];

	if (z_pid_register("serial", name, sizeof(name)))
		printf("serial: starting as pid %ld, registered as '%s'.\n",
			(long)z_getpid(), name);
	else
		printf("serial: starting as pid %ld (name registration failed).\n",
			(long)z_getpid());

	// Refuse to pretend. A board without UART1 has nothing for this
	// app to own, and staying resident to refuse every connection
	// would be a process taking a scheduler share to say no. Exiting
	// means `term`'s name lookup fails and it stays in local echo,
	// which is the same clean failure any other absent port provider
	// gives.
	if (!z_uart1_present()) {
		printf("serial: this bitstream has no general-purpose UART1.\n");
		printf("serial: try `zrelease build obst_uart_uart1` "
			"-- see docs/uart1.md\n");
		return 1;
	}

	cur_baud = DEFAULT_BAUD;

	if (!z_uart1_open(cur_baud))
		printf("serial: could not open UART1 at %ld baud\n", (long)cur_baud);

	conn.connected = false;

	while (1) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

			if (msg.subject == Z_PORT_CONNECT) {

				uint32_t baud = DEFAULT_BAUD;

				if (conn.connected) {
					z_port_refuse(&msg,
						"serial: UART1 is already connected to another "
						"client -- there is only one wire");
					continue;
				}

				// The CONNECT argument is the baud rate, as a scalar,
				// arriving with the connect itself rather than in a
				// second message that could race it. Same shape as
				// telnet's target IP (sw/apps/net/net.c).
				//
				// Absent is not an error: `port serial0` connects
				// with no argument at all and gets whatever rate the
				// port is already at, which is the useful behaviour
				// for reconnecting.
				if (msg.obj.type == Z_UINT32 && msg.obj.val.uint32)
					baud = msg.obj.val.uint32;
				else
					baud = cur_baud;

				z_port_accept(&conn, &msg, 1);

				// Reopened unconditionally rather than only when the
				// rate changed: a previous client may have left the
				// FIFOs in some state, and reopening is cheap and
				// idempotent.
				{
					if (z_uart1_open(baud)) {
						cur_baud = baud;
					} else {
						// Refusing the RATE, not the connection: the
						// port still works at whatever it was at, and
						// saying which is more useful than dropping
						// the session. See z_uart1_baud_error() for
						// why a rate can be impossible rather than
						// merely awkward -- 921600 at 48MHz lands on
						// 1 Mbaud, 8.5% off.
						snprintf(msg_buf, sizeof(msg_buf),
							"serial: %ld baud is not reachable on this "
							"clock -- staying at %ld\r\n",
							(long)baud, (long)cur_baud);
						say(msg_buf);
					}
				}

				snprintf(msg_buf, sizeof(msg_buf),
					"serial: UART1 at %ld baud, 8N1, no flow control. "
					"F12 returns to repl.\r\n",
					(long)cur_baud);
				say(msg_buf);

				printf("serial: client connected (pid %ld) at %ld baud\n",
					(long)msg.from, (long)cur_baud);

				z_uart1_flush_rx();

			} else if (msg.subject == Z_PORT_DATA) {

				if (conn.connected && msg.tag == conn.conn_id) {

					uint32_t len = z_blob_len(&msg.obj);
					uint8_t *data = z_blob_data(&msg.obj);

					// z_uart1_write() BLOCKS, and here that is the
					// right call rather than the careless one: this
					// is at most a few keystrokes or a paste, the
					// transmitter always drains (nothing throttles
					// TX -- there is no CTS), and a partial write
					// would mean silently dropping the tail of what
					// somebody typed.
					//
					// The bound is real: worst case is this buffer's
					// length at the current baud rate. A 1KB paste at
					// 9600 baud would hold this process for a second,
					// during which term looks frozen. Worth knowing;
					// not worth a partial-write state machine until
					// somebody pastes a kilobyte into a 9600 baud
					// modem.
					if (data && len) z_uart1_write(data, len);

				}

				z_port_send_ack(&msg);

			} else if (msg.subject == Z_PORT_DATA_ACK) {

				z_port_handle_ack(&conn, &msg);

			} else if (msg.subject == Z_PORT_CLOSE) {

				if (conn.connected && msg.tag == conn.conn_id) {
					conn.connected = false;
					printf("serial: client disconnected\n");
				}

			}

		}

		// Pump the wire into the connection.
		//
		// Only while connected: with nobody listening there is
		// nowhere for the bytes to go, and draining them into a void
		// would make reconnecting show the middle of whatever the far
		// end had been saying. The FIFO overruns instead, which is
		// the honest outcome -- nothing was reading.
		if (conn.connected) {

			uint8_t rx[RX_CHUNK];
			uint32_t n = z_uart1_read(rx, sizeof(rx));
			uint32_t st;

			if (n) z_port_send(&conn, rx, n);

			// Overruns are REPORTED, not swallowed. A byte lost in
			// the middle of a terminal session is invisible until it
			// matters; a line saying so is how the user learns to
			// pick a slower rate. See this file's header.
			st = z_uart1_status();
			if (st & Z_UART1_OVERRUN) {
				say("\r\n[serial: receive overrun -- bytes lost. "
					"Try a lower baud rate.]\r\n");
			}
			if (st & Z_UART1_FRAMING) {
				say("\r\n[serial: framing error -- baud rate probably "
					"wrong.]\r\n");
			}

		}

		// Yield. Z_TICK_HZ/60 rather than /30 because at 115200 one
		// scheduler slice is already 15.7 bytes against a 16-byte
		// FIFO -- see this file's header. Even this is not a
		// guarantee, because a poll interval is a floor and not a
		// ceiling when something else is busy.
		z_proc_wait(Z_TICK_HZ / 60);

	}

	return 0;

}
