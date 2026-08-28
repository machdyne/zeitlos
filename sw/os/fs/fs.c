#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fatfs/ff.h"
#include "fs.h"
#include "../kernel.h"	// maskirq(), z_pid (fs_lock)
#include "../../common/zexec.h"
#include "../../common/zsoc.h"
#include "../zar.h"

FATFS sdvol0;


// -- mutual exclusion for FatFs + the SD driver --------------------------
// Neither is re-entrant, and syscalls run in the caller's own context
// with interrupts enabled: a KTIMER swap in the middle of one f_read()
// onto another process that also touches the card interleaves two SPI
// transactions (fsapi.h's "Concurrency note"). Seen on the ULX3S:
// 2026-08-27 net reading NET.CFG while init() streamed repl (desktop
// frozen), 2026-08-28 wm's dock scan (11 EXEC_EXISTS syscalls) while
// init() loaded net/repl ("not installed", net image corrupted, reset).
// Recursive per pid because fs.c's functions call each other; waits
// with interrupts enabled so the holder keeps getting scheduled.
static volatile uint32_t fs_lock_owner = 0xFFFFFFFFu;
static volatile uint32_t fs_lock_depth = 0;

void fs_lock(void) {
	for (;;) {
		uint32_t m = maskirq(0xFFFFFFFF);
		if (fs_lock_depth == 0 || fs_lock_owner == z_pid) {
			fs_lock_owner = z_pid;
			fs_lock_depth++;
			maskirq(m);
			return;
		}
		maskirq(m);
	}
}

void fs_unlock(void) {
	uint32_t m = maskirq(0xFFFFFFFF);
	if (fs_lock_depth > 0 && --fs_lock_depth == 0)
		fs_lock_owner = 0xFFFFFFFFu;
	maskirq(m);
}

// scheduler death cleanup: a process killed while inside the
// filesystem must not leave everyone else spinning forever
void fs_lock_release_pid(uint32_t pid) {
	uint32_t m = maskirq(0xFFFFFFFF);
	if (fs_lock_owner == pid) {
		fs_lock_depth = 0;
		fs_lock_owner = 0xFFFFFFFFu;
	}
	maskirq(m);
}

static int fs_load__unlocked(uint32_t dst, char *path) {

	FIL f;
	FRESULT res;
	FSIZE_t sz;
	UINT br;

	char buf[1024];
	char *dst_ptr = (char *)dst;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);

	if (res != FR_OK)
		return 1;

	sz = f_size(&f);

	// printf("loading %li bytes ...\n", sz);

	int blks = sz / 1024;

	for (int i = 0; i < blks; i++) {
		res = f_read(&f, buf, 1024, &br);
		memcpy(dst_ptr, &buf, 1024);
		dst_ptr += 1024;
		if (res != FR_OK) return 1;
	}

	res = f_read(&f, buf, sz - (blks * 1024), &br);
	memcpy(dst_ptr, &buf, sz - (blks * 1024));
	if (res != FR_OK) return 1;

	f_close(&f);

	return 0;

}

static void * fs_mallocfile__unlocked(char *path) {

	FIL f;
	FRESULT res;
	FSIZE_t sz;
	UINT br;

	void *buf;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);

	if (res != FR_OK)
		return NULL;

	sz = f_size(&f);

	buf = malloc(sz);
	
	res = f_read(&f, buf, sz, &br);
	if (res != FR_OK) return NULL;

	f_close(&f);

	return buf;

}

static int fs_touch__unlocked(char *path) {

	FIL f;
	FRESULT res;

	res = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);

	if (res == FR_OK) {
		f_close(&f);
		printf("file touched.\n");
		return 0;
	}

	printf("write failed; error code: %i\n", res);
	return 1;

}

static int fs_mount__unlocked(void) {
	FRESULT res;
	res = f_mount(&sdvol0, "", 0);
	if (res == FR_OK)
		return 0;
	else
		return 1;
}

