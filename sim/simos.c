/*
 * zeitlos-sim: simos.c -- see simos.h for what this is and is not.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <time.h>

#include "machine.h"
#include "simos.h"

/* Syscall ids, from the same X-macro source machine.c uses. Duplicated
 * as an include rather than shared through a header because the enum
 * has to be a complete type in both translation units and there is no
 * generated header to put it in -- sw/common/zeitlos.h builds it the
 * same way, from the same file, for the same reason. */
enum {
	ZSYS_NONE = 0,
#define Z_MKSYSCALL(name, handler) ZSYS_##name,
#include "../sw/common/syscalls.def"
#undef Z_MKSYSCALL
	ZSYS_MAX
};

/* z_obj_t (sw/common/zobj.h) is { int32 type; union val; } -- 8 bytes
 * on rv32, val at +4. Every FS_* argument struct is a flat run of
 * 4-byte fields, so each one is addressed here by field INDEX rather
 * than by a host struct: a host struct would be laid out by the host
 * compiler, and on x86-64 its pointers are 8 bytes. That is the whole
 * reason these are read field by field and not with a memcpy. */
#define F(n) (obj + 4u * (n))

/* Z_FS_MAX_OPEN, sw/common/zfs.h. Same number deliberately: an app
 * that runs out of handles should run out here at the same point. */
#define SIMOS_MAX_OPEN 8

/* Z_TICK_HZ, sw/common/zsoc.h -- the KTIMER rate, Z_SYSCLK_HZ / 65536,
 * which is 732 at 48MHz. Uptime is reported in these, because that is
 * what z_uptime_ticks() returns and what every Z_TICK_HZ/N idle wait in
 * sw/apps is written against. */
#define SIMOS_TICK_HZ 732u

#define SIMOS_PATH_MAX 1024

static char simos_root[SIMOS_PATH_MAX];
static int  simos_have_root;

static struct {
	FILE *f;
	int   used;
	int   writable;
} simos_open[SIMOS_MAX_OPEN];

static struct timespec simos_t0;
static int simos_t0_set;

void simos_set_root(const char *dir) {
	if (!dir) { simos_have_root = 0; simos_root[0] = 0; return; }
	snprintf(simos_root, sizeof(simos_root), "%s", dir);
	simos_have_root = 1;
}

const char *simos_get_root(void) {
	return simos_have_root ? simos_root : NULL;
}

/* ------------------------------------------------------------------- */
/* guest memory helpers                                                 */

/* Reads a NUL-terminated guest string into a host buffer.
 *
 * Bounded, and bounded for a real reason rather than as a reflex: the
 * pointer comes from inside the app, the app may be the thing being
 * debugged, and an unbounded read walks off the end of the RAM
 * allocation on the host -- turning an app bug into a simulator crash
 * with a stack trace pointing at the wrong program. */
static int guest_str(machine_t *m, uint32_t addr, char *out, size_t cap) {
	size_t i;
	if (!addr) return -1;
	for (i = 0; i + 1 < cap; i++) {
		uint8_t c = bus_read8(m, addr + (uint32_t)i);
		out[i] = (char)c;
		if (!c) return 0;
	}
	out[cap - 1] = 0;
	return -1;			/* did not terminate inside cap */
}

static void guest_write(machine_t *m, uint32_t addr, const void *src, size_t n) {
	const uint8_t *p = (const uint8_t *)src;
	size_t i;
	for (i = 0; i < n; i++) bus_write8(m, addr + (uint32_t)i, p[i]);
}

static void guest_read(machine_t *m, uint32_t addr, void *dst, size_t n) {
	uint8_t *p = (uint8_t *)dst;
	size_t i;
	for (i = 0; i < n; i++) p[i] = bus_read8(m, addr + (uint32_t)i);
}

/* ------------------------------------------------------------------- */
/* path resolution                                                      */

