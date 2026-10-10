#ifndef Z_FS_H
#define Z_FS_H

#include <stdint.h>
#include <stdbool.h>

#include "fatfs/ff.h"
#include "../../common/zexec.h"
#include "../../common/zfs.h"		// z_fs_info_t, Z_FS_ERR_*

int fs_mount(void);
// forced (non-deferred) mount -- see fs.c for why this exists
int fs_mount_now(void);
int fs_format(void);
uint32_t fs_total(void);
uint32_t fs_free(void);

int fs_load(uint32_t dst, char *path);

// Zeitlos executable format -- see sw/common/zexec.h. Inspect first
// (the caller needs the image size to allocate), then load. Handles
// legacy raw --pad-to binaries transparently: they report bss_size 0
// and load exactly as fs_load() would.
int fs_exec_info(char *path, z_exec_info_t *info);
int fs_load_exec(uint32_t dst, char *path, const z_exec_info_t *info);

// Filesystem first, flash core-app archive underneath -- see fs.c.
// Every process-launch path should use these rather than the two
// above, so that a card-less board behaves identically.
// Executable resolution. A bare name is searched for in /apps, then
// the flash archive; a name containing '/' is taken literally
// (docs/layout.md, "Finding a program"). All three of these share one resolver in fs.c so they
// cannot disagree -- see its comment for why the search lives here
// rather than as an "apps/" prefix at every call site.
int fs_exec_info_any(char *path, z_exec_info_t *info);

// Both capacity figures in KB from a single FAT scan -- see fs.c for
// why calling fs_total() and fs_free() separately is worth avoiding.
void fs_df_kb(uint32_t *total_kb, uint32_t *free_kb);
int fs_load_exec_any(uint32_t dst, char *path, const z_exec_info_t *info);
int fs_exec_is_flash(char *path);	// 1 if resolved to flash
void *fs_mallocfile(char *path);
uint32_t fs_size(char *path);
int fs_write_file(char *path, char *buf, uint32_t len);

int fs_touch(char *path);
int fs_mkdir(char *path);
int fs_unlink(char *path);

// docs/filesystem.md, "Rename, stat and the extended listing".
// fs_rename() returns a Z_FS_ERR_* code (sw/common/zfs.h), 0 on success.
// -- pure helpers for fs_rename() and get_fattime() (fs.c), inline
// here so sw/os/tests/test_fsrename.c tests the shipped code --

// The FatFs drive a resolved path is on: "1:/x" is 1, "/x" is 0.
static inline int fs_vol_of(const char *p) {
	return (p[0] >= '0' && p[0] <= '9' && p[1] == ':') ? p[0] - '0' : 0;
}

// The path with its drive prefix removed.
static inline const char *fs_vol_strip(const char *p) {
	return (p[0] >= '0' && p[0] <= '9' && p[1] == ':') ? p + 2 : p;
}

// "" and "/" (after the drive) are a volume's root.
static inline bool fs_is_root(const char *p) {
	p = fs_vol_strip(p);
	while (*p == '/') p++;
	return *p == 0;
}

// Is `inner` the directory `outer` itself, or somewhere below it?
// Case-insensitive, as FAT names are. Trailing slashes on `outer` are
// ignored, so "/a/" and "/a" are the same directory.
static inline bool fs_is_within(const char *outer, const char *inner) {
	uint32_t n = 0, i;
	while (outer[n]) n++;
	while (n > 1 && outer[n - 1] == '/') n--;
	for (i = 0; i < n; i++) {
		char a = outer[i], b = inner[i];
		if (a >= 'a' && a <= 'z') a -= 32;
		if (b >= 'a' && b <= 'z') b -= 32;
		if (a != b) return false;
	}
	return inner[n] == 0 || inner[n] == '/';
}

