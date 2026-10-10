/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * fs -- the filesystem, as a driver (docs/fs_driver.md).
 *
 * Since Phase 2b the only filesystem there is. FatFs runs here and
 * nowhere else:
 *
 *   /            the SD card (drive 0), driven directly (fatfs/sdmm.c)
 *   /ram, /tmp   the RAM disk (drive 1), kernel memory, by sector
 *                through Z_SYS_BLK; formatted here at every start
 *   /usb         USB mass storage (drive 2): the `usb` driver's drive,
 *                by request (sw/common/zusbc.c, since Phase 3)
 *   underneath   the flash archive's files (Z_SYS_ZAR): read-only,
 *                shadowed by the card, there with no card at all
 *
 * Clients speak sw/common/zfsproto.h: apps through zfsapp (zfsc.c), the
 * kernel -- its shell, program loading, cfg, the old Z_SYS_FS_* calls --
 * through sw/os/kfs.c. The kernel starts fs from flash before anything
 * else (sh.c), since nothing can read the card without it.
 *
 * One request at a time, in the order they arrive: FatFs is not
 * reentrant, and a process serialises its own work for free. A request
 * that moves data moves at most 32KB, so a long read is several
 * requests and another client's stat is served between them.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zfsproto.h"
#include "../../common/zpaths.h"
#include "../../common/zrtc.h"
#include "../../common/zsoc.h"
#include "../../common/zproc.h"
#include "../../common/zusbc.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "fsvol.h"

// [dst, dst + n) inside the client's memory [base, base + size).
#ifndef FS_IN_WINDOW
#define FS_IN_WINDOW(dst, n, base, size) \
	((uint32_t)(uintptr_t)(dst) >= (base) && \
	 (uint32_t)(uintptr_t)(dst) - (base) <= (size) && \
	 (n) <= (size) - ((uint32_t)(uintptr_t)(dst) - (base)))
#endif

#define FS_MAX_FILES	16
#define FS_MAX_DIRS		4
#define FS_MAX_CLIENTS	6
#define FS_CLMT			32		// a read handle's cluster map, in words
#define FS_PATH			(Z_FS_PATH_MAX + 8)

#define FS_TICK_HZ		732u
#define FS_IDLE_TICKS	FS_TICK_HZ			// the loop's idle wake: USB plug check
#define FS_RETRY_TICKS	FS_TICK_HZ			// card mount retries, at most this often

typedef struct {
	bool		used;
	bool		zar;			// a file in flash: zoff/zsize/zpos, no FIL
	uint8_t		vol;
	uint32_t	owner;			// pid
	uint32_t	zoff, zsize, zpos;
	FIL			f;
	DWORD		clmt[FS_CLMT];
} fs_file_t;

// A listing is three things in turn: the volumes that hang off "/"
// (root only), the card's own entries, and what is in flash under the
// same path and not on the card.
typedef struct {
	bool		used;
	bool		on_card;		// d is open
	bool		under;			// the card's namespace (not /ram, /usb)
	bool		root;
	uint8_t		phase;			// 0 volumes, 1 card, 2 flash, 3 done
	uint8_t		mi;
	uint32_t	page;			// the client's page size (zfsc.c)
	uint32_t	owner;
	uint32_t	zit;
	DIR			d;
	char		path[FS_PATH];	// as asked, for the flash part
} fs_dir_t;

// One reply area per recent client, so a reply stays put until that
// client has copied it. Read data that comes back by copy (no grant
// window) goes in the one bulk buffer instead -- see op_read().
typedef struct {
	z_fsrep_t	h;
	uint8_t		data[Z_FS_DIR_BYTES];
} fs_small_t;

typedef struct {
	z_fsrep_t	h;
	uint8_t		data[Z_FS_CHUNK];
} fs_bulk_t;

typedef struct {
	uint32_t	pid;		// + 1; 0 is free
	uint32_t	last;		// for choosing which to reuse
	bool		unread;		// its last reply may not be copied yet
	z_blob_t	hdr;
	z_fsrep_t	*rep;		// this reply: &small.h or &bulk->h
	uint8_t		*data;		// and its data
	fs_small_t	small;
} fs_client_t;

static fs_file_t files[FS_MAX_FILES];
static fs_dir_t dirs[FS_MAX_DIRS];
static fs_client_t clients[FS_MAX_CLIENTS];
static uint32_t client_clock;

static fs_bulk_t *bulk;
static uint32_t bulk_owner;		// pid + 1; 0 is free
static uint32_t bulk_when;
#define FS_BULK_HOLD_TICKS	FS_TICK_HZ

static FATFS vol_card, vol_ram, vol_usb;
static bool card_ok, card_held;	// held: `umount /`, left alone until `mount`
static uint32_t card_tried;
static bool ram_ok;
static uint32_t ram_sectors;
static bool usb_ok, usb_held;	// held: `usbunmount`, until that drive goes
static uint32_t usb_gen, usb_failed_gen;
static uint32_t usb_sectors;

static FILINFO fno;
static FIL probe;				// rename's busy check
static bool grants;				// the grant window works here
static BYTE work[FF_MAX_SS];	// f_mkfs()

static uint32_t now_ticks(void) { return z_uptime_ticks(); }

// The console, without printf: these few lines are all fs says, and the
// printf family (with its 64-bit division) is several KB of a core app
// in a flash archive that is full (docs/flash_apps.md).
int write(int fd, const void *buf, size_t len);
static void say(const char *s) { write(1, s, strlen(s)); }
static void say_num(uint32_t v) {
	char b[11];
	int i = 10;
	b[i] = 0;
	do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
	say(b + i);
}

// -- timestamps --------------------------------------------------------

DWORD get_fattime(void) {
	uint32_t t;
#ifdef FS_HOST_TEST
	(void)t;
	return ((DWORD)(2020 - 1980) << 25) | (1u << 21) | (1u << 16);
#else
	if (!z_rtc_available() || !z_rtc_valid() ||
		(t = z_rtc_seconds()) < 315532800u)		// 1980-01-01
		return ((DWORD)(2020 - 1980) << 25) | (1u << 21) | (1u << 16);
	return fs_fattime_of(t);
#endif
}

// -- the disks (FatFs's diskio) ----------------------------------------
//
// Drive 0 is the card, sdmm.c's; 1 and 2 are the kernel's, by sector.

DSTATUS sd_disk_status(BYTE drv);
DSTATUS sd_disk_initialize(BYTE drv);
DRESULT sd_disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_ioctl(BYTE drv, BYTE ctrl, void *buff);