/*
 * Guest path -> host path, under the root and only under the root.
 *
 * Three things this has to get right, none of them obvious:
 *
 * 1. NO ESCAPING. Any ".." component is refused outright rather than
 *    normalised away. An app is not supposed to be able to reach the
 *    developer's home directory because it opened "../../../.ssh/id_rsa",
 *    and normalising is where that goes wrong quietly.
 *
 * 2. CASE. The card is FAT, which is case-insensitive, and sw/os/sh.c
 *    passes paths UPPERCASED -- so an app asking for "WM" on hardware
 *    finds a file called `wm`. A host filesystem is case-sensitive and
 *    would not. So an exact match is tried first, and a
 *    case-insensitive scan of the containing directory second. Without
 *    this, half the tree's own paths miss here and hit on hardware,
 *    which is the worst possible direction for a simulator to differ.
 *
 * 3. /ram. docs/ramdisk.md reserves it as a volume prefix. Here it is
 *    an ordinary subdirectory of the root, created on demand, and it
 *    is NOT volatile the way the real one is. Worth knowing before
 *    using the simulator to test something that depends on /ram being
 *    empty at startup -- the real one is reformatted at every boot.
 */
static int simos_hostpath(const char *guest, char *out, size_t cap) {

	const char *p = guest;
	char rel[SIMOS_PATH_MAX];
	size_t n = 0;

	if (!simos_have_root || !guest) return -1;

	/* FatFs drive syntax, which fs_path_resolve() passes through
	 * unchanged on hardware (docs/ramdisk.md): "1:/x" is the ramdisk.
	 * Map it to the same place /ram lands. */
	if (p[0] && p[1] == ':') {
		const char *vol = (p[0] == '1') ? "ram/" : "";
		n = (size_t)snprintf(rel, sizeof(rel), "%s", vol);
		p += 2;
	}

	while (*p == '/') p++;

	for (;;) {
		const char *seg = p;
		size_t len;
		while (*p && *p != '/') p++;
		len = (size_t)(p - seg);

		if (len == 2 && seg[0] == '.' && seg[1] == '.') return -1;

		if (len && !(len == 1 && seg[0] == '.')) {
			if (n + len + 2 >= sizeof(rel)) return -1;
			if (n) rel[n++] = '/';
			memcpy(rel + n, seg, len);
			n += len;
		}

		if (!*p) break;
		p++;
	}

	rel[n] = 0;

	if ((size_t)snprintf(out, cap, "%s/%s", simos_root, rel) >= cap) return -1;

	if (!n) return 0;						/* the root itself */
	if (access(out, F_OK) == 0) return 0;	/* exact match, done */

	/* Case-insensitive retry, one component at a time, rebuilding the
	 * path as it goes so that "APPS/WM" finds "apps/wm" -- a single
	 * scan of the last directory would not. */
	{
		char built[SIMOS_PATH_MAX];
		char *save, *tok;
		char work[SIMOS_PATH_MAX];

		snprintf(work, sizeof(work), "%s", rel);
		snprintf(built, sizeof(built), "%s", simos_root);

		for (tok = strtok_r(work, "/", &save); tok;
		     tok = strtok_r(NULL, "/", &save)) {

			char probe[SIMOS_PATH_MAX];
			DIR *d;
			struct dirent *de;
			int found = 0;

			if ((size_t)snprintf(probe, sizeof(probe), "%s/%s", built, tok)
				>= sizeof(probe)) return -1;

			if (access(probe, F_OK) == 0) {
				snprintf(built, sizeof(built), "%s", probe);
				continue;
			}

			d = opendir(built);
			if (!d) return -1;
			while ((de = readdir(d))) {
				if (strcasecmp(de->d_name, tok)) continue;
				if (strlen(built) + strlen(de->d_name) + 2 > sizeof(probe))
					break;
				strcpy(probe, built);
				strcat(probe, "/");
				strcat(probe, de->d_name);
				memcpy(built, probe, strlen(probe) + 1);
				found = 1;
				break;
			}
			closedir(d);

			/* Not found is not an error here: the caller may be
			 * CREATING this path. Fall back to the name as given and
			 * let open() decide. */
			if (!found) {
				snprintf(built, sizeof(built), "%s", probe);
			}
		}

		snprintf(out, cap, "%s", built);
	}

	return 0;
}

/* ------------------------------------------------------------------- */

