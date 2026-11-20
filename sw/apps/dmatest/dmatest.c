/*
 * dmatest -- a file written and read back through `fs`, compared, and
 * timed (docs/dma.md, "Testing"). With the DMA controller the sector
 * payloads go card <-> buffer by DMA (sw/apps/fs/fatfs/sdmm.c); this
 * says whether what comes back is what went out, at sizes and
 * alignments that take every path: whole sectors, odd lengths, and
 * buffers that start off a word boundary.
 *
 *   run dmatest            /dmatest.bin, 256 KB
 *   run dmatest PATH KB
 *   run dmatest dma off    fs stops using DMA for the card (dma on: again),
 *                          to compare the two on one build
 *   run dmatest crc PATH   a file's CRC-32 and size, read in chunks of
 *                          odd sizes: to compare with the host's
 *
 * Results on UART0 (stdout), one line a pass and a summary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zsoc.h"
#include "../../common/zcycles.h"
#include "../../common/zfsc.h"

/* where a phase's cycles went, from fs's counters (Z_FSOP_STATS):
 * the client's total, fs's own share of it, the card's share of that,
 * and in the card's, waiting for it against moving the bytes */
static z_fs_stats_t s0;
static int have_stats;

static void stats_begin(void) { have_stats = zfs_stats(&s0) == 0; }

static void per_byte(const char *what, uint32_t cyc, uint32_t n) {
	printf("  %-30s %10lu cycles  %4lu a byte\n", what, (unsigned long)cyc, (unsigned long)(n ? cyc / n : 0));
}

static void stats_end(int reading, uint32_t client, uint32_t n) {
	z_fs_stats_t s1;
	uint32_t ops, opc, dc, dn, sec;
	if (!have_stats || zfs_stats(&s1)) { printf("  (fs has no timing counters: an older fs)\n"); return; }
	ops = reading ? s1.reads - s0.reads : s1.writes - s0.writes;
	opc = reading ? s1.read_cyc - s0.read_cyc : s1.write_cyc - s0.write_cyc;
	dc = reading ? s1.disk_read_cyc - s0.disk_read_cyc : s1.disk_write_cyc - s0.disk_write_cyc;
	dn = reading ? s1.disk_reads - s0.disk_reads : s1.disk_writes - s0.disk_writes;
	sec = reading ? s1.sectors_read - s0.sectors_read : s1.sectors_written - s0.sectors_written;
	per_byte("all of it (here)", client, n);
	per_byte("calls to fs, and back", client - opc, n);
	per_byte("in fs", opc, n);
	per_byte("  of which FatFs and copies", opc - dc, n);
	per_byte("  of which the card", dc, n);
	if (reading) {
		per_byte("    waiting for its 0xFE", s1.token_cyc - s0.token_cyc, n);
		per_byte("    the bytes moving", s1.payload_cyc - s0.payload_cyc, n);
	}
	printf("  %lu calls to fs, %lu card %s (%lu sectors, %lu.%lu a call), %lu commands, %lu bytes by DMA%s\n",
		(unsigned long)ops, (unsigned long)dn, reading ? "reads" : "writes", (unsigned long)sec,
		(unsigned long)(dn ? sec / dn : 0), (unsigned long)(dn ? sec * 10 / dn % 10 : 0), (unsigned long)(s1.commands - s0.commands),
		(unsigned long)(s1.dma_bytes - s0.dma_bytes),
		reading ? (s1.grant_reads != s0.grant_reads ? ", straight into this buffer" : ", by copy") : "");
}

static uint8_t pat(uint32_t i) { return (uint8_t)((i * 2654435761u) >> 24) ^ (uint8_t)i; }

/* crc: the file read through read() (no stdio: each call one request to
 * fs), in sizes that start and end mid-sector, mid-cluster and across
 * clusters, and its CRC-32 -- the zlib one -- to compare with the
 * host's copy (python3 -c 'import zlib,sys;
 * print("%08x" % zlib.crc32(open(sys.argv[1],"rb").read()))' FILE) */
