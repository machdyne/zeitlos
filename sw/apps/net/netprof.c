/*
 * Zeitlos
 *
 * See netprof.h. Nothing here is linked unless NET_PROFILE=1.
 */

#include "netprof.h"

#if NET_PROFILE

#include <stdio.h>
#include <string.h>
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"

#define NP_REPORT_TICKS (5 * Z_TICK_HZ)
#define NP_CPU_HZ       Z_SYSCLK_HZ

const char *np_name[NP_N] = {
	"wifi", "eth", "ip", "msg", "tftp", "tcp", "telnet",
	"sock", "ssh", "dns", "ntp", "screen", "snap", "pack", "send",
};

uint32_t np_cyc[NP_N], np_cnt[NP_N], np_min[NP_N];
uint32_t np_loops, np_slept;
uint32_t np_uart_bytes, np_uart_chunks, np_uart_spin;

static uint32_t np_last_tick;
static int np_primed;

void np_report(void)
{
	uint32_t now = z_uptime_ticks();

	if (!np_primed) {
		np_primed = 1;
		np_last_tick = now;
		for (int i = 0; i < NP_N; i++)
			np_min[i] = 0xffffffffu;
		return;
	}
	if ((uint32_t)(now - np_last_tick) < NP_REPORT_TICKS)
		return;

	uint32_t span = now - np_last_tick;	/* ticks */
	uint32_t secs10 = span * 10u / Z_TICK_HZ;	/* tenths of a second */
	if (!secs10)
		secs10 = 1;

	printf("np: %lu.%lus loops=%lu (%lu/s) slept=%lu\n",
		(unsigned long)(secs10 / 10), (unsigned long)(secs10 % 10),
		(unsigned long)np_loops,
		(unsigned long)(np_loops * 10u / secs10),
		(unsigned long)np_slept);

	/* One line per phase would be sixteen lines down a shared
	 * console every five seconds, and the kernel's output ring
	 * drops what will not fit -- the first run of this lost every
	 * sub-phase. Four phases per line, and only the ones that ran. */
	{
		int on_line = 0;
		for (int i = 0; i < NP_N; i++) {
			if (!np_cnt[i])
				continue;
			/* permille of one CPU, to keep this in 32-bit
			 * integers: cycles per second against the clock */
			uint32_t cps = np_cyc[i] / (secs10 ? secs10 : 1) * 10u;
			uint32_t permille = cps / (NP_CPU_HZ / 1000u);
			if (!on_line)
				printf("np: ");
			printf(" %s=%lu.%lu%%/%lu/%lu", np_name[i],
				(unsigned long)(permille / 10),
				(unsigned long)(permille % 10),
				(unsigned long)np_cnt[i],
				(unsigned long)(np_min[i] == 0xffffffffu
					? 0 : np_min[i]));
			if (++on_line == 4) {
				printf("\n");
				on_line = 0;
			}
		}
		if (on_line)
			printf("\n");
	}

	if (np_uart_bytes)
		printf("np:  uart1  %lu B  %lu chunks (%lu B/chunk)  spin=%lu\n",
			(unsigned long)np_uart_bytes,
			(unsigned long)np_uart_chunks,
			(unsigned long)(np_uart_bytes / (np_uart_chunks ? np_uart_chunks : 1)),
			(unsigned long)np_uart_spin);
	np_uart_bytes = np_uart_chunks = np_uart_spin = 0;

	memset(np_cyc, 0, sizeof(np_cyc));
	memset(np_cnt, 0, sizeof(np_cnt));
	for (int i = 0; i < NP_N; i++)
		np_min[i] = 0xffffffffu;
	np_loops = 0;
	np_slept = 0;
	np_last_tick = now;
}

#endif