static uint32_t simos_uptime_ticks(void) {

	struct timespec now;
	uint64_t us;

	clock_gettime(CLOCK_MONOTONIC, &now);

	if (!simos_t0_set) { simos_t0 = now; simos_t0_set = 1; }

	us = (uint64_t)(now.tv_sec - simos_t0.tv_sec) * 1000000u +
	     (uint64_t)(now.tv_nsec - simos_t0.tv_nsec) / 1000u;

	/* Host wall clock, not instruction count.
	 *
	 * The alternative -- ticks derived from cpu.insn_count -- would be
	 * reproducible, which is genuinely attractive for a test harness.
	 * It is rejected because the simulator runs at a completely
	 * different rate from the hardware, so an app that waits
	 * Z_TICK_HZ/30 between frames would either spin or crawl depending
	 * on the host, and neither resembles the machine. Wall time at
	 * least makes an animation look approximately right.
	 *
	 * The consequence is that runs are not bit-reproducible. If that
	 * ever matters more than plausible timing, this is the one
	 * function to change. */
	return (uint32_t)((us * SIMOS_TICK_HZ) / 1000000u);
}

static int handle_ok(int h) {
	return h >= 0 && h < SIMOS_MAX_OPEN && simos_open[h].used;
}

static int handle_alloc(FILE *f, int writable) {
	int i;
	for (i = 0; i < SIMOS_MAX_OPEN; i++) {
		if (!simos_open[i].used) {
			simos_open[i].used = 1;
			simos_open[i].f = f;
			simos_open[i].writable = writable;
			return i;
		}
	}
	fclose(f);
	return -1;
}

/* ------------------------------------------------------------------- */
/* filesystem syscalls                                                  */

static int sys_fs_size(machine_t *m, uint32_t obj) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];
	struct stat st;

	/* "File not found" is a SUCCESSFUL call with size 0 here, not a
	 * failure -- sw/os/fsapi.h is explicit about it, and te's README
	 * gives the reason: opening a file that does not exist is how you
	 * create a new one. Returning failure would break that. */
	bus_write32(m, F(1), 0);

	if (guest_str(m, bus_read32(m, F(0)), guest, sizeof(guest)) != 0) return 0;
	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 1;
	if (stat(host, &st) != 0 || !S_ISREG(st.st_mode)) return 1;

	bus_write32(m, F(1), (uint32_t)st.st_size);
	return 1;
}

static int sys_fs_read(machine_t *m, uint32_t obj) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];
	uint32_t buf = bus_read32(m, F(1));
	uint32_t maxlen = bus_read32(m, F(2));
	uint8_t *tmp;
	size_t got;
	FILE *f;

	bus_write32(m, F(3), 0);

	if (guest_str(m, bus_read32(m, F(0)), guest, sizeof(guest)) != 0) return 0;
	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 0;

	f = fopen(host, "rb");
	if (!f) return 0;

	tmp = malloc(maxlen ? maxlen : 1);
	if (!tmp) { fclose(f); return 0; }

	got = fread(tmp, 1, maxlen, f);

	/* A file larger than the caller's buffer is a FAILURE, not a short
	 * read -- k_fs_read()'s own contract. Silently truncating would
	 * hand the caller a partial file it has no way to detect. */
	if (!feof(f) && got == maxlen && fgetc(f) != EOF) {
		free(tmp); fclose(f);
		return 0;
	}
	fclose(f);

	guest_write(m, buf, tmp, got);
	free(tmp);

	bus_write32(m, F(3), (uint32_t)got);
	return 1;
}

static int sys_fs_write(machine_t *m, uint32_t obj) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];
	uint32_t buf = bus_read32(m, F(1));
	uint32_t len = bus_read32(m, F(2));
	uint8_t *tmp;
	size_t put;
	FILE *f;

	bus_write32(m, F(3), 0);

	if (guest_str(m, bus_read32(m, F(0)), guest, sizeof(guest)) != 0) return 0;
	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 0;

	tmp = malloc(len ? len : 1);
	if (!tmp) return 0;
	guest_read(m, buf, tmp, len);

	f = fopen(host, "wb");
	if (!f) { free(tmp); return 0; }
	put = fwrite(tmp, 1, len, f);
	fclose(f);
	free(tmp);

	bus_write32(m, F(3), (uint32_t)put);
	return put == len;
}

static int sys_fs_unlink(machine_t *m, uint32_t obj) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];

	if (guest_str(m, bus_read32(m, F(0)), guest, sizeof(guest)) != 0) return 0;
	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 0;

	if (remove(host) != 0) return 0;
	return 1;
}

