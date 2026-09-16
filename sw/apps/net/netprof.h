/*
 * Zeitlos
 *
 * Where net's cycles go, compiled out unless NET_PROFILE=1.
 *
 * net is the process that shows up in `ps` at 30-60% whenever a
 * browser is watching the desktop, and `ps` cannot say which part of
 * the loop that is. These counters split one turn of main() into its
 * phases and print a line every few seconds.
 *
 * rdcycle is WALL CLOCK: a phase that gets preempted charges the
 * other process's timeslice to itself. So each phase keeps both a sum
 * (what it costs the system, preemption included) and a MINIMUM (what
 * it costs when it runs uninterrupted). The two together say whether
 * a phase is expensive or merely unlucky.
 */

#ifndef NETPROF_H
#define NETPROF_H

#ifndef NET_PROFILE
#define NET_PROFILE 0
#endif

#if NET_PROFILE

#include <stdint.h>

enum {
	NP_WIFI, NP_ETH, NP_IP, NP_MSG, NP_TFTP, NP_TCP, NP_TELNET,
	NP_SOCK, NP_SSH, NP_DNS, NP_NTP, NP_SCREEN,
	NP_SNAP, NP_PACK, NP_SEND,	/* inside NP_SCREEN, see screen.c */
	NP_N
};

extern const char *np_name[NP_N];
extern uint32_t np_cyc[NP_N], np_cnt[NP_N], np_min[NP_N];
extern uint32_t np_loops, np_slept;

/* uart1 (the ESP32 link), to answer one question directly: does the
 * 16550 in this bitstream actually give us the 16-byte TX FIFO the
 * driver assumes? chunks == bytes means it does not, and every byte
 * waits for THRE on its own. */
extern uint32_t np_uart_bytes, np_uart_chunks, np_uart_spin;

static inline uint32_t np_rdcycle(void)
{
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

#define NP_PHASE(i, stmt) do { \
		uint32_t _t0 = np_rdcycle(); \
		stmt; \
		uint32_t _d = np_rdcycle() - _t0; \
		np_cyc[i] += _d; np_cnt[i]++; \
		if (_d < np_min[i]) np_min[i] = _d; \
	} while (0)

#define NP_LOOP()  do { np_loops++; } while (0)
#define NP_SLEPT() do { np_slept++; } while (0)
#define NP_T0(v) uint32_t v = np_rdcycle()
#define NP_ACC(i, v) do { \
		uint32_t _d = np_rdcycle() - (v); \
		np_cyc[i] += _d; np_cnt[i]++; \
		if (_d < np_min[i]) np_min[i] = _d; \
	} while (0)

#define NP_UART(bytes, chunks, spin) do { \
		np_uart_bytes += (bytes); np_uart_chunks += (chunks); \
		np_uart_spin += (spin); \
	} while (0)

void np_report(void);		/* prints at most once every NP_REPORT_TICKS */

#else	/* !NET_PROFILE */

#define NP_PHASE(i, stmt) do { stmt; } while (0)
#define NP_LOOP()  do {} while (0)
#define NP_SLEPT() do {} while (0)
#define NP_T0(v) do {} while (0)
#define NP_ACC(i, v) do {} while (0)
#define np_report() do {} while (0)
#define NP_UART(bytes, chunks, spin) do {} while (0)

#endif
#endif
