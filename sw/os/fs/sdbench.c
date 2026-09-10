/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * `sdbench` -- a layered SD card throughput benchmark.
 *
 * WHY THIS EXISTS
 *
 * docs/ramdisk.md records sw/apps/web re-reading a 258KB page off the
 * card in 13.3 seconds -- about 19 KB/s, or roughly 26ms per 512-byte
 * sector. Nothing in the driver accounts for that. At DIV=1 the
 * gateware clocks SCLK at 12MHz, so 512 bytes is 384us of wire time,
 * and rtl/spim.v's own header puts the software cost at about 48 CPU
 * cycles per byte -- 1us at 48MHz. Those two numbers say a sector
 * should cost well under a millisecond, not twenty-six.
 *
 * So somewhere between the shift register and the application there is
 * a factor of thirty or more, and the honest answer is that nobody
 * knows where. That is what this measures. Four layers, each adding
 * exactly one thing to the one below it, so the ratio between adjacent
 * layers says which one is responsible:
 *
 *   layer 0  spi_xchg() with CS deasserted        bus + gateware only
 *   layer 1  sd_disk_read(), one sector per call  + card, CMD17
 *   layer 2  sd_disk_read(), N sectors per call   + CMD18 streaming
 *   layer 3  f_read() of a real file              + FatFs
 *
 * A read through the app-facing syscalls would be a fifth layer, but
 * it cannot be measured from here -- see docs/sdcard.md for how to do
 * that half, and why the split falls where it does.
 *
 * READ IT WITH NOTHING ELSE RUNNING. Same caveat sh_bench() carries:
 * rdcycle counts WALL cycles, so every figure is inflated by whatever
 * share of the CPU other processes took while this one was measuring.
 * With wm, net and repl up that is most of the machine. `ps` first,
 * `kill` them, then measure -- or measure before `init`.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zeitlos.h"
#include "zsoc.h"
#include "../kernel.h"
#include "fs.h"
#include "sdbench.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

/* One call's worth of sectors for the multi-block layer, and the read
 * buffer for the FatFs layer.
 *
 * Static, and 8KB rather than something more generous: this is kernel
 * .bss, which comes straight out of the kernel's own process image, and
 * k_proc_create() sizes that image from the binary. Every byte here is
 * a byte off the memory budget in docs/boot.md for the entire life of
 * the system, in exchange for a diagnostic that runs for two seconds
 * when somebody types a command. 16 sectors is already well past the
 * point where per-call overhead stops dominating. */
// 4 sectors, not 16.
//
// This is kernel .bss, and kernel .bss is FLASH IMAGE (see
// Z_PROCS_MAX's comment in kernel.h). 8KB of diagnostic buffer is
// 8KB of the 256KB the BIOS copies, held for the whole life of the
// system so that a command somebody types occasionally has somewhere
// to read into.
//
// Four sectors is still well past the point where per-call overhead
// stops dominating -- the measured difference between 4 and 16
// sectors per CMD18 is small next to the difference between one and
// four (docs/sdcard.md).
#define SDB_SECTORS		4
#define SDB_BUFSZ		(SDB_SECTORS * 512)

static uint8_t sdb_buf[SDB_BUFSZ];

/* Total bytes each layer moves. Enough that a millisecond of noise
 * does not show, small enough that the whole command finishes while
 * somebody is still looking at it. */
#define SDB_TOTAL		(256u * 1024u)