// Mount the card NOW, rather than deferring it.
//
// fs_mount() above passes opt=0, a deferred mount: FatFs registers the
// volume and returns success without touching the hardware, and the
// card is only really initialised by the first file operation that
// needs it. That is fine when something is about to do file I/O
// anyway, and actively wrong when nothing is -- the card sits
// uninitialised and every later operation returns FR_NOT_READY.
//
// opt=1 forces the mount immediately, so the return value actually
// means "there is a working filesystem here". Used at boot to bring
// the card up regardless of whether the core apps are going to be
// loaded from it, because `ls`, `xf` and everything else afterwards
// depend on it having happened.
//
// Returns 0 on success. A slow card may need a few attempts, so
// callers should retry rather than treating one failure as final.
static int fs_mount_now__unlocked(void) {
	FRESULT res;
	res = f_mount(&sdvol0, "", 1);
	if (res == FR_OK)
		return 0;
	else
		return 1;
}

static int fs_format__unlocked(void) {

	FRESULT res;
	BYTE work[FF_MAX_SS];

	printf("formating ...\n");

	res = f_mkfs("", 0, work, sizeof work);

	if (res == FR_OK) {
		printf("format succeeded.\n");
		return 0;
	}

	printf("write failed; error code: %i\n", res);
	return 1;

}

static uint32_t fs_total__unlocked(void) {
	FATFS *fs;
	FRESULT res;
	DWORD fre_clust;

	res = f_getfree("", &fre_clust, &fs);

	if (res == FR_OK) {
		return ((fs->n_fatent - 2) * fs->csize) / 2;
	}

	return 0;
}

// Both capacity figures from ONE f_getfree() call.
//
// fs_total() and fs_free() below each make their own, and f_getfree()
// on FAT32 walks the entire FAT to count free clusters -- seconds on
// a large card, during which the calling process is stuck. Asking for
// "how full is the card" therefore paid that cost TWICE, for two
// numbers that come out of the same scan.
//
// FatFs caches the count afterwards (fs->free_clst) and later calls
// are cheap until a write invalidates it, so this mostly matters for
// the first call after mounting or after writing -- which is exactly
// when something like a file browser or a system monitor asks.
//
// Both figures in KB, matching fs_total()/fs_free().
static void fs_df_kb__unlocked(uint32_t *total_kb, uint32_t *free_kb) {

	FATFS *fs;
	DWORD fre_clust;

	if (total_kb) *total_kb = 0;
	if (free_kb) *free_kb = 0;

	if (f_getfree("", &fre_clust, &fs) != FR_OK) return;

	if (total_kb) *total_kb = ((fs->n_fatent - 2) * fs->csize) / 2;
	if (free_kb) *free_kb = (fre_clust * fs->csize) / 2;

}

static uint32_t fs_free__unlocked(void) {
	FATFS *fs;
	FRESULT res;
	DWORD fre_clust;

	res = f_getfree("", &fre_clust, &fs);

	if (res == FR_OK) {
		return (fre_clust * fs->csize) / 2;
	}

	return 0;
}

static uint32_t fs_size__unlocked(char *path) {
	FIL f;
	FRESULT res;
	FSIZE_t fs = 0;
	res = f_open(&f, path, FA_WRITE | FA_OPEN_EXISTING);
	if (res == FR_OK) {
		fs = f_size(&f);
	}
	f_close(&f);
	return(fs);
}

static int fs_write_file__unlocked(char *path, char *buf, uint32_t len) {

	FIL f;
	FRESULT res;
	UINT bw;

	res = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);

	if (res == FR_OK) {

		f_write(&f, buf, len, &bw);
		res = f_close(&f);

		return bw;

	}

	printf("write failed; error code: %i\n", res);
	return 0;

}

// -- chunked (streaming) read/write -- see fs.h --

// Bytes written since the last metadata flush. See FS_SYNC_INTERVAL.
static uint32_t chunk_unsynced;

static int fs_open_write__unlocked(FIL *f, char *path) {
	chunk_unsynced = 0;
	FRESULT res = f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK) {
		printf("fs_open_write: failed; error code: %i\n", res);
		return 0;
	}
	return 1;
}

// How much unflushed data to tolerate before forcing a metadata write.
//
// A sync per chunk would be safest and far too slow -- it rewrites the
// directory entry and FAT every time. 64KB bounds what an interrupted
// transfer can lose or leave dangling, at roughly one extra metadata
// write per 128 blocks, which is lost in the noise of the transfer
// itself.
#define FS_SYNC_INTERVAL   (64 * 1024)

