/*
 * serial -- UART1 as a port, so `term` can talk to it.
 *
 * -- and a USB CDC-ACM device, in the same process --
 *
 * One app, one port (`serial0`), two backends, each with its own
 * connection: UART1, and a USB CDC-ACM device through the kernel's
 * driver (sw/common/zusbcdc.h). A CONNECT picks the backend by its
 * argument: Z_CONN_USBSERIAL_ARG (sw/common/zconnect.h) means the USB
 * device -- what term's `usbserial` sends -- and anything else is
 * UART1's baud rate, or 0/none for "as it is", so `serial [baud]` and
 * `port serial0` are unchanged. The two can be connected at once, from
 * different windows: they are different wires. For the USB device there
 * is no baud rate to set -- the host sets 115200 8N1 at enumeration --
 * and no overrun or framing status: USB delivers whole packets or
 * nothing, and a device with more to send NAKs until it is read.
 *
 * This used to be two binaries, `serial` and `usbserial`, built from
 * this file with and without -DSERIAL_USB.
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
 * message saying so. The rule is per wire: UART1 and the USB device
 * each take one client, and both can be connected at once.
 *
 * -- Flow control is not built on any current target --
 *
 * rtl/uart.v can do auto-RTS/auto-CTS behind `UART1_FLOW (docs/uart.md),
 * but no target defines it, so rtl/sysctl.v ties cts_pad_i to 1, and
 * release/hw/pmods/usbuart1.spec only wires TX and RX. So there is no
 * hardware flow control in either direction, and this app does not
 * implement software flow control either.
 *
 * If a target ever wires the pins, the banner below and this comment
 * both become wrong -- check z_soc_has_feature2() rather than
 * assuming, or drop the claim from the banner entirely.
 *
 * WHAT THAT MEANS IN PRACTICE: a far end that sends faster than this
 * process is scheduled will overrun the UART's 16-byte receive FIFO
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
#include "../../common/zusbcdc.h"
#include "../../common/zconnect.h"	// Z_CONN_USBSERIAL_ARG

#define DEFAULT_BAUD 115200

// One UART1 read per poll: the FIFO is 16 bytes (see this file's header).
#define RX_CHUNK 16

// Connection ids, the tag on every DATA, DATA_ACK and CLOSE.
#define CONN_UART 1
#define CONN_USB  2

static z_port_t conn_uart, conn_usb;

// -- flow control: never read what cannot be forwarded --
//
// z_port_send() REFUSES a send once Z_PORT_MAX_PENDING_SENDS (8) are
// awaiting the client's ack, or when the client's mailbox is full --
// it returns Z_FAIL and sends nothing. This app used to ignore that.
// For the USB device it meant bytes already taken off the device --
// our host had ACKed the packet, so the device let it go -- were
// dropped on the floor: a full-screen redraw from a Blaustahl (~2 KB,
// 30-odd packets) filled the 8 pending sends in the first pass and the
// rest of the screen never arrived.
//
// So: read only while the port has room, and if a send fails anyway,
// hold the bytes and retry them before reading more. USB has its own
// backpressure -- a device we do not read NAKs and keeps its data -- so
// nothing is lost, the device just waits. UART1 has a 16-byte FIFO and
// no flow control, so there waiting can still overrun; that is then
// REPORTED, as before, instead of being lost silently.
static uint8_t held_usb[64], held_uart[16];
static uint32_t held_usb_n, held_uart_n;

static bool port_has_room(const z_port_t *c) {
	return c->pending_count < Z_PORT_MAX_PENDING_SENDS;
}

// Send what is held, if anything. false: still held, read nothing more.
static bool flush_held(z_port_t *c, uint8_t *held, uint32_t *n) {
	if (*n == 0) return true;
	if (!port_has_room(c) || z_port_send(c, held, *n) != Z_OK) return false;
	*n = 0;
	return true;
}

// Forward bytes just read; if the port refuses them, keep them.
static void forward(z_port_t *c, const uint8_t *data, uint32_t n,
		uint8_t *held, uint32_t *held_n) {
	if (z_port_send(c, data, n) != Z_OK) {
		memcpy(held, data, n);
		*held_n = n;
	}
}
static bool have_uart;
static uint32_t cur_baud;

static void say(z_port_t *c, const char *s) {
	if (c->connected) z_port_send(c, s, (uint32_t)strlen(s));
}

static void connect_usb(const z_msg_t *msg) {
	if (conn_usb.connected) {
		z_port_refuse(msg, "serial: the USB CDC device is already "
			"connected to another client -- there is only one wire");
		return;
	}
	if (!z_usbcdc_present()) {
		z_port_refuse(msg, "serial: no USB CDC-ACM device is plugged in");
		return;
	}
	z_port_accept(&conn_usb, msg, CONN_USB);
	held_usb_n = 0;
	say(&conn_usb, "serial: connected to the USB CDC device. "
		"F12 disconnects.\r\n");
	printf("serial: USB client connected (pid %ld)\n", (long)msg->from);
}

static void connect_uart(const z_msg_t *msg, char *buf, size_t buflen) {
	uint32_t baud;

	if (!have_uart) {
		z_port_refuse(msg, "serial: this bitstream has no UART1 -- see "
			"docs/uart1.md; `usbserial` reaches a USB CDC device");
		return;
	}
	if (conn_uart.connected) {
		z_port_refuse(msg, "serial: UART1 is already connected to another "
			"client -- there is only one wire");
		return;
	}

	// The CONNECT argument is the baud rate, as a scalar, arriving with
	// the connect itself. Absent or 0 is not an error: `port serial0`
	// gets whatever rate the port is already at, which is what you want
	// when reconnecting.
	if (msg->obj.type == Z_UINT32 && msg->obj.val.uint32)
		baud = msg->obj.val.uint32;
	else
		baud = cur_baud;

	z_port_accept(&conn_uart, msg, CONN_UART);
	held_uart_n = 0;

	// Reopened unconditionally: a previous client may have left the
	// FIFOs in some state, and reopening is cheap and idempotent.
	// Refusing the RATE, not the connection, if it cannot be reached --
	// see z_uart1_baud_error().
	if (z_uart1_open(baud)) {
		cur_baud = baud;
	} else {
		snprintf(buf, buflen, "serial: %ld baud is not reachable on this "
			"clock -- staying at %ld\r\n", (long)baud, (long)cur_baud);
		say(&conn_uart, buf);
	}
	snprintf(buf, buflen, "serial: UART1 at %ld baud, 8N1, no flow control. "
		"F12 disconnects.\r\n", (long)cur_baud);
	say(&conn_uart, buf);
	printf("serial: UART1 client connected (pid %ld) at %ld baud\n",
		(long)msg->from, (long)cur_baud);
	z_uart1_flush_rx();
}

// Whatever the USB device has, a few packets a pass: one read is one
// USB packet, and the device NAKs until the next is ready. A failed read
// is not a gone device -- one transaction can time out and the next
// work; only a device the kernel no longer has bound ends the session.
static bool poll_usb(void) {
	uint8_t rx[64];
	int k;
	bool moved = false;
	if (!flush_held(&conn_usb, held_usb, &held_usb_n)) return false;
	for (k = 0; k < 8; k++) {
		int32_t n;
		if (!port_has_room(&conn_usb)) break;	// the device waits
		n = z_usbcdc_read(rx, sizeof(rx));
		if (n < 0) {
			if (z_usbcdc_present()) break;
			say(&conn_usb, "\r\n[serial: the USB device went away]\r\n");
			printf("serial: USB device gone, client dropped\n");
			conn_usb.connected = false;
			break;
		}
		if (n == 0) break;
		moved = true;
		forward(&conn_usb, rx, (uint32_t)n, held_usb, &held_usb_n);
		if (held_usb_n) break;
	}
	return moved;
}

// Only while connected: with nobody listening there is nowhere for the
// bytes to go. Overruns and framing errors are REPORTED on the
// connection, not swallowed -- see this file's header.
static bool poll_uart(void) {
	uint8_t rx[RX_CHUNK];
	uint32_t n = 0;
	uint32_t st;

	// Waiting on the client leaves bytes in the FIFO; if they overrun it,
	// the status check below says so.
	if (flush_held(&conn_uart, held_uart, &held_uart_n) &&
	    port_has_room(&conn_uart))
		n = z_uart1_read(rx, sizeof(rx));
	if (n) forward(&conn_uart, rx, n, held_uart, &held_uart_n);
	st = z_uart1_status();
	if (st & Z_UART1_OVERRUN)
		say(&conn_uart, "\r\n[serial: receive overrun -- bytes lost. "
			"Try a lower baud rate.]\r\n");
	if (st & Z_UART1_FRAMING)
		say(&conn_uart, "\r\n[serial: framing error -- baud rate probably "
			"wrong.]\r\n");
	return n != 0;
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

	// Stays resident without UART1: the USB side may still be used, and
	// a CDC device can be plugged in at any time.
	have_uart = z_uart1_present();
	cur_baud = DEFAULT_BAUD;
	if (have_uart) {
		if (!z_uart1_open(cur_baud))
			printf("serial: could not open UART1 at %ld baud\n",
				(long)cur_baud);
	} else {
		printf("serial: no UART1 in this bitstream -- USB CDC only "
			"(see docs/uart1.md)\n");
	}

	conn_uart.connected = false;
	conn_usb.connected = false;

	while (1) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

			if (msg.subject == Z_PORT_CONNECT) {

				if (msg.obj.type == Z_UINT32 &&
				    msg.obj.val.uint32 == Z_CONN_USBSERIAL_ARG)
					connect_usb(&msg);
				else
					connect_uart(&msg, msg_buf, sizeof(msg_buf));

			} else if (msg.subject == Z_PORT_DATA) {

				uint32_t len = z_blob_len(&msg.obj);
				uint8_t *data = z_blob_data(&msg.obj);

				if (conn_uart.connected && msg.tag == conn_uart.conn_id) {
					if (data && len) z_uart1_write(data, len);
				} else if (conn_usb.connected &&
				           msg.tag == conn_usb.conn_id) {
					if (data && len && z_usbcdc_write(data, len) < 0)
						say(&conn_usb, "\r\n[serial: USB write failed -- "
							"device gone?]\r\n");
				}

				// Every DATA is acked, whoever it was for: the sender's
				// z_port_send() frees its buffer on the ack.
				z_port_send_ack(&msg);

			} else if (msg.subject == Z_PORT_DATA_ACK) {

				// Each ignores acks not tagged with its own id.
				z_port_handle_ack(&conn_uart, &msg);
				z_port_handle_ack(&conn_usb, &msg);

			} else if (msg.subject == Z_PORT_CLOSE) {

				if (conn_uart.connected && msg.tag == conn_uart.conn_id) {
					conn_uart.connected = false;
					printf("serial: UART1 client disconnected\n");
				} else if (conn_usb.connected &&
				           msg.tag == conn_usb.conn_id) {
					conn_usb.connected = false;
					printf("serial: USB client disconnected\n");
				}

			}

		}

		bool busy = false;
		if (conn_uart.connected && poll_uart()) busy = true;
		if (conn_usb.connected && poll_usb()) busy = true;

		// Yield. Z_TICK_HZ/60 rather than /30 because at 115200 one
		// scheduler slice is already 15.7 bytes against UART1's 16-byte
		// FIFO -- see this file's header. While data is flowing, come
		// back on the next tick instead: a USB device streaming a screen
		// (a Blaustahl redraw is ~2 KB) otherwise waits ~16 ms per 8
		// packets, and a device that drops what overflows its FIFO
		// rather than waiting loses more the slower it is read.
		z_proc_wait(busy ? 1 : Z_TICK_HZ / 60);
	}

	return 0;
}