static uint32_t blk_dev(BYTE pdrv) { return pdrv == 1 ? Z_BLK_RAM : Z_BLK_USB; }

// The RAM disk is the kernel's (Z_SYS_BLK); the USB drive is `usb`'s
// (zusbproto.h). One shape for both, so FatFs's glue below does not
// care which.
static bool blk(z_blk_args_t *a) {
	if (a->dev == Z_BLK_RAM) return z_blk(a) == Z_OK;
	switch (a->op) {
	case Z_BLK_OP_INFO: {
		z_usb_msc_info_t mi;
		if (!zusb_msc_info(&mi)) return false;
		a->flags = (mi.present ? Z_BLK_PRESENT : 0) | (mi.ready ? Z_BLK_READY : 0);
		a->sectors = mi.sectors;
		a->gen = mi.gen;
		return true;
	}
	case Z_BLK_OP_START:	return zusb_msc_start() == 0;
	case Z_BLK_OP_READ:		return zusb_msc_read(a->lba, a->count, a->buf) == 0;
	case Z_BLK_OP_WRITE:	return zusb_msc_write(a->lba, a->count, a->buf) == 0;
	}
	return false;
}

static bool blk_info(uint32_t dev, z_blk_args_t *a) {
	memset(a, 0, sizeof(*a));
	a->op = Z_BLK_OP_INFO;
	a->dev = dev;
	return blk(a);
}

DSTATUS disk_status(BYTE pdrv) {
	z_blk_args_t a;
	if (pdrv == 0) return sd_disk_status(0);
	if (pdrv > 2 || !blk_info(blk_dev(pdrv), &a)) return STA_NOINIT;
	if (!(a.flags & Z_BLK_PRESENT)) return STA_NOINIT | STA_NODISK;
	return (a.flags & Z_BLK_READY) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
	if (pdrv == 0) return sd_disk_initialize(0);
	return disk_status(pdrv);
}

static DRESULT blk_rw(BYTE pdrv, uint32_t op, void *buf, LBA_t sector, UINT count) {
	z_blk_args_t a;
	memset(&a, 0, sizeof(a));
	a.op = op;
	a.dev = blk_dev(pdrv);
	a.lba = (uint32_t)sector;
	a.count = count;
	a.buf = buf;
	return blk(&a) ? RES_OK : RES_ERROR;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
	if (pdrv == 0) return sd_disk_read(0, buff, sector, count);
	if (pdrv > 2) return RES_PARERR;
	return blk_rw(pdrv, Z_BLK_OP_READ, buff, sector, count);
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
	if (pdrv == 0) return sd_disk_write(0, buff, sector, count);
	if (pdrv > 2) return RES_PARERR;
	return blk_rw(pdrv, Z_BLK_OP_WRITE, (void *)buff, sector, count);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
	z_blk_args_t a;
	if (pdrv == 0) return sd_disk_ioctl(0, cmd, buff);
	if (pdrv > 2) return RES_PARERR;
	switch (cmd) {
	case CTRL_SYNC:			return RES_OK;
	case GET_SECTOR_COUNT:
		if (!blk_info(blk_dev(pdrv), &a)) return RES_NOTRDY;
		*(LBA_t *)buff = a.sectors;
		return RES_OK;
	case GET_SECTOR_SIZE:	*(WORD *)buff = Z_BLK_SECTOR; return RES_OK;
	case GET_BLOCK_SIZE:	*(DWORD *)buff = 1; return RES_OK;
	default:				return RES_PARERR;
	}
}

// -- the flash archive -------------------------------------------------

static bool zar_file(const char *path, uint32_t *off, uint32_t *size) {
	z_zar_args_t a;
	memset(&a, 0, sizeof(a));
	a.op = Z_ZAR_OP_FILE;
	a.path = path;
	if (z_zar(&a) != Z_OK) return false;
	*off = a.off;
	*size = a.size;
	return true;
}

static bool zar_isdir(const char *path) {
	z_zar_args_t a;
	memset(&a, 0, sizeof(a));
	a.op = Z_ZAR_OP_ISDIR;
	a.path = path;
	return z_zar(&a) == Z_OK;
}

static bool zar_read(uint32_t off, void *buf, uint32_t n) {
	z_zar_args_t a;
	if (!n) return true;
	memset(&a, 0, sizeof(a));
	a.op = Z_ZAR_OP_READ;
	a.off = off;
	a.buf = buf;
	a.len = n;
	return z_zar(&a) == Z_OK;
}

static bool zar_child(const char *dir, uint32_t *iter, z_zar_args_t *a) {
	memset(a, 0, sizeof(*a));
	a->op = Z_ZAR_OP_CHILD;
	a->path = dir;
	a->iter = *iter;
	if (z_zar(a) != Z_OK) return false;
	*iter = a->iter;
	return true;
}

// -- paths -------------------------------------------------------------
//
// One namespace: /ram/x rather than a drive letter. /ram and /usb are
// reserved names at the root; /tmp is the RAM disk when there is one,
// and otherwise an ordinary directory on the card, emptied at start.
// Case-insensitive, as FAT names are.

static const struct { const char *prefix; const char *vol; } mounts[] = {
	{ Z_DIR_RAM, "1:" },
	{ Z_DIR_USB, "2:" },
	{ Z_DIR_TMP, "1:" },
};
#define N_MOUNTS	(sizeof(mounts) / sizeof(mounts[0]))

static bool prefix_eq(const char *a, const char *b, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
		if (x != y || !x) return false;
	}
	return true;
}

static bool mount_live(unsigned i) {
	if (mounts[i].vol[0] == '1') return ram_ok;
	return usb_ok;
}

// The FatFs path for `path`: "1:/..." for a mounted prefix, `path`
// itself (drive 0, the card) otherwise. NULL if it does not fit.
static const char *resolve(const char *path, char *buf, uint32_t cap) {
	for (unsigned i = 0; i < N_MOUNTS; i++) {
		const char *p = mounts[i].prefix;
		uint32_t n = (uint32_t)strlen(p);
		if (!prefix_eq(path, p, n)) continue;
		if (path[n] != 0 && path[n] != '/') continue;
		if (!ram_ok && !strcmp(p, Z_DIR_TMP)) continue;	// the card's /tmp
		const char *rest = path[n] ? path + n : "/";
		if (strlen(rest) + 3 > cap) return NULL;
		strcpy(buf, mounts[i].vol);
		strcat(buf, rest);
		return buf;
	}
	return path;
}