static int fs_write_chunk__unlocked(FIL *f, const void *buf, uint32_t len) {
	UINT bw;
	FRESULT res = f_write(f, buf, len, &bw);
	if (res != FR_OK) {
		printf("fs_write_chunk: failed; error code: %i\n", res);
		return -1;
	}

	// Periodic metadata flush -- see fs_sync() for why this matters.
	// Without it the directory entry stays stale for the whole of a
	// long transfer, so an interruption anywhere in it loses the lot
	// AND leaves clusters allocated with nothing pointing at them.
	chunk_unsynced += bw;
	if (chunk_unsynced >= FS_SYNC_INTERVAL) {
		f_sync(f);
		chunk_unsynced = 0;
	}

	return (int)bw;
}

// Flush everything FatFs is holding in RAM out to the card.
//
// FatFs buffers aggressively and, crucially, does NOT update a file's
// directory entry (its size and first cluster) until f_close(). With
// FF_FS_TINY 0 there is also a 512-byte sector buffer inside every FIL
// and another inside the volume itself. So between opening a file and
// closing it, the on-card filesystem is inconsistent by design: some
// clusters are allocated in the FAT with no directory entry pointing
// at them.
//
// Lose power in that window -- or, far more likely during development,
// reprogram the FPGA -- and the card is left with lost clusters and
// half-written directory records. That is exactly the damage `fsck`
// reported ("Wrong checksum for long file name"): interrupted writes,
// not bad hardware.
//
// f_sync() does what f_close() does to the metadata while leaving the
// file open, so this bounds the damage to whatever was written since
// the last call rather than the whole transfer.
static int fs_sync__unlocked(FIL *f) {
	if (!f) return 0;
	return (f_sync(f) == FR_OK) ? 1 : 0;
}

// Flush and unmount the volume. Call before deliberately cutting power
// or reprogramming; after this the card is consistent and can be
// removed. fs_mount_now() brings it back.
static int fs_unmount__unlocked(void) {
	FRESULT res = f_mount(NULL, "", 0);
	return (res == FR_OK) ? 0 : 1;
}

static int fs_close_write__unlocked(FIL *f) {
	FRESULT res = f_close(f);
	return (res == FR_OK) ? 1 : 0;
}

static int fs_open_read__unlocked(FIL *f, char *path) {
	FRESULT res = f_open(f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) {
		printf("fs_open_read: failed; error code: %i\n", res);
		return 0;
	}
	return 1;
}

static int32_t fs_read_chunk__unlocked(FIL *f, void *buf, uint32_t maxlen) {
	UINT br;
	FRESULT res = f_read(f, buf, maxlen, &br);
	if (res != FR_OK) {
		printf("fs_read_chunk: failed; error code: %i\n", res);
		return -1;
	}
	return (int32_t)br;	// 0 means EOF (nothing left to read), matching f_read()'s own convention
}

static int fs_close_read__unlocked(FIL *f) {
	FRESULT res = f_close(f);
	return (res == FR_OK) ? 1 : 0;
}

static int fs_mkdir__unlocked(char *path) {

	FRESULT res;

	printf("making directory '%s' ...\n", path);

	res = f_mkdir(path);

	if (res != FR_OK) {
		printf("mkdir failed; error code: %i\n", res);
		return 1;
	}

	return 0;

}

static int fs_unlink__unlocked(char *path) {

	FRESULT res;

	printf("deleting '%s' ...\n", path);

	res = f_unlink(path);

	if (res != FR_OK) {
		printf("unlink failed; error code: %i\n", res);
		return 1;
	}

	return 0;

}

