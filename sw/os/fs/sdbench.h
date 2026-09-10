#ifndef Z_SDBENCH_H
#define Z_SDBENCH_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SD card instrumentation and a layered throughput benchmark.
 *
 * Reachable as the `sdbench` shell command. See docs/sdcard.md for
 * what each layer measures, how to read the result, and the ranked
 * list of things it was built to distinguish between.
 *
 * Why a KERNEL command rather than an app: FatFs is non-reentrant and
 * the SPI transaction underneath it is not interruptible by another
 * caller (docs/filesystem.md). An app measuring the card would have to
 * go through the syscall layer, which is one of the things being
 * measured -- and the raw layer, spi_xchg() with CS deasserted, is not
 * reachable from an app at all without racing whatever FatFs is doing.
 */

#include <stdint.h>

/*
 * Whether sdmm.c's two wait loops poll tightly (1) or keep upstream's
 * 100us sleep between polls (0). Lives here rather than in sdmm.c so
 * that the benchmark can report which build it is measuring -- a
 * result recorded without that is a result nobody can reproduce.
 *
 * See sdmm.c's own comment above wait_ready() for why the default
 * changed, and docs/sdcard.md for the measurement it came from.
 */
#ifndef SD_POLL_TIGHT
#define SD_POLL_TIGHT 1
#endif

/*
 * Counters maintained by fs/fatfs/sdmm.c.
 *
 * Per POLL and per SECTOR, never per byte. A per-byte counter would be
 * an add and a store inside rcvr_mmc()'s inner loop -- the exact loop
 * whose cost is the question -- so it would change the answer it was
 * asked to report.
 *
 * `ready_polls` and `token_polls` are the interesting pair. Each is one
 * byte clocked at the current divider, so multiplying by the measured
 * cycles-per-byte from layer 0 turns them into a cycle count that can
 * be subtracted from the total. If most of a read's time is polls, the
 * card is slow to answer; if most of it is neither polls nor payload,
 * the time is going somewhere in software.
 */
typedef struct {
	uint32_t	commands;			/* send_cmd() calls, ACMD counts as two */
	uint32_t	sectors_read;		/* completed rcvr_datablock()s */
	uint32_t	sectors_written;	/* accepted xmit_datablock()s */
	uint32_t	ready_polls;		/* bytes clocked inside wait_ready() */
	uint32_t	token_polls;		/* bytes clocked waiting for 0xFE */
	uint32_t	ready_timeouts;		/* wait_ready() gave up */
	uint32_t	token_timeouts;		/* no data token arrived */
} sd_stat_t;

extern sd_stat_t sd_stat;

/*
 * Hooks into sdmm.c's statics. Only sdbench.c should call these --
 * sd_bench_set_div() in particular will happily clock a card faster
 * than it can go, which is the point (see docs/sdcard.md, "Trying
 * DIV=0") but is not something normal code should be doing.
 */
void sd_bench_xchg(uint32_t n);		/* clock n bytes, CS deasserted */
void sd_bench_xchg32(uint32_t n);	/* clock n bytes, 32 bits at a time */
int sd_bench_is_v1(void);			/* gateware is "SPI1" (rtl/spim.v) */
uint8_t sd_bench_get_div(void);
void sd_bench_set_div(uint8_t div);

/*
 * `sdbench [file]`. With no argument, layers 0 and 1 only (no
 * filesystem access at all, so it works on an unformatted card).
 * With a filename, also layer 2 -- a real f_read() of that file.
 */
void sh_sdbench(const char *path);

#endif