static int crc_file(const char *path, uint8_t *buf) {
	static const uint32_t sizes[] = { 777, 16384, 512, 4096 + 515, 1, 12000, 8192 };
	uint32_t crc = 0xffffffffu, total = 0, k, t0, cyc = 0, calls = 0;
	int fd = open(path, O_RDONLY), i = 0, j;
	if (fd < 0) { printf("dmatest: cannot open %s\n", path); return 1; }
	for (;;) {
		int r;
		t0 = z_cycles();
		r = (int)read(fd, buf, sizes[i]);
		cyc += z_cycles() - t0;
		calls++;
		if (r < 0) { printf("dmatest: read failed at %lu\n", (unsigned long)total); close(fd); return 1; }
		if (r == 0) break;
		for (k = 0; k < (uint32_t)r; k++) {
			crc ^= buf[k];
			for (j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
		}
		total += (uint32_t)r;
		i = (i + 1) % (int)(sizeof sizes / sizeof sizes[0]);
	}
	close(fd);
	printf("dmatest: %s: %lu bytes, crc32 %08lx, %lu calls, %lu cycles a byte reading\n", path,
		(unsigned long)total, (unsigned long)(crc ^ 0xffffffffu), (unsigned long)calls,
		(unsigned long)(total ? cyc / total : 0));
	return 0;
}

/* what followed the name in `run dmatest ...`: wm holds it
 * (zwin.c's z_launch_arg_take(), without the rest of zwin.c) */
static char arg[160];
static void take_arg(void) {
	uint32_t wm;
	z_obj_t none;
	z_msg_t m;
	uint32_t i, waited = 0;
	if (!z_pid_lookup("wm0", &wm)) return;
	none.type = Z_NONE;
	z_msg_new_send(wm, Z_WM_GET_ARG, 0, none);
	for (;;) {
		if (z_msg_read(&m) != Z_OK) {
			if (++waited > 2 * Z_TICK_HZ) return;
			z_proc_wait(1);
			continue;
		}
		if (m.subject == Z_WM_ARG && m.tag == 0) break;
	}
	if (m.obj.type != Z_STR || !m.obj.val.str) return;
	for (i = 0; i < sizeof arg - 1 && m.obj.val.str[i]; i++) arg[i] = m.obj.val.str[i];
	arg[i] = 0;
}

int main(void) {
	const char *path = "/dmatest.bin";
	uint32_t kb = 256, n, i, bad = 0, chunk;
	char *sp;
	uint8_t *buf = malloc(16384 + 8);
	take_arg();
	if (!strcmp(arg, "dma off") || !strcmp(arg, "dma on")) {
		z_fs_stats_t st;
		int on = arg[5] == 'n';
		if (zfs_stats_dma(&st, on)) { printf("dmatest: this fs cannot switch its DMA\n"); return 1; }
		printf("dmatest: the card's DMA is %s in fs until it restarts (%s)\n", on ? "allowed" : "off",
			z_dma_channels() ? "this build has it" : "this build has none");
		return 0;
	}
	if (!strncmp(arg, "crc ", 4)) {
		if (!buf) return 1;
		return crc_file(arg + 4, buf);
	}
	if (arg[0]) {
		path = arg;
		sp = strchr(arg, ' ');
		if (sp) { *sp = 0; kb = (uint32_t)atoi(sp + 1); }
	}
	n = kb * 1024;
	uint32_t t0, tw, tr;
	FILE *f;
	if (!buf || !n) { printf("dmatest: no memory\n"); return 1; }
	printf("dmatest: %s, %lu KB, DMA %s (%lu channels)\n", path, (unsigned long)kb,
		z_dma_channels() ? "on" : "off", (unsigned long)z_dma_channels());

	// write: chunks of varying size from a buffer that is sometimes misaligned
	f = fopen(path, "wb");
	if (!f) { printf("dmatest: cannot write %s\n", path); return 1; }
	stats_begin();
	tw = 0;
	for (i = 0, chunk = 0; i < n; chunk++) {
		uint32_t len = (chunk & 3) == 0 ? 16384 : (chunk & 3) == 1 ? 4096 + 13 : (chunk & 3) == 2 ? 512 : 1000;
		uint8_t *p = buf + (chunk & 4 ? 1 : 0);
		uint32_t k;
		if (len > n - i) len = n - i;
		for (k = 0; k < len; k++) p[k] = pat(i + k);
		t0 = z_cycles();
		if (fwrite(p, 1, len, f) != len) { printf("dmatest: write failed at %lu\n", (unsigned long)i); return 1; }
		tw += z_cycles() - t0;
		i += len;
	}
	t0 = z_cycles();
	fclose(f);
	tw += z_cycles() - t0;
	printf("dmatest: writing, where the time went:\n");
	stats_end(0, tw, n);

	// read it back, in other sizes
	f = fopen(path, "rb");
	if (!f) { printf("dmatest: cannot read %s\n", path); return 1; }
	stats_begin();
	tr = 0;
	for (i = 0, chunk = 0; i < n; chunk++) {
		uint32_t len = (chunk & 1) ? 8192 : 4096 + 515;
		uint8_t *p = buf + ((chunk >> 1) & 3);
		uint32_t k, got;
		if (len > n - i) len = n - i;
		t0 = z_cycles();
		got = (uint32_t)fread(p, 1, len, f);
		tr += z_cycles() - t0;
		if (got != len) { printf("dmatest: short read at %lu\n", (unsigned long)i); return 1; }
		for (k = 0; k < len; k++)
			if (p[k] != pat(i + k)) {
				if (bad < 8) printf("dmatest: byte %lu is %02x, wrote %02x\n",
					(unsigned long)(i + k), p[k], pat(i + k));
				bad++;
			}
		i += len;
	}
	fclose(f);
	printf("dmatest: reading, where the time went:\n");
	stats_end(1, tr, n);

	printf("dmatest: write %lu cycles (%lu a byte), read %lu cycles (%lu a byte), in fwrite/fread; %s\n",
		(unsigned long)tw, (unsigned long)(tw / n), (unsigned long)tr, (unsigned long)(tr / n),
		bad ? "MISMATCH" : "all bytes match");
	return bad ? 1 : 0;
}