static void fs_list_dir__unlocked(char *path) {

	FRESULT res;
	DIR dir;
	static FILINFO fno;

	res = f_opendir(&dir, path);

	// Say so, rather than printing nothing. An unreadable card and
	// an empty one look identical otherwise, and with core apps in
	// flash the "in flash:" section below still prints -- so a
	// failed mount presents as "only the flash apps are listed",
	// which looks like a bug in the underlay rather than a card that
	// never came up.
	if (res != FR_OK)
		printf("(filesystem unavailable: error %d)\n", (int)res);

	if (res == FR_OK) {
		for (;;) {
			res = f_readdir(&dir, &fno);
			if (res != FR_OK || fno.fname[0] == 0) break;
			if (fno.fattrib & AM_DIR) {
				// `path[i] = 0` used to live here, writing into the
				// caller's buffer at strlen(path) -- i.e. over the
				// existing NUL. Harmless in value, undefined in
				// principle: sh.c calls fs_list_dir("/"), a string
				// literal, so this was a write into .rodata. Nothing
				// read the result and nothing depended on it, so it is
				// simply gone rather than made conditional.
				printf("%s\n", fno.fname);
				if (res != FR_OK) break;
			} else {
				printf("%s/%s\n", path, fno.fname);
			}
		}
		f_closedir(&dir);
	}

	// Core apps living in flash (sw/os/zar.h) are listed after the
	// filesystem's own contents, marked, and only for the top-level
	// directory.
	//
	// They are NOT files -- there is no directory entry, they cannot be
	// opened, read, written or deleted, and only the process-launch
	// path (fs_exec_info_any() below) ever resolves them. Listing them
	// anyway is about not creating a mystery: on a board with no SD
	// card, `ls` would otherwise show nothing at all while `run term`
	// worked perfectly, which is exactly the sort of thing that costs
	// somebody an afternoon.
	//
	// An entry shadowed by a real file on the card is skipped rather
	// than shown twice -- what `ls` prints should match what `run`
	// would actually launch, and the filesystem copy is the one that
	// wins (see fs_exec_info_any()).
	if (z_zar_count() && (path[0] == 0 || (path[0] == '/' && path[1] == 0))) {

		int shown = 0;
		char name[Z_ZAR_NAME_MAX + 1];
		static FILINFO st;	// static: see the f_stat() note below

		for (uint32_t zi = 0; zi < z_zar_count(); zi++) {

			if (!z_zar_name(zi, name)) continue;

			// shadowed by a real file -- already listed above.
			//
			// f_stat(), NOT fs_exec_info(). fs_exec_info() OPENS the
			// file, and with FF_FS_TINY=0 (ffconf.h) every FIL carries
			// its own FF_MAX_SS sector buffer -- so that call put an
			// extra ~550 bytes of stack inside `ls`, on top of a live
			// DIR and whatever printf needs, for no reason at all: the
			// question here is only "does a directory entry with this
			// name exist", which needs no file handle and no data read.
			//
			// That extra depth is a plausible cause of the intermittent
			// garbled filenames and failed writes seen after the flash
			// underlay landed -- `ls` was the one command that suddenly
			// got deeper, and a stack that reaches into the FATFS work
			// area corrupts exactly the directory buffers being read.
			if (f_stat(name, &st) == FR_OK) continue;

			if (!shown) {
				printf("\nin flash:\n");
				shown = 1;
			}

			printf("%s\n", name);

		}

	}

	return;

}

// -- Zeitlos executable format (sw/common/zexec.h) --
//
// fs_load() above loads a file verbatim and knows nothing about what
// is in it. These two understand the ZEXE header: bss is a number to
// be memset() rather than a region of zeros to be read off the card,
// which is where the load-time saving comes from.
//
// Split into inspect-then-load, not one call, because the caller needs
// the image size BEFORE it can allocate: k_proc_create() has to be
// handed data_size + bss_size, and only then is there a base address
// to load into.

// Reads and parses the header. Returns 0 on success. A file with no
// magic is not an error -- it reports as a legacy raw binary, which is
// exactly right for an old --pad-to image whose bss is already present
// as zeros (see z_exec_parse()).
static int fs_exec_info__unlocked(char *path, z_exec_info_t *info) {

	FIL f;
	FRESULT res;
	UINT br = 0;
	uint8_t hdr[Z_EXEC_HEADER_SIZE];

	if (!path || !info) return 1;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) return 1;

	uint32_t sz = (uint32_t)f_size(&f);

	res = f_read(&f, hdr, Z_EXEC_HEADER_SIZE, &br);
	f_close(&f);

	if (res != FR_OK) return 1;

	return z_exec_parse(hdr, (uint32_t)br, sz, info);

}

