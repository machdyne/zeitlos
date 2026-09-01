#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "fatfs/ff.h"
#include "fs.h"
#include "../../common/zexec.h"
#include "../../common/zsoc.h"
#include "../zar.h"
#include "../kernel.h"		// k_fs_enter()/k_fs_leave() -- see
								// docs/filesystem.md

FATFS sdvol0;

int fs_load(uint32_t dst, char *path) {

	FIL f;
	FRESULT res;
	FSIZE_t sz;
	UINT br;

	char buf[1024];
	char *dst_ptr = (char *)dst;

	// see docs/filesystem.md
	k_fs_enter();

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);

	if (res != FR_OK) { k_fs_leave(); return 1; }

	sz = f_size(&f);

	// printf("loading %li bytes ...\n", sz);

	int blks = sz / 1024;

	for (int i = 0; i < blks; i++) {
		res = f_read(&f, buf, 1024, &br);
		memcpy(dst_ptr, &buf, 1024);
		dst_ptr += 1024;
		if (res != FR_OK) { f_close(&f); k_fs_leave(); return 1; }
	}

	res = f_read(&f, buf, sz - (blks * 1024), &br);
	memcpy(dst_ptr, &buf, sz - (blks * 1024));
	if (res != FR_OK) { f_close(&f); k_fs_leave(); return 1; }

	f_close(&f);

	k_fs_leave();

	return 0;

}