static int sys_fs_mkdir(machine_t *m, uint32_t obj) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];

	if (guest_str(m, bus_read32(m, F(0)), guest, sizeof(guest)) != 0) return 0;
	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 0;

	return mkdir(host, 0777) == 0;
}

static int sys_fs_touch(machine_t *m, uint32_t obj) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];
	FILE *f;

	if (guest_str(m, bus_read32(m, F(0)), guest, sizeof(guest)) != 0) return 0;
	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 0;

	f = fopen(host, "ab");
	if (!f) return 0;
	fclose(f);
	return 1;
}

/*
 * FS_LIST. Entries are packed into the caller's buffer as
 * NUL-terminated strings back to back, each already a full
 * "/"-prefixed path ("/WM", not "WM" or "//WM") -- sw/common/zfs.h is
 * specific about that, and sw/apps/files depends on it, so it is worth
 * matching exactly rather than approximately.
 *
 * `types` is optional and may be NULL. Directories are reported
 * because a file browser has to draw them differently and, more to the
 * point, do something different when one is picked.
 */
static int sys_fs_list(machine_t *m, uint32_t obj) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];
	uint32_t out      = bus_read32(m, F(1));
	uint32_t out_cap  = bus_read32(m, F(2));
	uint32_t max_ent  = bus_read32(m, F(3));
	uint32_t types    = bus_read32(m, F(6));
	uint32_t used = 0, count = 0, truncated = 0;
	uint32_t path_addr = bus_read32(m, F(0));
	DIR *d;
	struct dirent *de;

	bus_write32(m, F(4), 0);
	bus_write32(m, F(5), 0);

	if (!path_addr) guest[0] = 0;
	else if (guest_str(m, path_addr, guest, sizeof(guest)) != 0) return 0;

	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 0;

	d = opendir(host);
	if (!d) return 0;

	while ((de = readdir(d))) {

		char entry[SIMOS_PATH_MAX + 2];
		char full[SIMOS_PATH_MAX * 2];
		struct stat st;
		size_t len;
		int isdir;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

		if (max_ent && count >= max_ent) { truncated = 1; break; }

		snprintf(entry, sizeof(entry), "/%s", de->d_name);
		len = strlen(entry) + 1;

		if (used + len > out_cap) { truncated = 1; break; }

		snprintf(full, sizeof(full), "%s/%s", host, de->d_name);
		isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));

		guest_write(m, out + used, entry, len);
		used += (uint32_t)len;

		if (types) bus_write8(m, types + count, (uint8_t)(isdir ? 1 : 0));

		count++;
	}

	closedir(d);

	bus_write32(m, F(4), count);
	bus_write32(m, F(5), truncated);
	return 1;
}

static int sys_fs_open(machine_t *m, uint32_t obj, const char *mode, int writable) {

	char guest[SIMOS_PATH_MAX], host[SIMOS_PATH_MAX];
	FILE *f;
	int h;

	bus_write32(m, F(1), 0xffffffffu);		/* handle = -1 */

	if (guest_str(m, bus_read32(m, F(0)), guest, sizeof(guest)) != 0) return 0;
	if (simos_hostpath(guest, host, sizeof(host)) != 0) return 0;

	f = fopen(host, mode);
	if (!f) return 0;

	h = handle_alloc(f, writable);
	if (h < 0) return 0;

	bus_write32(m, F(1), (uint32_t)h);
	return 1;
}

static int sys_fs_read_chunk(machine_t *m, uint32_t obj) {

	int h = (int)bus_read32(m, F(0));
	uint32_t buf = bus_read32(m, F(1));
	uint32_t maxlen = bus_read32(m, F(2));
	uint8_t *tmp;
	size_t got;

	bus_write32(m, F(3), 0);

	if (!handle_ok(h)) return 0;

	tmp = malloc(maxlen ? maxlen : 1);
	if (!tmp) return 0;

	got = fread(tmp, 1, maxlen, simos_open[h].f);
	guest_write(m, buf, tmp, got);
	free(tmp);

	bus_write32(m, F(3), (uint32_t)got);

	/* 0 bytes is clean EOF, not failure -- k_fs_read_chunk()'s own
	 * comment in sw/common/zfs.h draws that line, and the return value
	 * is what distinguishes them. */
	return 1;
}

