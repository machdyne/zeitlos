/*
 * ping -- messaging demo app
 *
 * Sends MSG_PING messages (a Z_UINT32 counter) to pong and prints
 * pong's MSG_PONG replies.
 *
 * There's no way yet to pass a target pid to a launched app, so this
 * assumes pong was started first and got pid 1 (the first process
 * slot handed out after the kernel/shell, which is always pid 0):
 *
 *   > run pong
 *    - pid: 1
 *   > run ping
 *
 * Use `ps` in the shell to confirm pids if that assumption doesn't
 * hold on your setup.
 */

#include <stdio.h>
#include <stdint.h>

#include "../../common/zeitlos.h"

#define MSG_PING	1
#define MSG_PONG	2

#define PONG_PID	1

int main(void) {

	z_msg_t msg;
	uint32_t count = 0;

	printf("ping: sending to pid %d.\n", PONG_PID);

	// kick things off
	z_msg_new_send(PONG_PID, MSG_PING, 0, z_obj_uint32(count));

	while (1) {

		if (z_msg_wait(&msg, MSG_PONG, 0) != Z_OK) continue;

		// msg.obj.val.str is borrowed from pong's memory -- valid to
		// read here, but not to keep or free. we're done with it
		// before we send our own next message below, which is what
		// keeps that safe.
		printf("ping: got '%s' (count=%ld)\n", msg.obj.val.str, (long)count);

		count++;
		z_msg_new_send(PONG_PID, MSG_PING, 0, z_obj_uint32(count));

		for (volatile int i = 0; i < 500000; i++); // slow it down so it's readable

	}

}
