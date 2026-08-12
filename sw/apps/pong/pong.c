/*
 * pong -- messaging demo app
 *
 * Waits for MSG_PING messages (a Z_UINT32 counter) and replies to
 * whoever sent them with a MSG_PONG message carrying a short string.
 *
 * Run this before ping (`run pong` then `run ping`) -- see ping.c.
 */

#include <stdio.h>
#include <stdint.h>

#include "../../common/zeitlos.h"

#define MSG_PING	1
#define MSG_PONG	2

int main(void) {

	z_msg_t msg;
	char reply[32];

	printf("pong: waiting for pings.\n");

	while (1) {

		if (z_msg_wait(&msg, MSG_PING, 0) != Z_OK) continue;

		printf("pong: got ping #%ld from pid %ld\n",
			(long)msg.obj.val.uint32, (long)msg.from);

		// z_obj_str() copies into pong's own heap, so this string
		// stays valid for ping to read even after we loop back
		// around and call z_msg_wait() again.
		snprintf(reply, sizeof(reply), "pong %ld", (long)msg.obj.val.uint32);

		z_msg_new_send(msg.from, MSG_PONG, 0, z_obj_str(reply));

		// note: we never free the string above. it's borrowed by
		// ping until ping sends its own next message, and pong has
		// no way to know when that's happened without an explicit
		// ack -- so for this simple demo we just accept the leak.
		// a longer-running version would want a reply-received ack
		// (or a bounded object pool) before freeing.

	}

}