static int sys_fs_write_chunk(machine_t *m, uint32_t obj) {

	int h = (int)bus_read32(m, F(0));
	uint32_t buf = bus_read32(m, F(1));
	uint32_t len = bus_read32(m, F(2));
	uint8_t *tmp;
	size_t put;

	bus_write32(m, F(3), 0);

	if (!handle_ok(h) || !simos_open[h].writable) return 0;

	tmp = malloc(len ? len : 1);
	if (!tmp) return 0;
	guest_read(m, buf, tmp, len);

	put = fwrite(tmp, 1, len, simos_open[h].f);
	free(tmp);

	bus_write32(m, F(3), (uint32_t)put);
	return put == len;
}

static int sys_fs_seek(machine_t *m, uint32_t obj) {

	int h = (int)bus_read32(m, F(0));
	uint32_t off = bus_read32(m, F(1));

	bus_write32(m, F(2), 0);

	if (!handle_ok(h)) return 0;
	if (fseek(simos_open[h].f, (long)off, SEEK_SET) != 0) return 0;

	bus_write32(m, F(2), (uint32_t)ftell(simos_open[h].f));
	return 1;
}

static int sys_fs_truncate(machine_t *m, uint32_t obj) {

	int h = (int)bus_read32(m, F(0));
	uint32_t size = bus_read32(m, F(1));

	bus_write32(m, F(2), 0);

	if (!handle_ok(h) || !simos_open[h].writable) return 0;

	fflush(simos_open[h].f);
	if (ftruncate(fileno(simos_open[h].f), (off_t)size) != 0) return 0;

	bus_write32(m, F(2), size);
	return 1;
}

static int sys_fs_close(machine_t *m, uint32_t obj) {

	int h = (int)bus_read32(m, F(0));

	if (!handle_ok(h)) return 0;

	fclose(simos_open[h].f);
	simos_open[h].used = 0;
	simos_open[h].f = NULL;
	return 1;
}

static int sys_fs_sync(machine_t *m, uint32_t obj) {

	int h = (int)bus_read32(m, F(0));

	if (!handle_ok(h)) return 0;
	fflush(simos_open[h].f);
	return 1;
}

static int sys_fs_df(machine_t *m, uint32_t obj) {

	struct statvfs vfs;

	bus_write32(m, F(0), 0);
	bus_write32(m, F(1), 0);

	if (!simos_have_root || statvfs(simos_root, &vfs) != 0) return 0;

	/* KILOBYTES, not bytes -- z_fs_df_args_t is explicit that a 32GB
	 * card's byte count overflows a uint32. Clamped as well, because a
	 * host filesystem really can be big enough to overflow the KB
	 * figure too, and reporting a wrapped number is worse than
	 * reporting an implausibly round one. */
	{
		uint64_t total = ((uint64_t)vfs.f_blocks * vfs.f_frsize) / 1024u;
		uint64_t avail = ((uint64_t)vfs.f_bavail * vfs.f_frsize) / 1024u;
		if (total > 0xffffffffu) total = 0xffffffffu;
		if (avail > 0xffffffffu) avail = 0xffffffffu;
		bus_write32(m, F(0), (uint32_t)total);
		bus_write32(m, F(1), (uint32_t)avail);
	}

	return 1;
}

/* ------------------------------------------------------------------- */

