/*
 * ttytest -- the smallest thing that asks `posix` for the terminal.
 *
 * Run it from a posix prompt:
 *
 *     $ ttytest
 *
 * It paints a full-screen box, waits for a key, and exits. The term
 * window should switch to it, show the box, and return to the posix
 * prompt when it quits.
 *
 * -- What this is for --
 *
 * docs/posix.md's 5.2 built the terminal handoff and nothing used it.
 * A mechanism with no client is a mechanism nobody has tested, and the
 * first program to exercise a path is worth making as small as
 * possible -- when `vi` eventually fails to appear, the question "is it
 * vi or is it the handoff" should already be answered.
 *
 * So this draws with plain VT100 escapes, keeps no state, and reads one
 * key. Everything that can go wrong here is the handoff.
 *
 * -- The sequence, which is the part that matters --
 *
 *   1. register a name       -- BEFORE asking, or term cannot resolve it
 *   2. ask posix0 for the tty -- connect with arg "tty:<name>"
 *   3. accept term's connect  -- term arrives a moment later
 *   4. draw, read a key
 *   5. close and exit         -- posix sees the exit, takes the tty back
 *
 * Step 1 before step 2 is the ordering that makes this work at all.
 * posix hands term a NAME and term resolves it through the pid
 * registry; ask first and term looks up a name that does not exist yet.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ, for the idle wait
#include "../../common/zport.h"
#include "../../common/zobj.h"

// Must match PX_TTY_TAG in sw/apps/posix/posix.h. Not included from
// there because that header pulls in the whole shell; the protocol is
// four characters and duplicating it is honest as long as it is said
// out loud, which is what this comment is for.
#define PX_TTY_TAG	"tty:"

static z_port_t conn;			// term, once it arrives
static z_port_t ask;			// the request channel to posix

static char name[24] = "ttytest";

// One send per frame: z_port_send() refuses past
// Z_PORT_MAX_PENDING_SENDS (8) unacked messages, and a screen painted
// with one send per line would lose its tail on a 25-line terminal.
// See sw/apps/repl/repl.c, which found that with paste echo.
static char screen[2048];

static void paint(void) {

	int n = 0;

	// \e[2J clear, \e[H home, \e[7m reverse, \e[0m normal.
	n += snprintf(screen + n, sizeof(screen) - n, "\033[2J\033[H");

	n += snprintf(screen + n, sizeof(screen) - n,
		"\033[7m  ttytest -- the terminal handoff works  \033[0m\r\n\r\n");

	n += snprintf(screen + n, sizeof(screen) - n,
		"  This window was handed over by posix.\r\n\r\n");

	n += snprintf(screen + n, sizeof(screen) - n,
		"  pid %ld, registered as '%s'\r\n\r\n",
		(long)z_getpid(), name);

	for (int row = 0; row < 3; row++) {
		n += snprintf(screen + n, sizeof(screen) - n, "  ");
		for (int col = 0; col < 40; col++)
			n += snprintf(screen + n, sizeof(screen) - n,
				"%c", (row + col) % 2 ? '#' : '.');
		n += snprintf(screen + n, sizeof(screen) - n, "\r\n");
	}

	n += snprintf(screen + n, sizeof(screen) - n,
		"\r\n  press q to quit -- F12 escapes to repl if this hangs\r\n");

	// Checked, because a refused send is silent otherwise and a blank
	// terminal is indistinguishable from a handoff that did not
	// happen. z_port_send() refuses past Z_PORT_MAX_PENDING_SENDS (8)
	// unacked messages; one frame per keystroke should never reach
	// that, and if it does the count is what says so.
	if (z_port_send(&conn, screen, (uint32_t)n) != Z_OK)
		printf("ttytest: paint refused (%d bytes)\n", n);

}

int main(void) {

	uint32_t posix_pid = 0;
	char arg[32];
	bool quit = false;

	// 1. The name first. See the header comment -- asking before
	//    registering gives term a name it cannot resolve.
	if (!z_pid_register("ttytest", name, sizeof(name))) {
		printf("ttytest: cannot register a name; nothing can find me\n");
		return 1;
	}

	printf("ttytest: pid %ld, registered as '%s'\n",
		(long)z_getpid(), name);

	// 2. Ask posix for the terminal.
	if (!z_pid_lookup("posix0", &posix_pid) || !posix_pid) {
		// Not an error worth dying over: run from the kernel shell
		// there is no posix and no terminal to be handed. Say so on
		// the console, which is where that message can actually be
		// read, and stop.
		printf("ttytest: no posix0 -- run me from a posix prompt\n");
		return 1;
	}

	snprintf(arg, sizeof(arg), "%s%s", PX_TTY_TAG, name);

	if (z_port_connect_arg(&ask, posix_pid, z_obj_str(arg)) != Z_OK) {
		printf("ttytest: posix0 would not take the request\n");
		return 1;
	}

	// posix accepts and immediately closes: the request is
	// acknowledged, the channel is done. Closing from this side too
	// keeps its connection table tidy.
	//
	// Everything this channel subsequently sends -- its CLOSE, and the
	// reply to ours -- carries ITS conn_id, and the loop below ignores
	// anything that is not term's. See the tag check there.
	z_port_close(&ask);

	printf("ttytest: asked posix0 for the terminal, waiting for term\n");

	while (!quit) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

			// Everything except CONNECT belongs to a specific
			// connection, and TWO of them exist here: the request
			// channel to posix, and the session with term.
			//
			// Filtered by SENDER, not by tag.
			//
			// zport.h suggests the tag -- "an app with exactly one
			// connection can just check msg.tag == port.conn_id" --
			// and that advice is exactly right for an app with ONE
			// connection. This one has two, and their conn_ids can
			// collide: posix assigns slot+1 to the request channel,
			// so a request landing in its slot 0 gets conn_id 1, and
			// a provider that assigns its own conn_id 1 to term then
			// cannot tell them apart.
			//
			// The sender cannot collide. msg.from is the pid the
			// message came from, and posix and term are different
			// processes.
			//
			// The first version guarded on `!conn.connected`, which
			// was a guess about which message ARRIVES FIRST. It was
			// wrong on hardware: posix's CLOSE for the request channel
			// landed after term's CONNECT, so ttytest closed the
			// terminal it had just been given and term dropped to
			// local echo.
			if (msg.subject != Z_PORT_CONNECT &&
				(!conn.connected || msg.from != conn.peer_pid)) {
				// Traced, not silently dropped. This filter is the
				// part of a handoff client that is easiest to get
				// wrong, and a message ignored for the wrong reason
				// looks identical to one that never arrived.
				printf("ttytest: ignored subject %lu from pid %lu "
					"(conn %s, peer %lu)\n",
					(unsigned long)msg.subject, (unsigned long)msg.from,
					conn.connected ? "up" : "down",
					(unsigned long)conn.peer_pid);
				continue;
			}

			if (msg.subject == Z_PORT_CONNECT) {

				// 3. term, arriving because posix pointed it here.
				if (conn.connected) {
					z_port_refuse(&msg, "ttytest: already in use");
					continue;
				}

				z_port_accept(&conn, &msg, 1);
				printf("ttytest: term connected, pid %lu conn %lu\n",
					(unsigned long)conn.peer_pid,
					(unsigned long)conn.conn_id);
				paint();

			} else if (msg.subject == Z_PORT_DATA) {

				uint32_t len = z_blob_len(&msg.obj);
				const uint8_t *data =
					(const uint8_t *)z_blob_data(&msg.obj);

				// Ack FIRST. The far end will not send again until
				// this arrives, so acking after the work below means a
				// terminal that cannot type while it happens.
				z_port_send_ack(&msg);

				for (uint32_t i = 0; i < len && !quit; i++) {
					if (data[i] == 'q' || data[i] == 'Q' ||
						data[i] == 0x1b) {			// Escape
						printf("ttytest: quit key\n");
						quit = true;
					} else {
						paint();					// any other key: repaint
					}
				}

			} else if (msg.subject == Z_PORT_DATA_ACK) {

				z_port_handle_ack(&conn, &msg);

			} else if (msg.subject == Z_PORT_CLOSE) {

				// Term leaving -- F12, or the window closing. The
				// sender check above has already ruled out posix's
				// CLOSE for the request channel.
				printf("ttytest: term (pid %lu) closed the connection\n",
					(unsigned long)msg.from);
				z_port_close(&conn);
				quit = true;

			}

		}

		// Blocks rather than spins. docs/app_runtime.md: one busy
		// process is a tax on every other one, and this program spends
		// almost all of its life waiting for a keystroke.
		z_proc_wait(Z_TICK_HZ / 20);

	}

	if (conn.connected) {
		const char *bye = "\033[2J\033[H";
		z_port_send(&conn, bye, (uint32_t)strlen(bye));
		z_port_close(&conn);
	}

	printf("ttytest: exiting -- posix should take the terminal back\n");

	// 5. Returning from main() exits. posix's child watcher
	//    (Z_SYS_PROC_STATUS) notices and hands the terminal back to
	//    itself before printing the next prompt.
	return 0;

}