// Loads the data section to `dst` and zeroes the bss immediately after
// it. `info` must come from fs_exec_info() on the same file.
//
// The bss memset() is the entire point of the format: for `repl` that
// is ~110KB that used to be read from the SD card one 1KB block at a
// time and is now a single memset over RAM.
// -- executable resolution: filesystem first, flash underneath --
//
// The core apps live in flash as well as (optionally) on the SD card
// -- see sw/os/zar.h. These two are the single place that decides
// which copy a launch uses, so every path that starts a process gets
// the same answer: sh.c's `run`, sh.c's `init`, and k_proc_run()
// (which is what wm's dock calls).
//
// The rule is one line: if the filesystem has it, use that; otherwise
// fall back to the flash copy. Deliberately an UNDERLAY rather than a
// second namespace -- there is still exactly one name for `term`, and
// `run term` works identically whether it came from a card or from
// flash, whether a card is present at all, and whether the app was
// just killed and is being restarted.
//
// Drive letters (A: = flash, B: = card) were the alternative and were
// rejected as the wrong shape for this problem: every path-taking API
// in the tree -- fs_open/size/read/write, ls, te, repl's file API,
// tget/tput -- would have to learn about drives, writes to the
// read-only flash drive would need a new failure path in each of them,
// and callers like the dock would then need to know WHICH drive an app
// lives on, which is precisely the thing they should not have to care
// about. That is a namespace solution to what is actually a fallback
// question. If explicit selection is ever genuinely needed, a
// "flash:term" prefix handled here is a much smaller change than
// teaching the whole filesystem API about drives.
//
// The shadowing rule is unchanged and intentional: a file on the card
// wins, because the only way it got there was somebody deliberately
// putting it there. Callers that want to report which source was used
// can compare fs_exec_info()'s result themselves; both sh.c's init and
// `run` print it.
static int fs_exec_info_any__unlocked(char *path, z_exec_info_t *info) {

	if (fs_exec_info(path, info) == 0 && info->total)
		return 0;

	if (z_zar_exec_info(path, info) == 0 && info->total)
		return 0;

	return 1;

}

// true if `path` resolved to the flash copy rather than the card.
// Only for reporting -- the loader below re-resolves on its own.
static int fs_exec_is_flash__unlocked(char *path) {
	z_exec_info_t tmp;
	if (fs_exec_info(path, &tmp) == 0 && tmp.total) return 0;
	return (z_zar_exec_info(path, &tmp) == 0 && tmp.total) ? 1 : 0;
}

// Loads whichever copy fs_exec_info_any() would have chosen. Re-runs
// the same test rather than taking a source argument: two functions
// that must be called with matching arguments is exactly the kind of
// pairing that eventually gets called with mismatched ones.
static int fs_load_exec_any__unlocked(uint32_t dst, char *path, const z_exec_info_t *info) {

	if (fs_exec_is_flash(path))
		return z_zar_load_exec(dst, path, info);

	return fs_load_exec(dst, path, info);

}

static int fs_load_exec__unlocked(uint32_t dst, char *path, const z_exec_info_t *info) {

	FIL f;
	FRESULT res;
	UINT br;

	char buf[1024];
	char *dst_ptr = (char *)dst;

	if (!path || !info) return 1;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) return 1;

	// skip the header -- legacy files have data_off 0, so this is a
	// no-op for them and the same code path serves both formats.
	if (info->data_off) {
		res = f_lseek(&f, info->data_off);
		if (res != FR_OK) { f_close(&f); return 1; }
	}

	uint32_t left = info->data_size;

	while (left) {
		uint32_t n = (left > sizeof(buf)) ? (uint32_t)sizeof(buf) : left;
		res = f_read(&f, buf, n, &br);
		if (res != FR_OK || br != n) { f_close(&f); return 1; }
		memcpy(dst_ptr, buf, n);
		dst_ptr += n;
		left -= n;
	}

	f_close(&f);

	// Nothing else zeroes .bss on this OS -- there is no crt0 doing it,
	// which is why the old format shipped it as literal zeros in the
	// file. Doing it here is what makes dropping them safe.
	if (info->bss_size) memset(dst_ptr, 0, info->bss_size);

	// We have just written CODE through the data path. The
	// instruction cache (rtl/cache.v) caches fetches only, so it
	// never saw any of those stores -- but it may still hold lines
	// for these same PHYSICAL addresses from a previous occupant of
	// this memory, since k_mem_free()/k_mem_alloc() happily hand the
	// same base back out to the next app. Without this flush that
	// app runs the previous one's instructions.
	//
	// This is the ONLY app loader in the tree (kernel.c, sh.c), so
	// this one call covers every launch path. The only other place
	// code is written as data is sw/bios/bios.c's load_zeitlos(),
	// which has its own flush.
	z_icache_flush();

	return 0;

}

// -- locked entry points (see fs_lock() in fs.c) --------------------
int fs_load(uint32_t dst, char *path) {
	fs_lock();
	int r = fs_load__unlocked(dst, path);
	fs_unlock();
	return r;
}