static void layout_prepare(void);

// The card, mounted if it can be: once at start, and again on demand
// (a card inserted later, or slow to wake), at most once a second.
static bool card_ensure(void) {
	if (card_ok) return true;
	if (card_held) return false;
	if (card_tried && now_ticks() - card_tried < FS_RETRY_TICKS) return false;
	card_tried = now_ticks();
	if (!card_tried) card_tried = 1;
	if (f_mount(&vol_card, "", 1) != FR_OK) return false;
	card_ok = true;
	layout_prepare();
	return true;
}

// Mount the card if this path needs it. Harmless for the others.
static void need(const char *real) {
	if (fs_vol_of(real) == 0) card_ensure();
}

static int32_t fs_err(FRESULT r) {
	switch (r) {
	case FR_OK:				return 0;
	case FR_NO_FILE:
	case FR_NO_PATH:		return -Z_ENOENT;
	case FR_INVALID_NAME:	return -Z_EINVAL;
	case FR_DENIED:			return -Z_EACCES;
	case FR_EXIST:			return -Z_EEXIST;
	case FR_WRITE_PROTECTED: return -Z_EROFS;
	case FR_NOT_READY:
	case FR_NOT_ENABLED:
	case FR_NO_FILESYSTEM:	return -Z_ENODEV;
	case FR_TOO_MANY_OPEN_FILES: return -Z_EMFILE;
	case FR_LOCKED:			return -Z_EBUSY;
	default:				return -Z_EIO;
	}
}

static void fs_info_of(const FILINFO *fi, z_fs_info_t *out) {
	memset(out, 0, sizeof(*out));
	out->size = (fi->fattrib & AM_DIR) ? 0 : (uint32_t)fi->fsize;
	out->fdate = fi->fdate;
	out->ftime = fi->ftime;
	out->attr = fi->fattrib & 0x3f;
	out->type = (fi->fattrib & AM_DIR) ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
}

static bool vol_busy(int vol) {
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (files[i].used && !files[i].zar && files[i].vol == vol) return true;
	for (int i = 0; i < FS_MAX_DIRS; i++)
		if (dirs[i].used && dirs[i].on_card &&
			dirs[i].d.obj.fs == (vol == 0 ? &vol_card : vol == 1 ? &vol_ram : &vol_usb))
			return true;
	return false;
}

// -- the volumes -------------------------------------------------------

// /sys, /home and /data on the card (docs/layout.md), and -- with no
// RAM disk -- /tmp, emptied once per start, files two levels deep.
static char tmp_path[FS_PATH];
static bool tmp_cleared;

static void tmp_clear(int depth) {
	DIR d;
	size_t len = strlen(tmp_path);
	if (f_opendir(&d, tmp_path) != FR_OK) return;
	while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
		if (len + 1 + strlen(fno.fname) + 1 > sizeof(tmp_path)) continue;
		tmp_path[len] = '/';
		strcpy(tmp_path + len + 1, fno.fname);
		if ((fno.fattrib & AM_DIR) && depth > 1) tmp_clear(depth - 1);
		f_unlink(tmp_path);
		tmp_path[len] = 0;
	}
	f_closedir(&d);
}

static void layout_prepare(void) {
	f_mkdir(Z_DIR_SYS);
	f_mkdir(Z_DIR_HOME);
	f_mkdir(Z_DIR_DATA);
	if (ram_ok) return;
	f_mkdir(Z_DIR_TMP);
	if (tmp_cleared) return;
	tmp_cleared = true;
	strcpy(tmp_path, Z_DIR_TMP);
	tmp_clear(2);
}

static bool ram_format(void) {
	MKFS_PARM opt;
	z_blk_args_t a;
	ram_ok = false;
	if (!blk_info(Z_BLK_RAM, &a) || !(a.flags & Z_BLK_PRESENT)) return false;
	ram_sectors = a.sectors;
	memset(&opt, 0, sizeof(opt));
	opt.fmt = FM_FAT;			// FAT16: no FAT32 reserved-sector overhead
	opt.n_fat = 1;				// no redundancy worth having in RAM
	opt.au_size = 4096;
	if (f_mkfs("1:", &opt, work, sizeof work) != FR_OK) return false;
	if (f_mount(&vol_ram, "1:", 1) != FR_OK) return false;
	ram_ok = true;
	return true;
}

static void usb_release(void) {
	if (!usb_ok) return;
	f_mount(NULL, "2:", 0);
	usb_ok = false;
}

// Bring /usb in line with what is plugged in: released when its drive
// goes (or is replaced), mounted when one arrives ready. Cheap when
// nothing changed. `usb` brings a new drive to ready itself; only
// `usbmount` (force) has fs wait for that (MSC_START).
static void usb_poll(bool force) {
	z_blk_args_t a;
	z_blk_args_t s;
	if (!blk_info(Z_BLK_USB, &a)) return;
	bool present = (a.flags & Z_BLK_PRESENT) != 0;
	if (usb_ok && (!present || a.gen != usb_gen)) {
		usb_release();
		say("fs: usb storage removed, " Z_DIR_USB " released\n");
	}
	if (!present) {
		usb_held = false;
		usb_failed_gen = 0;
		return;
	}
	if (usb_ok || usb_held) return;
	if (!force && usb_failed_gen == a.gen + 1) return;	// tried this one
	// Not ready yet: `usb` is bringing it up, a TEST UNIT READY at a
	// time, and says so (Z_USB_EVENT) once it is. Waiting here instead
	// would hold up every client of fs for as long as a slow drive
	// takes. `usbmount` (force) still asks for the whole wait.
	if (!force && !(a.flags & Z_BLK_READY)) return;
	memset(&s, 0, sizeof(s));
	s.op = Z_BLK_OP_START;
	s.dev = Z_BLK_USB;
	if (!blk(&s) || f_mount(&vol_usb, "2:", 1) != FR_OK) {
		usb_failed_gen = a.gen + 1;
		say("fs: usb storage: mount failed\n");
		return;
	}
	blk_info(Z_BLK_USB, &a);
	usb_ok = true;
	usb_gen = a.gen;
	usb_sectors = a.sectors;
	say("fs: usb storage: ");
	say_num(usb_sectors / 2);
	say(" KB at " Z_DIR_USB "\n");
}

// -- clients -----------------------------------------------------------