void *fs_mallocfile(char *path) {

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

int fs_touch(char *path) {

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

int fs_mount(void)
{
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
int fs_mount_now(void)
{
	FRESULT res;
	res = f_mount(&sdvol0, "", 1);
	if (res == FR_OK)
		return 0;
	else
		return 1;
}

int fs_format(void) {

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

uint32_t fs_total(void) {
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
void fs_df_kb(uint32_t *total_kb, uint32_t *free_kb) {

	FATFS *fs;
	DWORD fre_clust;

	if (total_kb) *total_kb = 0;
	if (free_kb) *free_kb = 0;

	if (f_getfree("", &fre_clust, &fs) != FR_OK) return;

	if (total_kb) *total_kb = ((fs->n_fatent - 2) * fs->csize) / 2;
	if (free_kb) *free_kb = (fre_clust * fs->csize) / 2;

}

uint32_t fs_free(void) {
	FATFS *fs;
	FRESULT res;
	DWORD fre_clust;

	res = f_getfree("", &fre_clust, &fs);

	if (res == FR_OK) {
		return (fre_clust * fs->csize) / 2;
	}

	return 0;
}

uint32_t fs_size(char *path) {
	FIL f;
	FRESULT res;
	FSIZE_t fs = 0;
	k_fs_enter();				// see docs/filesystem.md
	res = f_open(&f, path, FA_WRITE | FA_OPEN_EXISTING);
	if (res == FR_OK) {
		fs = f_size(&f);
	}
	f_close(&f);
	k_fs_leave();
	return(fs);
}

int fs_write_file(char *path, char *buf, uint32_t len) {

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

int fs_open_write(FIL *f, char *path) {
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

int fs_write_chunk(FIL *f, const void *buf, uint32_t len) {
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
int fs_sync(FIL *f) {
	if (!f) return 0;
	return (f_sync(f) == FR_OK) ? 1 : 0;
}

// Flush and unmount the volume. Call before deliberately cutting power
// or reprogramming; after this the card is consistent and can be
// removed. fs_mount_now() brings it back.
int fs_unmount(void) {
	FRESULT res = f_mount(NULL, "", 0);
	return (res == FR_OK) ? 0 : 1;
}

int fs_close_write(FIL *f) {
	FRESULT res = f_close(f);
	return (res == FR_OK) ? 1 : 0;
}

int fs_open_read(FIL *f, char *path) {
	FRESULT res = f_open(f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) {
		printf("fs_open_read: failed; error code: %i\n", res);
		return 0;
	}
	return 1;
}

int32_t fs_read_chunk(FIL *f, void *buf, uint32_t maxlen) {
	UINT br;
	FRESULT res = f_read(f, buf, maxlen, &br);
	if (res != FR_OK) {
		printf("fs_read_chunk: failed; error code: %i\n", res);
		return -1;
	}
	return (int32_t)br;	// 0 means EOF (nothing left to read), matching f_read()'s own convention
}

int fs_close_read(FIL *f) {
	FRESULT res = f_close(f);
	return (res == FR_OK) ? 1 : 0;
}

int fs_mkdir(char *path) {

	FRESULT res;

	printf("making directory '%s' ...\n", path);

	res = f_mkdir(path);

	if (res != FR_OK) {
		printf("mkdir failed; error code: %i\n", res);
		return 1;
	}

	return 0;

}

int fs_unlink(char *path) {

	FRESULT res;

	printf("deleting '%s' ...\n", path);

	res = f_unlink(path);

	if (res != FR_OK) {
		printf("unlink failed; error code: %i\n", res);
		return 1;
	}

	return 0;

}

void fs_list_dir(char *path) {

	FRESULT res;
	DIR dir;
	static FILINFO fno;

	k_fs_enter();				// see docs/filesystem.md

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

	// Released only here: the flash-archive loop above calls f_stat(),
	// which is a FatFs call like any other.
	k_fs_leave();

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
int fs_exec_info(char *path, z_exec_info_t *info) {

	FIL f;
	FRESULT res;
	UINT br = 0;
	uint8_t hdr[Z_EXEC_HEADER_SIZE];

	if (!path || !info) return 1;

	// Serialised against every other FatFs user -- see
	// docs/filesystem.md. Needed here and not only in the syscall
	// dispatcher because sh.c's init() reaches this function directly,
	// without a syscall.
	k_fs_enter();

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) { k_fs_leave(); return 1; }

	uint32_t sz = (uint32_t)f_size(&f);

	res = f_read(&f, hdr, Z_EXEC_HEADER_SIZE, &br);
	f_close(&f);

	k_fs_leave();

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
// -- the search path --
//
// A bare program name is looked for in the root, then in APPS/, then
// in the flash archive. A name containing '/' is taken literally and
// not searched at all.
//
// WHY A SEARCH PATH AND NOT A PREFIX. Moving the apps into APPS/
// without one would mean every reference to a program grows a
// constant "apps/": dock_candidates[] in wm, the extension->app table
// in ztype.c, z_proc_stack_size_for() in kernel.h, pidreg
// registrations, `run apps/term` at the shell, and the names inside
// the flash archive -- where it would also spend 5 of
// Z_ZAR_NAME_MAX's 16 bytes on a prefix that carries no information
// anywhere it appears. A prefix repeated at every call site is a
// prefix that belongs in the resolver instead.
//
// Doing it here means the directory move costs nothing above this
// line: bare names keep working everywhere, the archive stays flat,
// and a card written before the move still boots, because the root is
// still searched.
//
// This is NOT the drive-letter idea rejected below returning by
// another name. It applies to EXECUTABLE RESOLUTION only. Files are
// still opened by exact path -- fs_open/size/read/write, ls, te,
// repl's file API, tget/tput are all untouched, and none of them
// gains a way for the same string to mean two different files. The
// split is the ordinary one: data is addressed, programs are
// resolved.
//
// ROOT IS SEARCHED BEFORE APPS/, deliberately. It preserves the
// shadowing rule the underlay already documents -- the only way a
// file gets to the card root is somebody deliberately putting it
// there -- so `xf wm` still hot-swaps a single app during development
// exactly as it did before APPS/ existed. The cost is one failed
// f_open per launch on a normally-laid-out card, which is not
// measurable next to loading the executable itself.
#define FS_APPS_DIR "apps/"

// Long enough for APPS_DIR plus any name k_proc_run() can pass in
// (Z_PROC_RUN_NAME_MAX, 64) plus the NUL. A name that would not fit is
// one that could not have been launched anyway.
#define FS_RESOLVED_MAX (64 + sizeof(FS_APPS_DIR))

typedef enum {
	FS_EXEC_NONE = 0,	// no such program anywhere
	FS_EXEC_FS,			// found on the filesystem, at `resolved`
	FS_EXEC_ZAR			// found in the flash archive, under the bare name
} fs_exec_src_t;

// The one function that decides what a program name means. Everything
// below calls it; nothing below does its own searching.
//
// `resolved` receives the path that actually matched (for FS_EXEC_FS)
// or the archive name (for FS_EXEC_ZAR), so a caller that has already
// resolved can act on the result without repeating the search.
static fs_exec_src_t fs_exec_resolve(const char *name, char *resolved,
	z_exec_info_t *info) {

	z_exec_info_t tmp;
	if (!info) info = &tmp;
	if (!name || !*name) return FS_EXEC_NONE;

	// Anything with a separator in it is a path the caller means
	// literally -- the file browser launching something several
	// directories deep, for instance. Searching it would be wrong:
	// "docs/term" must not find APPS/term.
	bool is_path = false;
	for (const char *p = name; *p; p++)
		if (*p == '/' || *p == '\\') { is_path = true; break; }

	size_t len = strlen(name);
	if (len + sizeof(FS_APPS_DIR) > FS_RESOLVED_MAX) return FS_EXEC_NONE;

	// 1. literal -- the root for a bare name, the given path otherwise
	strcpy(resolved, name);
	if (fs_exec_info(resolved, info) == 0 && info->total)
		return FS_EXEC_FS;

	// 2. APPS/, for bare names only
	if (!is_path) {
		strcpy(resolved, FS_APPS_DIR);
		strcpy(resolved + sizeof(FS_APPS_DIR) - 1, name);
		if (fs_exec_info(resolved, info) == 0 && info->total)
			return FS_EXEC_FS;
	}

	// 3. the flash archive, under the bare name. Entries there are
	// flat -- no "apps/" prefix -- precisely because this resolver
	// exists; see sw/os/zar.h.
	strcpy(resolved, name);
	if (z_zar_exec_info(resolved, info) == 0 && info->total)
		return FS_EXEC_ZAR;

	return FS_EXEC_NONE;

}

int fs_exec_info_any(char *path, z_exec_info_t *info) {

	char resolved[FS_RESOLVED_MAX];
	return (fs_exec_resolve(path, resolved, info) == FS_EXEC_NONE) ? 1 : 0;

}

// true if `path` resolved to the flash copy rather than the card.
// Only for reporting -- the loader below re-resolves on its own.
int fs_exec_is_flash(char *path) {
	char resolved[FS_RESOLVED_MAX];
	return (fs_exec_resolve(path, resolved, NULL) == FS_EXEC_ZAR) ? 1 : 0;
}

// Loads whichever copy fs_exec_info_any() would have chosen. Re-runs
// the same search rather than taking a source argument: two functions
// that must be called with matching arguments is exactly the kind of
// pairing that eventually gets called with mismatched ones.
//
// Re-running it is also what makes the search safe to have at all --
// all three entry points share fs_exec_resolve(), so they cannot
// disagree about which of root, APPS/ or flash a name meant.
int fs_load_exec_any(uint32_t dst, char *path, const z_exec_info_t *info) {

	char resolved[FS_RESOLVED_MAX];
	fs_exec_src_t src = fs_exec_resolve(path, resolved, NULL);

	if (src == FS_EXEC_ZAR)
		return z_zar_load_exec(dst, resolved, info);

	if (src == FS_EXEC_FS)
		return fs_load_exec(dst, resolved, info);

	return 1;

}

int fs_load_exec(uint32_t dst, char *path, const z_exec_info_t *info) {

	FIL f;
	FRESULT res;
	UINT br;

	char buf[1024];
	char *dst_ptr = (char *)dst;

	if (!path || !info) return 1;

	// see docs/filesystem.md
	k_fs_enter();

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) { k_fs_leave(); return 1; }

	// skip the header -- legacy files have data_off 0, so this is a
	// no-op for them and the same code path serves both formats.
	if (info->data_off) {
		res = f_lseek(&f, info->data_off);
		if (res != FR_OK) { f_close(&f); k_fs_leave(); return 1; }
	}

	uint32_t left = info->data_size;

	while (left) {
		uint32_t n = (left > sizeof(buf)) ? (uint32_t)sizeof(buf) : left;
		res = f_read(&f, buf, n, &br);
		if (res != FR_OK || br != n) { f_close(&f); k_fs_leave(); return 1; }
		memcpy(dst_ptr, buf, n);
		dst_ptr += n;
		left -= n;
	}

	f_close(&f);

	// Released here rather than at the end: everything below this
	// point (the bss memset and the icache flush) touches RAM only,
	// never FatFs, and there is no reason to hold the scheduler off
	// across a memset that can be tens of kilobytes.
	k_fs_leave();

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