void * fs_mallocfile(char *path) {
	fs_lock();
	void * r = fs_mallocfile__unlocked(path);
	fs_unlock();
	return r;
}

int fs_touch(char *path) {
	fs_lock();
	int r = fs_touch__unlocked(path);
	fs_unlock();
	return r;
}

int fs_mount(void) {
	fs_lock();
	int r = fs_mount__unlocked();
	fs_unlock();
	return r;
}

int fs_mount_now(void) {
	fs_lock();
	int r = fs_mount_now__unlocked();
	fs_unlock();
	return r;
}

int fs_format(void) {
	fs_lock();
	int r = fs_format__unlocked();
	fs_unlock();
	return r;
}

uint32_t fs_total(void) {
	fs_lock();
	uint32_t r = fs_total__unlocked();
	fs_unlock();
	return r;
}

void fs_df_kb(uint32_t *total_kb, uint32_t *free_kb) {
	fs_lock();
	fs_df_kb__unlocked(total_kb, free_kb);
	fs_unlock();
}

uint32_t fs_free(void) {
	fs_lock();
	uint32_t r = fs_free__unlocked();
	fs_unlock();
	return r;
}

uint32_t fs_size(char *path) {
	fs_lock();
	uint32_t r = fs_size__unlocked(path);
	fs_unlock();
	return r;
}

int fs_write_file(char *path, char *buf, uint32_t len) {
	fs_lock();
	int r = fs_write_file__unlocked(path, buf, len);
	fs_unlock();
	return r;
}

int fs_open_write(FIL *f, char *path) {
	fs_lock();
	int r = fs_open_write__unlocked(f, path);
	fs_unlock();
	return r;
}

int fs_write_chunk(FIL *f, const void *buf, uint32_t len) {
	fs_lock();
	int r = fs_write_chunk__unlocked(f, buf, len);
	fs_unlock();
	return r;
}

int fs_sync(FIL *f) {
	fs_lock();
	int r = fs_sync__unlocked(f);
	fs_unlock();
	return r;
}

int fs_unmount(void) {
	fs_lock();
	int r = fs_unmount__unlocked();
	fs_unlock();
	return r;
}

int fs_close_write(FIL *f) {
	fs_lock();
	int r = fs_close_write__unlocked(f);
	fs_unlock();
	return r;
}

int fs_open_read(FIL *f, char *path) {
	fs_lock();
	int r = fs_open_read__unlocked(f, path);
	fs_unlock();
	return r;
}

int32_t fs_read_chunk(FIL *f, void *buf, uint32_t maxlen) {
	fs_lock();
	int32_t r = fs_read_chunk__unlocked(f, buf, maxlen);
	fs_unlock();
	return r;
}

int fs_close_read(FIL *f) {
	fs_lock();
	int r = fs_close_read__unlocked(f);
	fs_unlock();
	return r;
}

int fs_mkdir(char *path) {
	fs_lock();
	int r = fs_mkdir__unlocked(path);
	fs_unlock();
	return r;
}

int fs_unlink(char *path) {
	fs_lock();
	int r = fs_unlink__unlocked(path);
	fs_unlock();
	return r;
}

void fs_list_dir(char *path) {
	fs_lock();
	fs_list_dir__unlocked(path);
	fs_unlock();
}

int fs_exec_info(char *path, z_exec_info_t *info) {
	fs_lock();
	int r = fs_exec_info__unlocked(path, info);
	fs_unlock();
	return r;
}

int fs_exec_info_any(char *path, z_exec_info_t *info) {
	fs_lock();
	int r = fs_exec_info_any__unlocked(path, info);
	fs_unlock();
	return r;
}

int fs_exec_is_flash(char *path) {
	fs_lock();
	int r = fs_exec_is_flash__unlocked(path);
	fs_unlock();
	return r;
}

int fs_load_exec_any(uint32_t dst, char *path, const z_exec_info_t *info) {
	fs_lock();
	int r = fs_load_exec_any__unlocked(dst, path, info);
	fs_unlock();
	return r;
}

int fs_load_exec(uint32_t dst, char *path, const z_exec_info_t *info) {
	fs_lock();
	int r = fs_load_exec__unlocked(dst, path, info);
	fs_unlock();
	return r;
}
