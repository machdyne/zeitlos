/*
 * Zeitlos
 *
 * The RX FIFO status word (rtl/esp32_rxfifo.v, DEPTH_BITS 13) is
 *
 *   {{(31-DEPTH_BITS){1'b0}}, overrun, count}
 *
 * with count declared [DEPTH_BITS:0], so count is bits 13:0 and overrun
 * is bit 14. The driver masks that with RXFIFO_COUNT_MASK / RXFIFO_OVERRUN
 * in sw/apps/net/esp32link.c. Bit 12 is part of the count: a FIFO holding
 * 4096 bytes must not be reported as an overrun.
 *
 *   cc -O2 -o /tmp/test_rxfifo_status tests/test_rxfifo_status.c && /tmp/test_rxfifo_status
 */

#include <stdio.h>
#include <stdint.h>

#define DEPTH_BITS 13
#define RXFIFO_COUNT_MASK 0x3fffu
#define RXFIFO_OVERRUN    (1u << 14)

static uint32_t status_word(uint32_t count, int overrun)
{
	return ((overrun ? 1u : 0u) << (DEPTH_BITS + 1)) |
		(count & ((1u << (DEPTH_BITS + 1)) - 1));
}

static int fails;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

int main(void)
{
	uint32_t w;

	check(RXFIFO_COUNT_MASK == ((1u << (DEPTH_BITS + 1)) - 1),
		"count mask is the fourteen count bits");
	check(RXFIFO_OVERRUN == (1u << (DEPTH_BITS + 1)),
		"overrun is the bit just above count");
	check((RXFIFO_OVERRUN & RXFIFO_COUNT_MASK) == 0,
		"overrun is not part of the count");

	w = status_word(4096, 0);
	check((w & RXFIFO_COUNT_MASK) == 4096, "4096 bytes queued");
	check((w & RXFIFO_OVERRUN) == 0, "4096 bytes is not an overrun");
	check(((w >> 12) & 1) == 1, "bit 12 is set at 4096, and it is count");

	/* 500 mouse frames of 13 bytes, read back on the board as 0x1964. */
	w = status_word(6500, 0);
	check(w == 0x1964u, "6500 bytes, no overrun, is 0x1964");
	check((w & RXFIFO_OVERRUN) == 0, "that word's overrun bit is clear");
	check((w & RXFIFO_COUNT_MASK) == 6500, "that word's count is 6500");

	w = status_word(100, 1);
	check((w & RXFIFO_OVERRUN) != 0, "a raised overrun is bit 14");
	check((w & RXFIFO_COUNT_MASK) == 100, "count survives a raised overrun");

	w = status_word(8192, 1);
	check((w & RXFIFO_COUNT_MASK) == 8192, "a full FIFO still reports 8192");
	check((w & RXFIFO_OVERRUN) != 0, "and the overrun bit with it");
	check(((w >> 12) & 1) == 0, "bit 12 is clear at 8192");

	if (fails) {
		printf("%d failed\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