static bool gone(uint32_t pid);

// A slot can be given to another client once its own has copied its
// last reply: it has sent something since (a client copies before it
// sends again), or it has died, or 256 requests have been served since,
// far longer than a client takes to run. Before this, a seventh client
// could take the slot of a reply nobody had read yet.
static bool slot_free(const fs_client_t *c) {
	return !c->pid || !c->unread || client_clock - c->last > 256 ||
		gone(c->pid - 1);
}

// This client's reply slot, or NULL if every slot holds a reply its
// client has not copied yet -- it is then told to try again
// (-Z_EAGAIN, fs_serve_msg()).
static fs_client_t *client_of(uint32_t pid) {
	fs_client_t *c = NULL, *old = NULL;
	for (int i = 0; i < FS_MAX_CLIENTS; i++)
		if (clients[i].pid == pid + 1) { c = &clients[i]; break; }
	if (!c) {
		// The least recently served of those free to take.
		for (int i = 0; i < FS_MAX_CLIENTS; i++)
			if ((!old || clients[i].last < old->last) && slot_free(&clients[i]))
				old = &clients[i];
		if (!old) return NULL;
		c = old;
		c->pid = pid + 1;
	}
	c->unread = false;		// it has sent again: it has its last reply
	c->last = ++client_clock;
	c->rep = &c->small.h;
	c->data = c->small.data;
	c->rep->len = 0;
	c->rep->flags = 0;
	if (bulk_owner == pid + 1) bulk_owner = 0;	// it has read its last reply
	return c;
}

static fs_file_t *file_of(int32_t fd, uint32_t pid) {
	if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used || files[fd].owner != pid)
		return NULL;
	return &files[fd];
}

static fs_dir_t *dir_of(int32_t dd, uint32_t pid) {
	if (dd < 0 || dd >= FS_MAX_DIRS || !dirs[dd].used || dirs[dd].owner != pid)
		return NULL;
	return &dirs[dd];
}

static void put_info(fs_client_t *c, const z_fs_info_t *fi) {
	memcpy(c->data, fi, sizeof(*fi));
	c->rep->len = sizeof(*fi);
}

// -- files -------------------------------------------------------------

static int32_t op_open(const z_fsreq_t *rq, uint32_t pid, fs_client_t *c) {
	char rb[FS_PATH];
	const char *real = resolve(rq->path, rb, sizeof rb);
	BYTE mode = 0;
	int32_t fd;
	z_fs_info_t fi;

	if (!real) return -Z_ENAMETOOLONG;
	for (fd = 0; fd < FS_MAX_FILES && files[fd].used; fd++) ;
	if (fd == FS_MAX_FILES) return -Z_EMFILE;
	fs_file_t *f = &files[fd];

	switch (rq->flags & Z_O_ACCMODE) {
	case Z_O_RDONLY: mode = FA_READ; break;
	case Z_O_WRONLY: mode = FA_WRITE; break;
	default:         mode = FA_READ | FA_WRITE; break;
	}
	if (rq->flags & Z_O_CREAT) {
		if (rq->flags & Z_O_EXCL) mode |= FA_CREATE_NEW;
		else if (rq->flags & Z_O_TRUNC) mode |= FA_CREATE_ALWAYS;
		else mode |= FA_OPEN_ALWAYS;
	}

	need(real);
	FRESULT r = f_open(&f->f, real, mode);
	if (r != FR_OK) {
		// Not on the card -- or no card -- and not /ram or /usb: the
		// file in flash at that path, if there is one, for reading.
		uint32_t zo, zs;
		if (mode != FA_READ || real != rq->path || (rq->flags & Z_O_NOFLASH) ||
			!zar_file(rq->path, &zo, &zs))
			return fs_err(r);
		memset(f, 0, sizeof(*f));
		f->used = true;
		f->zar = true;
		f->owner = pid;
		f->zoff = zo;
		f->zsize = zs;
		memset(&fi, 0, sizeof(fi));
		fi.size = zs;
		fi.attr = Z_FS_ATTR_RDONLY;
		fi.type = Z_FS_TYPE_FILE;
		put_info(c, &fi);
		return fd;
	}
	f->zar = false;
	f->vol = (uint8_t)fs_vol_of(real);
	if ((rq->flags & (Z_O_TRUNC | Z_O_CREAT)) == Z_O_TRUNC && (mode & FA_WRITE)) {
		r = f_truncate(&f->f);		// an existing file, emptied
		if (r != FR_OK) { f_close(&f->f); return fs_err(r); }
	}
	if (rq->flags & Z_O_APPEND) f_lseek(&f->f, f_size(&f->f));
	// A read handle's cluster map (FatFs fast seek): without it, every
	// backward seek walks the cluster chain from the start, a FAT
	// sector read per step on the card. Read-only handles only: FatFs
	// will not extend a file while a map is in use.
	f->f.cltbl = 0;
	if (mode == FA_READ) {
		f->f.cltbl = f->clmt;
		f->clmt[0] = FS_CLMT;
		if (f_lseek(&f->f, CREATE_LINKMAP) != FR_OK) f->f.cltbl = 0;
	}
	f->used = true;
	f->owner = pid;
	memset(&fi, 0, sizeof(fi));
	fi.size = (uint32_t)f_size(&f->f);
	fi.type = Z_FS_TYPE_FILE;
	put_info(c, &fi);
	return fd;
}

static FRESULT file_read(fs_file_t *f, void *to, uint32_t want, UINT *br) {
	if (f->zar) {
		uint32_t left = f->zsize - f->zpos;
		uint32_t n = want < left ? want : left;
		if (!zar_read(f->zoff + f->zpos, to, n)) return FR_DISK_ERR;
		f->zpos += n;
		*br = n;
		return FR_OK;
	}
	return f_read(&f->f, to, want, br);
}

