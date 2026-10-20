/*
 * The board's file calls and clock, on the build machine, for draw's
 * host tests: stdio, FAT's rename (it refuses to replace a file), a
 * clock that moves one tick a call, and faults to order.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "hostfs.h"

#include "../../../common/zfs.h"	// Z_FS_ERR_*

hostfs_faults_t hostfs_fault;

static FILE *hf[8];
static int writes, reads;

void hostfs_reset(void) {
	memset(&hostfs_fault, 0, sizeof(hostfs_fault));
	writes = reads = 0;
}

static int slot(const char *p, const char *mode) {
	int i;
	for (i = 0; i < 8; i++)
		if (!hf[i]) { hf[i] = fopen(p, mode); return hf[i] ? i : -1; }
	return -1;
}

int fs_open_read(const char *p) { return slot(p, "rb"); }

int fs_open_write(const char *p) {
	if (hostfs_fault.open_write) return -1;
	return slot(p, "wb");
}

int fs_read_chunk(int h, void *b, int n) {
	if (h < 0 || h >= 8 || !hf[h]) return -1;
	if (hostfs_fault.read_at && ++reads == hostfs_fault.read_at) return -1;
	return (int)fread(b, 1, (size_t)n, hf[h]);
}

int fs_write_chunk(int h, const void *b, int n) {
	if (h < 0 || h >= 8 || !hf[h]) return -1;
	if (hostfs_fault.write_at && ++writes == hostfs_fault.write_at)
		return hostfs_fault.write_short ? n / 2 : -1;
	return (int)fwrite(b, 1, (size_t)n, hf[h]);
}

int fs_seek(int h, uint32_t o) {
	if (h < 0 || h >= 8 || !hf[h]) return 0;
	return fseek(hf[h], (long)o, SEEK_SET) == 0;
}

int fs_close_handle(int h) {
	if (h < 0 || h >= 8 || !hf[h]) return 0;
	fclose(hf[h]);
	hf[h] = NULL;
	return !hostfs_fault.close;
}

int fs_size(char *p) {
	FILE *f = fopen(p, "rb");
	long n;
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (int)n;
}

int fs_unlink(char *p) {
	if (hostfs_fault.unlink) return 0;
	return unlink(p) == 0;
}

int fs_rename(const char *from, const char *to, int *err) {
	int e = 0;
	if (access(from, F_OK) != 0) e = Z_FS_ERR_NOENT;
	else if (access(to, F_OK) == 0) e = Z_FS_ERR_EXIST;	// FAT's way
	else if (hostfs_fault.rename) e = Z_FS_ERR_IO;
	else if (rename(from, to) != 0) e = Z_FS_ERR_IO;
	if (err) *err = e;
	return e == 0;
}

int hostfs_open_count(void) {
	int i, n = 0;
	for (i = 0; i < 8; i++) if (hf[i]) n++;
	return n;
}

static uint32_t clock_now;
uint32_t z_uptime_ticks(void) { return clock_now++; }