int simos_syscall(machine_t *m, uint32_t id, uint32_t obj) {

	switch (id) {

	/* -- time and identity -- */

	case ZSYS_UPTIME:
		bus_write32(m, obj + 0, 2);					/* Z_UINT32 */
		bus_write32(m, obj + 4, simos_uptime_ticks());
		return 1;

	case ZSYS_GETPID:
		bus_write32(m, obj + 0, 2);					/* Z_UINT32 */
		bus_write32(m, obj + 4, 4);
		return 1;

	/* -- messaging: there is one process -- */

	case ZSYS_MSG_SEND:
		/* Accepted and dropped. Reporting failure instead would be
		 * more honest about what happened, but a send failure means
		 * "mailbox full" to every caller in the tree, and apps retry
		 * on it -- so the honest answer produces an infinite retry
		 * loop where the vacuous one produces an app that carries on
		 * with nobody listening, which is what running one app with no
		 * OS actually is. */
		return 1;

	case ZSYS_MSG_READ:
		return 0;									/* mailbox is empty */

	case ZSYS_PROC_WAIT:
		/* No scheduler to yield to, so this cannot block for a message
		 * that can never arrive -- Z_TICK_HZ/N waits would hang and
		 * z_proc_wait(0) would hang forever. Sleep briefly instead, so
		 * an idle app costs the host nothing while still making
		 * progress. Not faithful; the alternative is a simulator that
		 * appears to hang on every well-behaved app in the tree, which
		 * is worse. */
		{
			struct timespec ts = { 0, 1000000 };	/* 1ms */
			nanosleep(&ts, NULL);
		}
		return 1;

	case ZSYS_PID_REGISTER:
		return 1;
	case ZSYS_PID_LOOKUP:
		return 0;									/* nothing is running */

	case ZSYS_PROC_RUN:
	case ZSYS_PROC_KILL:
	case ZSYS_PROC_LIST:
		return 0;

	case ZSYS_PROC_STATUS:
		/* There is one process and it is the one asking, so any pid it
		 * names is not running here. UNKNOWN with status 0 is the
		 * truthful answer and it is a SUCCESSFUL one -- a shell that
		 * got a failure could not tell it from a malformed call. See
		 * syscalls.def's PROC_STATUS entry. */
		bus_write32(m, obj + 4, 0);     /* state = Z_PROC_STATE_UNKNOWN */
		bus_write32(m, obj + 8, 0);     /* status */
		return 1;

	/* -- display mode: a socctl register, not really an OS service -- */

	case ZSYS_VIDEO_GET_MODE:
		bus_write32(m, obj + 0, 2);					/* Z_UINT32 */
		bus_write32(m, obj + 4, 0);
		return 1;
	case ZSYS_VIDEO_SET_MODE:
		return 1;

	case ZSYS_HID_PTR_SUBSCRIBE:
		return 0;

	/* -- filesystem -- */

	case ZSYS_FS_SIZE:			return sys_fs_size(m, obj);
	case ZSYS_EXEC_EXISTS:		return sys_fs_size(m, obj);
	case ZSYS_FS_READ:			return sys_fs_read(m, obj);
	case ZSYS_FS_WRITE:			return sys_fs_write(m, obj);
	case ZSYS_FS_UNLINK:		return sys_fs_unlink(m, obj);
	case ZSYS_FS_LIST:			return sys_fs_list(m, obj);
	case ZSYS_FS_MKDIR:			return sys_fs_mkdir(m, obj);
	case ZSYS_FS_TOUCH:			return sys_fs_touch(m, obj);
	case ZSYS_FS_DF:			return sys_fs_df(m, obj);

	case ZSYS_FS_OPEN_READ:		return sys_fs_open(m, obj, "rb", 0);
	case ZSYS_FS_OPEN_WRITE:	return sys_fs_open(m, obj, "wb", 1);
	case ZSYS_FS_OPEN_RW:		return sys_fs_open(m, obj, "r+b", 1);
	case ZSYS_FS_READ_CHUNK:	return sys_fs_read_chunk(m, obj);
	case ZSYS_FS_WRITE_CHUNK:	return sys_fs_write_chunk(m, obj);
	case ZSYS_FS_SEEK:			return sys_fs_seek(m, obj);
	case ZSYS_FS_TRUNCATE:		return sys_fs_truncate(m, obj);
	case ZSYS_FS_SYNC:			return sys_fs_sync(m, obj);
	case ZSYS_FS_CLOSE:			return sys_fs_close(m, obj);

	default:
		break;
	}

	/* Reported once per id, not once per call: an app polling an
	 * unimplemented syscall in its main loop would otherwise bury
	 * everything else on stderr under it. */
	{
		static uint8_t seen[256];
		if (id < sizeof(seen) && !seen[id]) {
			seen[id] = 1;
			fprintf(stderr, "zeitlos-sim: unimplemented syscall id=%u "
				"(see sim/simos.c)\n", id);
		}
	}

	return 0;
}