// Into the client's buffer through the grant window when it can; into
// the reply otherwise. `dst` is the buffer's physical address, as the
// kernel resolved it.
static int32_t op_read(const z_fsreq_t *rq, uint32_t pid, uint8_t *dst,
	uint32_t dstlen, fs_client_t *c) {
	fs_file_t *f = file_of(rq->fd, pid);
	uint32_t want = rq->len, base, size;
	UINT br = 0;
	FRESULT r;
	if (!f) return -Z_EBADF;
	if (dst && want > dstlen) want = dstlen;
	if (want > Z_FS_CHUNK) want = Z_FS_CHUNK;
	if (grants && dst && z_grant_open(pid, &base, &size) == Z_OK) {
		// The kernel resolved the blob to a physical address but did not
		// bound its length; the window does, and so does this, so that a
		// bad buffer is an error for the client rather than the end of fs.
		if (!FS_IN_WINDOW(dst, want, base, size)) {
			z_grant_close();
			return -Z_EINVAL;
		}
		r = file_read(f, dst, want, &br);
		z_grant_close();
	} else {
		// By copy, into the bulk buffer if it is free (or its holder has
		// surely read it), else into this client's own slot: a short
		// count, and the client asks again.
		uint8_t *to = c->data;
		uint32_t cap = sizeof(c->small.data);
		if (!bulk) bulk = malloc(sizeof(*bulk));
		if (bulk && (!bulk_owner ||
			now_ticks() - bulk_when >= FS_BULK_HOLD_TICKS)) {
			bulk_owner = pid + 1;
			bulk_when = now_ticks();
			c->rep = &bulk->h;
			c->rep->flags = 0;
			c->data = to = bulk->data;
			cap = sizeof(bulk->data);
		}
		if (want > cap) want = cap;
		r = file_read(f, to, want, &br);
		c->rep->len = br;
	}
	return r == FR_OK ? (int32_t)br : fs_err(r);
}

static int32_t op_write(const z_fsreq_t *rq, uint32_t pid, const uint8_t *src,
	uint32_t srclen) {
	fs_file_t *f = file_of(rq->fd, pid);
	uint32_t n = rq->len;
	UINT bw = 0;
	if (!f) return -Z_EBADF;
	if (f->zar) return -Z_EROFS;		// files in flash are read-only
	if (!src) return n ? -Z_EINVAL : 0;
	if (n > srclen) n = srclen;
	if (n > Z_FS_CHUNK) n = Z_FS_CHUNK;
	FRESULT r = f_write(&f->f, src, n, &bw);	// read straight from the client
	return r == FR_OK ? (int32_t)bw : fs_err(r);
}

static int32_t op_lseek(const z_fsreq_t *rq, uint32_t pid) {
	fs_file_t *f = file_of(rq->fd, pid);
	int64_t pos, size, here;
	if (!f) return -Z_EBADF;
	size = f->zar ? f->zsize : (int64_t)f_size(&f->f);
	here = f->zar ? f->zpos : (int64_t)f_tell(&f->f);
	switch (rq->whence) {
	case Z_SEEK_SET: pos = rq->off; break;
	case Z_SEEK_CUR: pos = here + rq->off; break;
	case Z_SEEK_END: pos = size + rq->off; break;
	default: return -Z_EINVAL;
	}
	if (pos < 0 || pos > 0x7fffffff) return -Z_EINVAL;
	if (f->zar) {			// as on a read handle: past the end stops there
		f->zpos = (uint32_t)(pos < size ? pos : size);
		return (int32_t)f->zpos;
	}
	FRESULT r = f_lseek(&f->f, (FSIZE_t)pos);
	return r == FR_OK ? (int32_t)f_tell(&f->f) : fs_err(r);
}

static int32_t op_fstat(const z_fsreq_t *rq, uint32_t pid, fs_client_t *c) {
	fs_file_t *f = file_of(rq->fd, pid);
	z_fs_info_t fi;
	if (!f) return -Z_EBADF;
	memset(&fi, 0, sizeof(fi));
	fi.size = f->zar ? f->zsize : (uint32_t)f_size(&f->f);
	fi.attr = f->zar ? Z_FS_ATTR_RDONLY : 0;
	fi.type = Z_FS_TYPE_FILE;
	put_info(c, &fi);
	return 0;
}

static int32_t op_stat(const z_fsreq_t *rq, fs_client_t *c) {
	char rb[FS_PATH];
	const char *real = resolve(rq->path, rb, sizeof rb);
	z_fs_info_t fi;
	if (!real) return -Z_ENAMETOOLONG;
	memset(&fi, 0, sizeof(fi));
	if (fs_is_root(real)) {		// a volume root, or a synthetic one
		if (real != rq->path && !(fs_vol_of(real) == 1 ? ram_ok : usb_ok))
			return -Z_ENOENT;
		fi.attr = Z_FS_ATTR_DIR;
		fi.type = Z_FS_TYPE_DIR;
		put_info(c, &fi);
		return 0;
	}
	need(real);
	if (f_stat(real, &fno) == FR_OK) {
		fs_info_of(&fno, &fi);
		put_info(c, &fi);
		return 0;
	}
	if (real == rq->path) {		// the flash underlay
		uint32_t zo, zs;
		if (zar_file(rq->path, &zo, &zs)) {
			fi.size = zs;
			fi.attr = Z_FS_ATTR_RDONLY;
			fi.type = Z_FS_TYPE_FILE;
			put_info(c, &fi);
			return 0;
		}
		if (zar_isdir(rq->path)) {
			fi.attr = Z_FS_ATTR_DIR | Z_FS_ATTR_RDONLY;
			fi.type = Z_FS_TYPE_DIR;
			put_info(c, &fi);
			return 0;
		}
	}
	return -Z_ENOENT;
}

// Is `real` open for writing? Refused for a rename (zfs.h, FS_RENAME):
// the handle would record its size in the old, deleted entry. A write
// handle keeps the sector and place of its directory entry; opening
// the path read-only fills in the same two -- by position, so a long
// name and its 8.3 alias are the same file. A directory cannot be
// opened as a file, so it is never busy.
static bool write_open(const char *real) {
	bool busy = false;
	if (f_open(&probe, real, FA_READ) != FR_OK) return false;
	for (int i = 0; i < FS_MAX_FILES; i++) {
		const FIL *f = &files[i].f;
		if (!files[i].used || files[i].zar || !(f->flag & FA_WRITE)) continue;
		if (f->obj.fs == probe.obj.fs && f->dir_sect == probe.dir_sect &&
			f->dir_ptr == probe.dir_ptr)
			busy = true;
	}
	f_close(&probe);
	return busy;
}

