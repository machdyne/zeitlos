/*
 * Host tests for sw/apps/zerdesk/deskcfg.h.
 *
 *   cc -std=gnu99 -Wall -Wextra -I sw/apps/zerdesk -o /tmp/test_deskcfg \
 *      sw/apps/zerdesk/tests/test_deskcfg.c && /tmp/test_deskcfg
 */

#include <stdio.h>

#include "deskcfg.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

int main(void)
{
	CHECK(zd_clamp_port(8080) == 8080, "the default port is kept");
	CHECK(zd_clamp_port(1) == 1 && zd_clamp_port(65535) == 65535, "port range");
	CHECK(zd_clamp_port(0) == 8080 && zd_clamp_port(65536) == 8080 &&
		zd_clamp_port(-1) == 8080, "a bad port falls back to 8080");

	CHECK(zd_clamp_viewers(6) == 6, "the default viewer cap is kept");
	CHECK(zd_clamp_viewers(1) == 1 && zd_clamp_viewers(3) == 3, "viewer range");
	CHECK(zd_clamp_viewers(0) == 6 && zd_clamp_viewers(7) == 6, "a bad cap falls back to 6");

	CHECK(!zd_allow_any("subnet") && !zd_allow_any("") && !zd_allow_any(0),
		"subnet, empty and missing stay on the subnet");
	CHECK(!zd_allow_any("ANY") && !zd_allow_any("any "), "only the exact word");
	CHECK(zd_allow_any("any"), "any opens it");

	CHECK(zd_take_peer(0, 1), "a neighbour is taken");
	CHECK(!zd_take_peer(0, 0), "off-subnet is refused by default");
	CHECK(zd_take_peer(1, 0) && zd_take_peer(1, 1), "any takes both");

	CHECK(!zd_viewers_full(0, 3) && !zd_viewers_full(2, 3), "room for another viewer");
	CHECK(zd_viewers_full(3, 3) && zd_viewers_full(4, 1), "at the cap, and past it");

	CHECK(zd_next_conn(0) == 1, "the first id is 1, not 0");
	CHECK(zd_next_conn(1) == 2 && zd_next_conn(40) == 41, "ids only grow");
	CHECK(zd_next_conn(0xffffffffu) == 1, "wrap skips 0");

	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