// FAT's packed date/time (as get_fattime() returns it) for `t`, Unix
// seconds, UTC, which must be at or after 1980-01-01 (315532800).
// Among the years FAT can hold (1980-2107), a year divisible by 4 is a
// leap year with ONE exception, 2100 (y == 120 here) -- which the RTC's
// uint32 seconds do reach (they run out in 2106), so it has to be
// right; test_fsrename.c checks every day through 2106. Years past
// 2107 clamp to FAT's last second.
#define FS_LEAP(y)	(!((y) & 3) && (y) != 120)	// y: years since 1980
static inline uint32_t fs_fattime_of(uint32_t t) {
	static const uint8_t mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	uint32_t days, secs, y, m, n;
	t -= 315532800u;			// seconds since FAT's epoch
	days = t / 86400;
	secs = t % 86400;
	for (y = 0; days >= (n = FS_LEAP(y) ? 366 : 365); y++) days -= n;
	for (m = 0; m < 11; m++) {
		n = mdays[m] + (m == 1 && FS_LEAP(y));
		if (days < n) break;
		days -= n;
	}
	if (y > 127) return (127u << 25) | (12u << 21) | (31u << 16) |
		(23u << 11) | (59u << 5) | 29u;
	return (y << 25) | ((m + 1) << 21) | ((days + 1) << 16) |
		((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2);
}

int fs_rename(const char *from, const char *to);
bool fs_stat_info(const char *path, z_fs_info_t *fi);
void fs_info_from(const FILINFO *st, z_fs_info_t *fi);
extern FILINFO fs_fno;		// scratch; see fs_stat_info()

// sw/os/fsapi.c: is `path` (already resolved) open for writing by any
// process? fs_rename() refuses if so.
bool k_fs_write_open(const char *path);
void fs_list_dir(char *path);

// -- chunked (streaming) read/write --
//
// for moving a file to/from disk incrementally, without needing the
// whole thing in memory at once -- see sw/common/zstream.h, which
// this is meant to pair with. FIL (from fatfs/ff.h) is exposed
// directly rather than wrapped, since this is already a thin
// FatFs-backed API.

int fs_open_write(FIL *f, char *path);
int fs_write_chunk(FIL *f, const void *buf, uint32_t len);
int fs_close_write(FIL *f);
// flush FatFs's buffered metadata to the card -- see fs.c
int fs_sync(FIL *f);
// flush and unmount; call before cutting power or reprogramming
int fs_unmount(void);

// -- the ramdisk, mounted at /ram --
//
// Created at boot on any machine with more than 1MB of RAM (see
// kernel.c). Backed by ramdisk.c and reachable through every path
// function here: /ram/foo is FatFs 1:/foo.
//
// Not persistent, not reserved, not arbitrated -- see ramdisk.h.
// Callers must cope with it being absent or full rather than assume
// it is there.
// Rewrites a mount prefix (/ram) into a FatFs volume prefix (1:).
// Returns `path` unchanged when nothing matches.
//
// Must be applied to EVERY path that reaches FatFs. The functions in
// this file do it themselves; sw/os/fsapi.c has to call it explicitly
// because it goes to f_open()/f_opendir() directly.
#define FS_PATH_MAX 320
const char *fs_path_resolve(const char *path, char *buf, uint32_t cap);

bool fs_ramdisk_create(uint32_t bytes);
void fs_ramdisk_destroy(void);
bool fs_ramdisk_present(void);

int fs_open_read(FIL *f, char *path);
int32_t fs_read_chunk(FIL *f, void *buf, uint32_t maxlen);
int fs_close_read(FIL *f);

// USB mass storage at /usb (FatFs drive 2). Mounted on demand, not at
// boot: the drive appears when somebody plugs it in, and the mount
// blocks while the unit reports ready, so it must run in process
// context. sh.c's `usbmount` is the usual caller.
bool fs_usb_mount(void);
void fs_usb_unmount(void);
// Mount /usb on insertion, release it on removal; run by pid 0 when the
// USB driver asks (k_deferred_request(), uart.h).
void fs_usb_poll(void);
bool fs_usb_mounted(void);

// The synthetic roots -- /ram, /usb -- for anything that lists "/".
//
// They are path prefixes, not directories on the card, so f_readdir on
// the root never mentions them. A caller listing "/" should emit these
// alongside whatever FatFs returns. fs_mount_live() reports whether
// the volume is actually mounted; an unplugged /usb should not appear
// as an empty directory you can descend into.
int fs_mount_count(void);
const char *fs_mount_name(int i);
int fs_mount_live(int i);

// The card's top-level folders (docs/layout.md): makes sure /sys,
// /home and /data exist, and on a board without a ramdisk makes /tmp and empties
// it -- once per boot. Call whenever the card is known to be up; safe
// to call again.
void fs_layout_prepare(void);

#endif