static int32_t op_rename(const z_fsreq_t *rq) {
	char rf[FS_PATH], rt[FS_PATH];
	const char *from = resolve(rq->path, rf, sizeof rf);
	const char *to = resolve(rq->path2, rt, sizeof rt);
	FRESULT r;
	if (!from || !to) return -Z_ENAMETOOLONG;
	if (!from[0] || !to[0]) return -Z_EINVAL;
	// FatFs would rename within `from`'s drive whatever `to` says.
	if (fs_vol_of(from) != fs_vol_of(to)) return -Z_EXDEV;
	if (fs_is_root(from) || fs_is_root(to) ||
		fs_is_within(fs_vol_strip(from), fs_vol_strip(to)))
		return -Z_EINVAL;
	need(from);
	if (write_open(from)) return -Z_EBUSY;
	r = f_rename(from, fs_vol_strip(to));
	if (r == FR_NO_PATH || r == FR_INVALID_NAME) return -Z_EINVAL;
	return fs_err(r);
}

// -- directories -------------------------------------------------------

// One entry into the page at *at, if it fits.
static bool dir_put(fs_client_t *c, uint32_t page, uint32_t *at, const char *name,
	const z_fs_info_t *fi) {
	uint32_t nl = (uint32_t)strlen(name);
	uint32_t rl = (uint32_t)((sizeof(z_fs_dirent_t) + nl + 1 + 3) & ~3u);
	if (*at + rl > page) return false;
	z_fs_dirent_t *e = (z_fs_dirent_t *)(c->data + *at);
	e->info = *fi;
	e->namelen = (uint16_t)nl;
	e->reclen = (uint16_t)rl;
	memcpy(e->name, name, nl + 1);
	*at += rl;
	return true;
}

// A page is as big as the client asked (len), up to a slot's worth, and
// big enough for at least one long name.
static uint32_t dir_page(const z_fsreq_t *rq) {
	uint32_t min = (sizeof(z_fs_dirent_t) + FF_LFN_BUF + 1 + 3) & ~3u;
	uint32_t p = rq->len ? (rq->len & ~3u) : Z_FS_DIR_BYTES;
	if (p > Z_FS_DIR_BYTES) p = Z_FS_DIR_BYTES;
	if (p < min) p = min;
	return p;
}

static void dir_close(fs_dir_t *d) {
	if (d->on_card) f_closedir(&d->d);
	d->on_card = false;
	d->used = false;
}

// A page of entries into the reply. At the end of the directory it is
// closed here and the reply says so (Z_FSREP_END), which saves the
// client a READDIR and a CLOSEDIR.
static int32_t dir_fill(fs_dir_t *d, fs_client_t *c) {
	uint32_t at = 0;
	int32_t n = 0;
	z_fs_info_t fi;
	char full[FS_PATH];

	while (d->phase < 3) {
		if (d->phase == 0) {			// the volumes off "/"
			if (!d->root || d->mi >= N_MOUNTS) { d->phase = 1; continue; }
			if (mount_live(d->mi)) {
				memset(&fi, 0, sizeof(fi));
				fi.attr = Z_FS_ATTR_DIR;
				fi.type = Z_FS_TYPE_DIR;
				if (!dir_put(c, d->page, &at, mounts[d->mi].prefix + 1, &fi)) break;
				n++;
			}
			d->mi++;
			continue;
		}
		if (d->phase == 1) {			// the card's entries
			if (!d->on_card) { d->phase = 2; continue; }
			// FatFs has no "peek": read one only when it surely fits
			uint32_t worst = (sizeof(z_fs_dirent_t) + FF_LFN_BUF + 1 + 3) & ~3u;
			if (at + worst > d->page) break;
			FRESULT r = f_readdir(&d->d, &fno);
			if (r != FR_OK || !fno.fname[0]) {
				f_closedir(&d->d);
				d->on_card = false;
				d->phase = 2;
				if (r != FR_OK && !n && !d->under) return fs_err(r);
				continue;
			}
			fs_info_of(&fno, &fi);
			dir_put(c, d->page, &at, fno.fname, &fi);
			n++;
			continue;
		}
		// phase 2: in flash under this path and not on the card
		if (!d->under) { d->phase = 3; continue; }
		{
			z_zar_args_t za;
			uint32_t it = d->zit;
			if (!zar_child(d->path, &it, &za)) { d->phase = 3; continue; }
			size_t pl = strlen(d->path);
			while (pl && d->path[pl - 1] == '/') pl--;
			if (pl + 1 + strlen(za.name) + 1 <= sizeof(full)) {
				memcpy(full, d->path, pl);
				full[pl] = '/';
				strcpy(full + pl + 1, za.name);
				if (card_ok && f_stat(full, &fno) == FR_OK) { d->zit = it; continue; }
			}
			memset(&fi, 0, sizeof(fi));
			fi.size = za.is_dir ? 0 : za.size;
			fi.attr = za.is_dir ? (Z_FS_ATTR_DIR | Z_FS_ATTR_RDONLY) : Z_FS_ATTR_RDONLY;
			fi.type = za.is_dir ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
			if (!dir_put(c, d->page, &at, za.name, &fi)) break;	// it again, next page
			d->zit = it;
			n++;
		}
	}
	c->rep->len = at;
	if (d->phase >= 3) {
		dir_close(d);
		c->rep->flags |= Z_FSREP_END;
	}
	return n;
}

// The directory, and its first page of entries in the same reply.
static int32_t op_opendir(const z_fsreq_t *rq, uint32_t pid, fs_client_t *c) {
	char rb[FS_PATH];
	const char *path = rq->path[0] ? rq->path : "/";
	const char *real = resolve(path, rb, sizeof rb);
	int32_t dd;
	if (!real) return -Z_ENAMETOOLONG;
	if (strlen(path) >= FS_PATH) return -Z_ENAMETOOLONG;
	for (dd = 0; dd < FS_MAX_DIRS && dirs[dd].used; dd++) ;
	if (dd == FS_MAX_DIRS) return -Z_EMFILE;
	fs_dir_t *d = &dirs[dd];
	memset(d, 0, sizeof(*d));
	d->under = (real == path);
	d->root = (path[0] == '/' && path[1] == 0);
	d->page = dir_page(rq);
	strcpy(d->path, path);
	need(real);
	FRESULT r = f_opendir(&d->d, real);
	d->on_card = (r == FR_OK);
	// A board with no card still lists "/", "/apps" and "/docs".
	if (!d->on_card && !(d->under && (d->root || zar_isdir(path))))
		return fs_err(r);
	d->used = true;
	d->owner = pid;
	int32_t n = dir_fill(d, c);
	if (n < 0) {
		if (d->used) dir_close(d);
		return n;
	}
	return dd;
}

