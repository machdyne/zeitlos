/*
 * Host test for FS_RENAME's design and the pure helpers behind it.
 *
 *   cc -std=gnu99 -Wall -I sw/os -I sw/os/fs -I sw/common \
 *      -o /tmp/t sw/os/tests/test_fsrename.c \
 *      sw/os/fs/fatfs/ff.c sw/os/fs/fatfs/ffunicode.c && /tmp/t
 *
 * Runs the project's own FatFs (sw/os/fs/fatfs, with its ffconf.h) on
 * three RAM disks standing in for drives 0: (the card), 1: (/ram) and
 * 2: (/usb). docs/filesystem.md, "Rename, stat and the extended
 * listing", explains the design these checks stand behind:
 *
 *  1. The hazard is real. With FF_FS_LOCK 0, renaming a file that is
 *     open for writing and then closing the handle loses what the
 *     handle wrote: its size goes into the old, deleted entry.
 *  2. The busy check is exact. A read-only probe opened on the file
 *     has the same dir_sect/dir_ptr as the write handle -- under its
 *     long name AND its 8.3 alias -- and a probe on any other file
 *     does not.
 *  3. f_rename() ignores the drive in the new name, which is why a
 *     cross-volume rename must be refused before FatFs sees it.
 *  4. fs_fattime_of() agrees with zfs.h's z_fs_info_time() -- the
 *     encoder the kernel uses and the decoder apps use -- over every
 *     day FAT can represent.
 *  5. fs_vol_of()/fs_is_root()/fs_is_within() behave at the edges.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "fs.h"

// -- three RAM disks --

#define NSEC 4096			// 2MB each
static uint8_t disk[3][NSEC * 512];

DSTATUS disk_status(BYTE pd) { return pd < 3 ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pd) { return disk_status(pd); }
DRESULT disk_read(BYTE pd, BYTE *buf, LBA_t sec, UINT n) {
	if (pd > 2 || sec + n > NSEC) return RES_PARERR;
	memcpy(buf, disk[pd] + sec * 512, n * 512);
	return RES_OK;
}
DRESULT disk_write(BYTE pd, const BYTE *buf, LBA_t sec, UINT n) {
	if (pd > 2 || sec + n > NSEC) return RES_PARERR;
	memcpy(disk[pd] + sec * 512, buf, n * 512);
	return RES_OK;
}
DRESULT disk_ioctl(BYTE pd, BYTE cmd, void *buf) {
	switch (cmd) {
	case CTRL_SYNC: return RES_OK;
	case GET_SECTOR_COUNT: *(LBA_t *)buf = NSEC; return RES_OK;
	case GET_SECTOR_SIZE: *(WORD *)buf = 512; return RES_OK;
	case GET_BLOCK_SIZE: *(DWORD *)buf = 1; return RES_OK;
	}
	return RES_PARERR;
}

static uint32_t fake_now = 1790433008u;		// 2026-09-26 14:30:08 UTC
DWORD get_fattime(void) { return fs_fattime_of(fake_now); }

// -- checking --

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put(const char *path, const char *text) {
	FIL f; UINT bw;
	CHECK(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
	CHECK(f_write(&f, text, strlen(text), &bw) == FR_OK);
	CHECK(f_close(&f) == FR_OK);
}

static FSIZE_t size_of(const char *path) {
	FILINFO fi;
	return f_stat(path, &fi) == FR_OK ? fi.fsize : (FSIZE_t)-1;
}

// The comparison k_fs_write_open() (sw/os/fsapi.c) makes.
static bool same_entry(const FIL *w, const FIL *probe) {
	return w->obj.fs == probe->obj.fs && w->dir_sect == probe->dir_sect &&
		w->dir_ptr == probe->dir_ptr;
}

int main(void) {

	static FATFS fs[3];
	static BYTE work[FF_MAX_SS];
	MKFS_PARM opt = { FM_FAT, 1, 0, 0, 0 };
	char vol[3] = "0:";

	for (int d = 0; d < 3; d++) {
		vol[0] = '0' + d;
		CHECK(f_mkfs(vol, &opt, work, sizeof(work)) == FR_OK);
		CHECK(f_mount(&fs[d], vol, 1) == FR_OK);
	}

	// -- 1. the hazard --
	{
		FIL w; UINT bw;
		const char *text = "written while open";
		CHECK(f_open(&w, "0:/live.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
		CHECK(f_write(&w, text, strlen(text), &bw) == FR_OK);
		CHECK(f_rename("0:/live.txt", "0:/moved.txt") == FR_OK);
		CHECK(f_close(&w) == FR_OK);
		// The data went to the moved file's clusters, but its size
		// was recorded in the old, deleted entry: the file reads as
		// empty. This is what Z_FS_ERR_BUSY prevents.
		CHECK(size_of("0:/moved.txt") == 0);
		printf("hazard: size after rename-while-writing = %lu (wrote %zu)\n",
			(unsigned long)size_of("0:/moved.txt"), strlen(text));
	}

	// -- 2. the probe identifies the entry exactly --
	{
		FIL w, probe;
		FILINFO fi;
		put("0:/other.txt", "x");
		CHECK(f_open(&w, "0:/A long file name.txt",
			FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
		CHECK(f_sync(&w) == FR_OK);

		CHECK(f_open(&probe, "0:/A long file name.txt", FA_READ) == FR_OK);
		CHECK(same_entry(&w, &probe));
		f_close(&probe);

		CHECK(f_stat("0:/A long file name.txt", &fi) == FR_OK);
		char alias[20];
		snprintf(alias, sizeof(alias), "0:/%s", fi.altname);
		CHECK(f_open(&probe, alias, FA_READ) == FR_OK);
		CHECK(same_entry(&w, &probe));
		f_close(&probe);

		CHECK(f_open(&probe, "0:/other.txt", FA_READ) == FR_OK);
		CHECK(!same_entry(&w, &probe));
		f_close(&probe);

		// A directory cannot be probed at all -- the "never busy"
		// branch in k_fs_write_open().
		CHECK(f_mkdir("0:/dir") == FR_OK);
		CHECK(f_open(&probe, "0:/dir", FA_READ) != FR_OK);

		CHECK(f_close(&w) == FR_OK);
	}

	// A directory renamed while a file inside it is open for writing:
	// the file's entry is in the directory's own clusters, which do not
	// move, so the close still records the size correctly.
	{
		FIL w; UINT bw;
		CHECK(f_open(&w, "0:/dir/inner.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
		CHECK(f_write(&w, "12345", 5, &bw) == FR_OK);
		CHECK(f_rename("0:/dir", "0:/dir2") == FR_OK);
		CHECK(f_close(&w) == FR_OK);
		CHECK(size_of("0:/dir2/inner.txt") == 5);
	}

	// -- 3. f_rename() and drives --
	{
		put("1:/onram.txt", "ram");
		CHECK(f_rename("1:/onram.txt", "0:/tocard.txt") == FR_OK);
		// It stayed on drive 1, under the new name; drive 0 has nothing.
		CHECK(size_of("1:/tocard.txt") == 3);
		CHECK(size_of("0:/tocard.txt") == (FSIZE_t)-1);
	}

	// Timestamps are written from get_fattime().
	{
		FILINFO fi;
		z_fs_info_t zi;
		put("0:/stamped.txt", "t");
		CHECK(f_stat("0:/stamped.txt", &fi) == FR_OK);
		zi.fdate = fi.fdate; zi.ftime = fi.ftime;
		CHECK(z_fs_info_time(&zi) == fake_now);
	}

	// -- 4. encoder and decoder agree, every day 1980-2106 --
	{
		uint32_t t, bad = 0;
		for (t = 315532800u; t < 4294967295u - 86400u * 2; t += 86400u + 7) {
			uint32_t ft = fs_fattime_of(t);
			z_fs_info_t zi;
			zi.fdate = ft >> 16; zi.ftime = ft & 0xFFFF;
			if (z_fs_info_time(&zi) != (t & ~1u)) bad++;
		}
		CHECK(bad == 0);
		// FAT's resolution is two seconds; odd seconds round down.
		z_fs_info_t zi;
		uint32_t ft = fs_fattime_of(1790433009u);
		zi.fdate = ft >> 16; zi.ftime = ft & 0xFFFF;
		CHECK(z_fs_info_time(&zi) == 1790433008u);
		// 2000-02-29 exists; 2100-02-29 does not: the day before
		// 2100-03-01 (4107542400) is 2100-02-28.
		ft = fs_fattime_of(951782400u);
		CHECK((ft >> 16) == (((2000 - 1980) << 9) | (2 << 5) | 29));
		ft = fs_fattime_of(4107542400u);
		CHECK((ft >> 16) == (((2100 - 1980) << 9) | (3 << 5) | 1));
		ft = fs_fattime_of(4107542400u - 86400u);
		CHECK((ft >> 16) == (((2100 - 1980) << 9) | (2 << 5) | 28));
	}

	// -- 5. path helpers --
	CHECK(fs_vol_of("/x") == 0);
	CHECK(fs_vol_of("1:/x") == 1);
	CHECK(fs_vol_of("2:") == 2);
	CHECK(fs_is_root("/") && fs_is_root("") && fs_is_root("1:/") &&
		fs_is_root("1:") && fs_is_root("//"));
	CHECK(!fs_is_root("/a") && !fs_is_root("1:/a"));
	CHECK(fs_is_within("/a", "/a"));
	CHECK(fs_is_within("/a", "/a/b"));
	CHECK(fs_is_within("/a/", "/A/B"));
	CHECK(fs_is_within("/Docs", "/docs/x/y"));
	CHECK(!fs_is_within("/a", "/ab"));
	CHECK(!fs_is_within("/a/b", "/a"));
	CHECK(!fs_is_within("/a", "/b/a"));

	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