static inline uint32_t sdb_cycles(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

/* KB/s from a byte count and a cycle count.
 *
 * 64-bit intermediates because the obvious 32-bit forms both overflow
 * at figures this benchmark produces routinely: bytes * 1000000 blows
 * up past 4KB, and cycles * 100 past about 42M, which is under a
 * second of wall time. */
static uint32_t sdb_kbs(uint32_t bytes, uint32_t cycles) {
	if (!cycles) return 0;
	return (uint32_t)(((uint64_t)bytes * Z_SYSCLK_HZ) / ((uint64_t)cycles * 1024u));
}

/* Cycles per byte, x100, so one decimal prints without floating point
 * (there is none in kernel code -- see sh_bench()'s own note). */
static uint32_t sdb_cpb100(uint32_t bytes, uint32_t cycles) {
	if (!bytes) return 0;
	return (uint32_t)(((uint64_t)cycles * 100u) / bytes);
}

/* Prints the three figures only; the caller has already printed a
 * label. Split that way because one caller's label is a computed
 * DIV/clock pair, and building it into a string would mean
 * snprintf() -- which hangs in kernel-compiled code on this hardware.
 * See the comment above append_decimal() in sw/os/pidreg.c; printf()
 * straight to the UART is fine, formatting INTO a buffer is not. */
static void sdb_figures(uint32_t bytes, uint32_t cycles) {

	uint32_t cpb = sdb_cpb100(bytes, cycles);
	uint32_t ms  = cycles / (Z_SYSCLK_HZ / 1000u);

	printf("%6lu KB/s  %4lu.%02lu cyc/byte  %5lu ms\n",
		(unsigned long)sdb_kbs(bytes, cycles),
		(unsigned long)(cpb / 100), (unsigned long)(cpb % 100),
		(unsigned long)ms);

}

static void sdb_report(const char *name, uint32_t bytes, uint32_t cycles) {

	printf(" %-22s ", name);
	sdb_figures(bytes, cycles);

}

static void sdb_stat_reset(void) {
	memset(&sd_stat, 0, sizeof(sd_stat));
}

static void sdb_stat_report(void) {

	printf("   cmds %lu  sect %lu  ready-polls %lu  token-polls %lu",
		(unsigned long)sd_stat.commands,
		(unsigned long)sd_stat.sectors_read,
		(unsigned long)sd_stat.ready_polls,
		(unsigned long)sd_stat.token_polls);

	if (sd_stat.ready_timeouts || sd_stat.token_timeouts)
		printf("  TIMEOUTS r=%lu t=%lu",
			(unsigned long)sd_stat.ready_timeouts,
			(unsigned long)sd_stat.token_timeouts);

	printf("\n");

}

/* -- layer 0: the bus and the shift register, nothing else --
 *
 * CS stays deasserted, so the card sees clock but no command and
 * cannot be the thing being measured. What is left is: one wishbone
 * write to DATA, however many reads of STATUS the busy-poll takes, and
 * one read of DATA, per byte.
 *
 * This is the number to compare against the divider. At DIV=1, SCLK is
 * 12MHz and eight bits is 32 CPU cycles at 48MHz. If this layer
 * reports meaningfully more than that, the CPU's bus access is the
 * limit and raising the SPI clock will not help -- which is the first
 * thing worth knowing, because "run it at 24MHz" is otherwise the
 * obvious move and would be wasted effort. */
static void sdb_layer0(void) {

	uint8_t saved = sd_bench_get_div();
	uint8_t divs[3];
	int n = 0, i;

	divs[n++] = saved;
	if (saved != 1) divs[n++] = 1;
	if (saved != 0) divs[n++] = 0;

	printf("layer 0: raw byte exchange, CS deasserted (bus + gateware only)\n");

	for (i = 0; i < n; i++) {

		uint32_t t0, cyc;
		uint32_t sclk_khz = (Z_SYSCLK_HZ / 1000u) / (2u * ((uint32_t)divs[i] + 1u));

		/* One k_fs_enter() for the whole burst rather than per byte:
		   this must not interleave with another caller's transaction
		   (docs/filesystem.md), and 64KB at DIV=1 is about 55ms, which
		   is inside K_NO_PREEMPT_MAX_TICKS' 87ms cap. Do not raise the
		   burst size without checking that again. */
		k_fs_enter();
		sd_bench_set_div(divs[i]);
		t0 = sdb_cycles();
		sd_bench_xchg(65536u);
		cyc = sdb_cycles() - t0;
		sd_bench_set_div(saved);
		k_fs_leave();

		printf(" 8-bit  DIV=%-2u %5luk    ",
			(unsigned)divs[i], (unsigned long)sclk_khz);
		sdb_figures(65536u, cyc);

		/* And the same again through the WIDE path, which is what
		 * layers 1-3 actually use.
		 *
		 * Without it, layer 0 reads as a floor that the layers above
		 * it somehow beat -- 285 KB/s for a raw exchange under a 848
		 * KB/s multi-block read. Those are not the same measurement
		 * taken twice; they are two different paths, and reporting
		 * only the narrow one invites exactly the wrong conclusion. */
		if (sd_bench_is_v1()) {
			k_fs_enter();
			sd_bench_set_div(divs[i]);
			t0 = sdb_cycles();
			sd_bench_xchg32(65536u);
			cyc = sdb_cycles() - t0;
			sd_bench_set_div(saved);
			k_fs_leave();

			printf(" 32-bit DIV=%-2u %5luk    ",
				(unsigned)divs[i], (unsigned long)sclk_khz);
			sdb_figures(65536u, cyc);
		}

	}

	printf("   theoretical at DIV=%u: %lu cyc/byte of wire time\n",
		(unsigned)saved,
		(unsigned long)(8u * 2u * ((uint32_t)saved + 1u)));

}

/* -- layers 1 and 2: the card, via the block driver, FatFs bypassed --
 *
 * Reads sector 0 upward, which on any card is the MBR and the start of
 * the reserved region: read-only traffic that cannot damage anything
 * even on a card with no filesystem at all. That is deliberate -- this
 * layer needs to work on an unformatted card, because "the card is
 * unreadable" is one of the states somebody would run this in.
 *
 * Layer 1 asks for one sector per call, so every sector pays a full
 * CMD17: deselect, select, wait_ready, six command bytes, response
 * poll, token poll, 512 bytes, CRC, deselect. Layer 2 asks for
 * SDB_SECTORS at a time, so one CMD18 covers all of them and only the
 * token poll repeats. The ratio between the two is the per-command
 * overhead, and it is the number that decides whether a bigger chunk
 * size in fsapi.c is worth anything. */
static void sdb_layer12(void) {

	uint32_t t0, cyc, i;
	uint32_t n_single = SDB_TOTAL / 512u;
	uint32_t n_multi  = SDB_TOTAL / SDB_BUFSZ;

	printf("layer 1: sd_disk_read(), 1 sector per call (CMD17)\n");

	sdb_stat_reset();
	t0 = sdb_cycles();
	for (i = 0; i < n_single; i++) {
		k_fs_enter();
		if (disk_read(0, sdb_buf, i, 1) != RES_OK) {
			k_fs_leave();
			printf("   read failed at sector %lu\n", (unsigned long)i);
			return;
		}
		k_fs_leave();
	}
	cyc = sdb_cycles() - t0;
	sdb_report("single-block", n_single * 512u, cyc);
	sdb_stat_report();

	printf("layer 2: sd_disk_read(), %u sectors per call (CMD18)\n", SDB_SECTORS);

	sdb_stat_reset();
	t0 = sdb_cycles();
	for (i = 0; i < n_multi; i++) {
		k_fs_enter();
		if (disk_read(0, sdb_buf, i * SDB_SECTORS, SDB_SECTORS) != RES_OK) {
			k_fs_leave();
			printf("   read failed at sector %lu\n",
				(unsigned long)(i * SDB_SECTORS));
			return;
		}
		k_fs_leave();
	}
	cyc = sdb_cycles() - t0;
	sdb_report("multi-block", n_multi * SDB_BUFSZ, cyc);
	sdb_stat_report();

}

/* -- layer 3: the same bytes through FatFs --
 *
 * Takes a real file rather than raw sectors, because what FatFs adds
 * over the block driver is FAT chain walking and its window buffer,
 * and neither happens without a file to walk.
 *
 * Reads in SDB_BUFSZ chunks at aligned offsets, which is the case
 * FatFs handles best: it recognises a whole-sector-aligned request and
 * reads straight into the caller's buffer rather than through the
 * per-file 512-byte window. If layer 3 is much worse than layer 2 with
 * a chunk this size, the cost is FatFs's own bookkeeping and not the
 * copy.
 *
 * `/ram` works here too and is worth running for contrast --
 * `sdbench /ram/whatever` measures the same code path with the card
 * taken out of it, which puts a floor under how fast layer 3 could
 * ever be. */
static void sdb_layer3(const char *path) {

	char rp[64];
	FIL f;
	FRESULT res;
	UINT br;
	uint32_t t0, cyc, total = 0;

	const char *rpath = fs_path_resolve(path, rp, sizeof(rp));

	printf("layer 3: f_read() of '%s', %u byte chunks\n", path, SDB_BUFSZ);

	k_fs_enter();
	res = f_open(&f, rpath, FA_READ);
	k_fs_leave();

	if (res != FR_OK) {
		printf("   f_open failed, FRESULT %d\n", (int)res);
		return;
	}

	sdb_stat_reset();
	t0 = sdb_cycles();

	do {
		k_fs_enter();
		res = f_read(&f, sdb_buf, SDB_BUFSZ, &br);
		k_fs_leave();
		if (res != FR_OK) break;
		total += br;
	} while (br == SDB_BUFSZ && total < SDB_TOTAL);

	cyc = sdb_cycles() - t0;

	k_fs_enter();
	f_close(&f);
	k_fs_leave();

	if (res != FR_OK) {
		printf("   f_read failed, FRESULT %d\n", (int)res);
		return;
	}

	if (total < SDB_BUFSZ) {
		printf("   file is only %lu bytes -- too small to time\n",
			(unsigned long)total);
		return;
	}

	sdb_report("f_read", total, cyc);
	sdb_stat_report();

}

void sh_sdbench(const char *path) {

	printf("sdbench -- see docs/sdcard.md\n");
	printf("kill wm/net/repl first or every figure is inflated (see ps)\n");
	printf("sysclk %lu Hz, SD_POLL_TIGHT=%d\n\n",
		(unsigned long)Z_SYSCLK_HZ, (int)SD_POLL_TIGHT);

	if (disk_status(0) & STA_NOINIT) {
		/* Deferred mount: the card is not initialised until something
		   actually touches it (docs/filesystem.md). Do that here rather
		   than reporting a card fault that is really just laziness. */
		printf("card not initialised, bringing it up ...\n");
		if (disk_initialize(0) & STA_NOINIT) {
			printf("disk_initialize failed -- no card?\n");
			return;
		}
	}

	sdb_layer0();
	printf("\n");
	sdb_layer12();

	if (path && *path) {
		printf("\n");
		sdb_layer3(path);
	} else {
		printf("\npass a filename for layer 3 (FatFs), e.g. sdbench wm\n");
	}

}