static int32_t op_readdir(const z_fsreq_t *rq, uint32_t pid, fs_client_t *c) {
	fs_dir_t *d = dir_of(rq->fd, pid);
	if (!d) return -Z_EBADF;
	d->page = dir_page(rq);
	return dir_fill(d, c);
}

// -- volumes -----------------------------------------------------------

static int vol_of_path(const char *path) {
	char rb[FS_PATH];
	const char *real = resolve(path[0] ? path : "/", rb, sizeof rb);
	return real ? fs_vol_of(real) : -1;
}

static int32_t op_statvfs(const z_fsreq_t *rq, fs_client_t *c) {
	static const char *const vn[3] = { "0:", "1:", "2:" };
	FATFS *fsp;
	DWORD fre;
	z_fs_vfs_t out;
	int v = vol_of_path(rq->path);
	if (v < 0) return -Z_EINVAL;
	if (v == 0 && !card_ensure()) return -Z_ENODEV;
	FRESULT r = f_getfree(vn[v], &fre, &fsp);
	if (r != FR_OK) return fs_err(r);
	out.total_kb = (uint32_t)((fsp->n_fatent - 2) * fsp->csize / 2);
	out.free_kb = (uint32_t)(fre * fsp->csize / 2);
	memcpy(c->data, &out, sizeof(out));
	c->rep->len = sizeof(out);
	return 0;
}

static int32_t op_mkfs(const z_fsreq_t *rq) {
	int v = vol_of_path(rq->path);
	if (v < 0) return -Z_EINVAL;
	if (vol_busy(v)) return -Z_EBUSY;
	if (v == 1) return ram_format() ? 0 : -Z_EIO;
	if (v == 2) {
		if (!usb_ok) return -Z_ENODEV;
		FRESULT r = f_mkfs("2:", 0, work, sizeof work);
		if (r == FR_OK) r = f_mount(&vol_usb, "2:", 1);
		return fs_err(r);
	}
	// The card: unmounted for the format, mounted fresh after.
	f_mount(NULL, "", 0);
	card_ok = false;
	FRESULT r = f_mkfs("", 0, work, sizeof work);
	card_held = false;
	card_tried = 0;
	tmp_cleared = true;			// nothing on a new card to empty
	if (r != FR_OK) return fs_err(r);
	return card_ensure() ? 0 : -Z_EIO;
}

static int32_t op_mount(const z_fsreq_t *rq) {
	int v = vol_of_path(rq->path);
	if (v == 0) {
		card_held = false;
		card_tried = 0;
		return card_ensure() ? 0 : -Z_ENODEV;
	}
	if (v == 2) {
		usb_held = false;
		usb_poll(true);
		return usb_ok ? 0 : -Z_ENODEV;
	}
	return v == 1 && ram_ok ? 0 : -Z_ENODEV;
}

static int32_t op_umount(const z_fsreq_t *rq) {
	z_blk_args_t a;
	int v = vol_of_path(rq->path);
	if (v < 0 || v == 1) return -Z_EINVAL;
	if (vol_busy(v)) return -Z_EBUSY;
	if (v == 0) {
		if (card_ok) f_mount(NULL, "", 0);
		card_ok = false;
		card_held = true;
		return 0;
	}
	usb_release();
	usb_held = blk_info(Z_BLK_USB, &a) && (a.flags & Z_BLK_PRESENT);
	return 0;
}

static int32_t op_syncall(void) {
	int32_t n = 0;
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (files[i].used && !files[i].zar && (files[i].f.flag & FA_WRITE)) {
			f_sync(&files[i].f);
			n++;
		}
	return n;
}

// -- dispatch ----------------------------------------------------------

static int32_t serve(const z_fsreq_t *rq, uint32_t pid, uint8_t *buf,
	uint32_t buflen, fs_client_t *c) {
	char rb[FS_PATH];
	const char *real;
	fs_file_t *f;
	fs_dir_t *d;

	switch (rq->op) {
	case Z_FSOP_OPEN:	return op_open(rq, pid, c);
	case Z_FSOP_CLOSE: {
		if (!(f = file_of(rq->fd, pid))) return -Z_EBADF;
		f->used = false;
		return f->zar ? 0 : fs_err(f_close(&f->f));
	}
	case Z_FSOP_READ:	return op_read(rq, pid, buf, buflen, c);
	case Z_FSOP_WRITE:	return op_write(rq, pid, buf, buflen);
	case Z_FSOP_LSEEK:	return op_lseek(rq, pid);
	case Z_FSOP_FSTAT:	return op_fstat(rq, pid, c);
	case Z_FSOP_STAT:	return op_stat(rq, c);
	case Z_FSOP_UNLINK: {
		FRESULT fr;
		FILINFO fno;
		if (!(real = resolve(rq->path, rb, sizeof rb))) return -Z_ENAMETOOLONG;
		need(real);
		fr = f_unlink(real);
		// FatFs says DENIED for a directory with something in it as
		// for a read-only file; POSIX's rmdir() says ENOTEMPTY
		// (sw/common/zposix.c).
		if (fr == FR_DENIED && f_stat(real, &fno) == FR_OK && (fno.fattrib & AM_DIR))
			return -Z_ENOTEMPTY;
		return fs_err(fr);
	}
	case Z_FSOP_MKDIR:
		if (!(real = resolve(rq->path, rb, sizeof rb))) return -Z_ENAMETOOLONG;
		need(real);
		return fs_err(f_mkdir(real));
	case Z_FSOP_RENAME:		return op_rename(rq);
	case Z_FSOP_OPENDIR:	return op_opendir(rq, pid, c);
	case Z_FSOP_READDIR:	return op_readdir(rq, pid, c);
	case Z_FSOP_CLOSEDIR:
		if (!(d = dir_of(rq->fd, pid))) return -Z_EBADF;
		dir_close(d);
		return 0;
	case Z_FSOP_FSYNC:
		if (!(f = file_of(rq->fd, pid))) return -Z_EBADF;
		return f->zar ? 0 : fs_err(f_sync(&f->f));
	case Z_FSOP_FTRUNCATE: {
		if (!(f = file_of(rq->fd, pid))) return -Z_EBADF;
		if (f->zar) return -Z_EROFS;
		if (rq->off < 0) return -Z_EINVAL;
		FSIZE_t here = f_tell(&f->f);
		FRESULT r = f_lseek(&f->f, (FSIZE_t)rq->off);
		// a short seek: the card ran out growing it
		if (r == FR_OK && f_tell(&f->f) != (FSIZE_t)rq->off) return -Z_ENOSPC;
		if (r == FR_OK) r = f_truncate(&f->f);
		if (r == FR_OK && here < (FSIZE_t)rq->off) r = f_lseek(&f->f, here);
		return fs_err(r);
	}
	case Z_FSOP_STATVFS:	return op_statvfs(rq, c);
	case Z_FSOP_MKFS:		return op_mkfs(rq);
	case Z_FSOP_MOUNT:		return op_mount(rq);
	case Z_FSOP_UMOUNT:		return op_umount(rq);
	case Z_FSOP_SYNCALL:	return op_syncall();
	default:				return -Z_EINVAL;
	}
}

