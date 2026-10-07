/*
 * Host test for sw/common/zlisten.h. The decision is pure; the syscall
 * that feeds it is what the netserve test drives.
 *
 *   cc -std=gnu99 -Wall -Wextra -I sw/common -o /tmp/test_zlisten \
 *      sw/common/tests/test_zlisten.c
 *   /tmp/test_zlisten
 */
#include <stdio.h>

#include "zlisten.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

int main(void)
{
	CHECK(!z_listen_lost(0, false), "no pid yet: not a death");
	CHECK(!z_listen_lost(0, true), "no pid, and running, is not a death");
	CHECK(!z_listen_lost(2, true), "the net we listened to is still running");
	CHECK(z_listen_lost(2, false), "that net is not running: it has been replaced");

	/* The old idle sleep was a fifth of a second. Two polls have to
	 * fit in that, or a listener that used to wake that often can
	 * still step over the gap. */
	CHECK(Z_LISTEN_POLL_TICKS > 0, "the poll is a real wait");
	CHECK(Z_LISTEN_POLL_TICKS * 2 <= Z_TICK_HZ / 5, "two polls fit in the old idle sleep");
	CHECK(Z_LISTEN_POLL_TICKS <= Z_TICK_HZ / 10, "at least ten polls a second");

	uint32_t next = 0;
	CHECK(z_listen_due(1000, &next), "the first look is due");
	CHECK(next == 1000 + Z_LISTEN_POLL_TICKS, "the next look is one poll later");
	CHECK(!z_listen_due(next - 1, &next), "a tick early is not due");
	CHECK(z_listen_due(next, &next), "the poll instant is due");

	printf("zlisten test: %d checks, %d failed\n", run, failed);
	return failed != 0;
}