// A client that has gone has left its descriptors open; they are closed
// once it no longer exists. Checked now and then, not per request.
static bool gone(uint32_t pid) {
	uint32_t st = Z_PROC_STATE_UNKNOWN;
	z_proc_status(pid, &st, NULL);
	return st != Z_PROC_STATE_RUNNING;
}

static void sweep_dead(void) {
	if (bulk_owner && gone(bulk_owner - 1)) bulk_owner = 0;
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (files[i].used && gone(files[i].owner)) {
			if (!files[i].zar) f_close(&files[i].f);
			files[i].used = false;
		}
	for (int i = 0; i < FS_MAX_DIRS; i++)
		if (dirs[i].used && gone(dirs[i].owner)) dir_close(&dirs[i]);
}

// The answer to a client with no slot free: try again (zfsc.c does).
// The same few bytes for everyone, never written, so sharing is safe.
static z_fsrep_t busy_rep = { -Z_EAGAIN, 0, 0 };
static z_blob_t busy_hdr = { sizeof(z_fsrep_t), (uint8_t *)&busy_rep };

// One request, served and answered.
static void fs_serve_msg(z_msg_t *m) {
	fs_client_t *c = client_of(m->from);
	z_obj_t reply;
	if (!c) {
		reply.type = Z_BLOB;
		reply.val.ptr = &busy_hdr;
		z_msg_new_send(m->from, Z_FS_REPLY, m->tag, reply);
		return;
	}
	z_obj_t *o0 = z_list_get(&m->obj, 0), *o1 = z_list_get(&m->obj, 1);
	const z_fsreq_t *rq = o0 ? (const z_fsreq_t *)z_blob_data(o0) : NULL;
	if (!rq || z_blob_len(o0) < sizeof(z_fsreq_t) ||
		!memchr(rq->path, 0, sizeof(rq->path)) || !memchr(rq->path2, 0, sizeof(rq->path2))) {
		c->rep->result = -Z_EINVAL;
	} else {
		uint8_t *buf = o1 ? (uint8_t *)z_blob_data(o1) : NULL;
		uint32_t blen = o1 ? z_blob_len(o1) : 0;
		// Not `c->rep->result = serve(...)`: serve() may point c->rep
		// at the bulk buffer, and the left side can be evaluated first.
		int32_t result = serve(rq, m->from, buf, blen, c);
		c->rep->result = result;
	}

	c->hdr.len = (uint32_t)sizeof(z_fsrep_t) + c->rep->len;
	c->hdr.data = (uint8_t *)c->rep;
	c->unread = true;
	reply.type = Z_BLOB;
	reply.val.ptr = &c->hdr;
	z_msg_new_send(m->from, Z_FS_REPLY, m->tag, reply);
}

// The RAM disk at start: the filesystem already on it if there is one,
// else a fresh one. The kernel clears its first sector at boot
// (sw/os/kblk.c), so after a boot there is none and it is formatted, as
// it always was; after a restart of fs (a crash, docs/fs_driver.md) the
// one fs left is mounted, and /ram keeps its files.
static bool ram_start(bool *kept) {
	z_blk_args_t a;
	*kept = false;
	if (!blk_info(Z_BLK_RAM, &a) || !(a.flags & Z_BLK_PRESENT)) return false;
	ram_sectors = a.sectors;
	if (f_mount(&vol_ram, "1:", 1) == FR_OK) {
		ram_ok = true;
		*kept = true;
		return true;
	}
	return ram_format();
}

// The volumes, at start.
static void fs_start(void) {
	bool kept;
	if (ram_start(&kept)) {
		say("fs: ramdisk: ");
		say_num(ram_sectors / 2);
		say(kept ? " KB at " Z_DIR_RAM " (and " Z_DIR_TMP "), kept from before\n"
			: " KB at " Z_DIR_RAM " (and " Z_DIR_TMP ")\n");
	}
	// One attempt: a board with no card must not wait for one. A card
	// that was slow to wake is mounted by the first request that needs
	// it (card_ensure()).
	if (card_ensure()) say("fs: sdcard mounted\n");
	else say("fs: no sdcard (yet)\n");
	usb_poll(false);
}

#ifndef FS_HOST_TEST
int main(void) {
	char name[16];

	fs_start();

	// Can this process write a client's buffer through the grant
	// window? Only a driver on gateware with one; the answer for a
	// particular client is still that client's (zfsc.c opts in).
	grants = (z_mpu_version() >= 2);

	if (!z_pid_register("fs", name, sizeof name)) {
		say("fs: cannot register\n");
		return 1;
	}
	say("fs: ");
	say(name);
	say(grants ? " ready, reads straight into clients\n" :
		" ready, reads by copy (no grant window)\n");

	// Idle wakes, once a second: USB plugs, and every fourth, the
	// descriptors of clients that died. Every 256 requests too, for a
	// fs that is never idle.
	uint32_t served = 0, idle = 0;
	for (;;) {
		z_msg_t m;
		if (z_msg_recv(&m, FS_IDLE_TICKS) != Z_OK) {
			usb_poll(false);
			if ((++idle & 3u) == 0) sweep_dead();
			continue;
		}
		// `usb` says a drive came or went (zusbproto.h).
		if (m.subject == Z_USB_EVENT) { usb_poll(false); continue; }
		if (m.subject != Z_FS_REQ) continue;
		fs_serve_msg(&m);
		if ((++served & 255u) == 0) sweep_dead();
	}
}
#endif

#ifdef FS_HOST_TEST
// For tests/test_fs.c: the server without its loop.
void fs_test_start(bool with_grants) { grants = with_grants; fs_start(); }
void fs_test_serve(z_msg_t *m) { fs_serve_msg(m); }
void fs_test_sweep(void) { sweep_dead(); }
void fs_test_usb_poll(void) { usb_poll(false); }
#endif
