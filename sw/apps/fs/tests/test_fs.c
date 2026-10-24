/*
 * Host test for the filesystem driver and its client: sw/apps/fs/fs.c
 * serving requests made by sw/common/zfsc.c, and zfsapp.c's routing to
 * it, on the project's own FatFs. docs/fs_driver.md.
 *
 *   make -C sw/apps/fs/tests && sw/apps/fs/tests/test_fs
 *
 * The kernel is replaced by stubs: a message layer that delivers a
 * client's request straight to fs_serve_msg() and hands the reply back
 * to the client's z_msg_recv_match(); Z_SYS_BLK over a RAM disk;
 * Z_SYS_ZAR over a small made-up archive; and the card's driver
 * (sd_disk_*) over another RAM disk. Pointers need no translation on
 * the host, so what is tested is the protocol and fs itself: the card,
 * /ram and /tmp, the flash underlay and its shadowing, listings with
 * the volumes at the root, both read paths, rename's checks, volume
 * operations, descriptor ownership and the sweep, and how many
 * requests things take. Built with AddressSanitizer and UBSan's
 * alignment check: x86 forgives a misaligned load, the board does not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zproc.h"
#include "zfsc.h"
#include "zfsapp.h"
#include "zrtc.h"
#include "zusbc.h"
#include "../fatfs/ff.h"
#include "../fatfs/diskio.h"

void fs_test_start(bool with_grants);
void fs_test_serve(z_msg_t *m);
void fs_test_sweep(void);
void fs_test_usb_poll(void);
void fs_test_nfs(bool up);
void fs_test_kick(void);
#define NET_PID		9
static bool in_fs;				// fs is serving: what it sends is fs's
static uint32_t nfs_events, nfs_event_val;
static bool net_pump(void);
#include "../../../common/znfsproto.h"
void nfs_handle_msg(const z_msg_t *msg);

#define FS_PID		7
static uint32_t client = 5;		// who is calling
static bool alive[64];
static bool grant_allowed = true;
static bool grant_open_now;
static uint32_t grant_opens, grant_misuse;
static uint32_t reqs;			// requests sent to fs
#ifdef ZFSC_KERNEL
volatile uint32_t z_pid;		// zfsc.c buffers writes for pid 0 only
#endif
static uint32_t ticks;

static bool have_reply;
static z_msg_t reply;
static bool hold_next;			// withhold the next reply (test_stale())
static z_msg_t held;
static uint32_t takes;

static int fails, checks;
static bool watch_mailbox_full;
static uint32_t changed_n, changed_to, changed_tag;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// -- the kernel, as far as fs and zfsc need it ---------------------------

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	z_msg_t m;
	memset(&m, 0, sizeof m);
	m.to = to; m.subject = subject; m.tag = tag; m.obj = obj;
	if (to == NET_PID) {			// net's NFS client (nfs_fake.h)
		CHECK(subject == Z_NET_NFS_REQ);
		m.from = in_fs ? FS_PID : client;
		nfs_handle_msg(&m);
		return Z_OK;
	}
	if (subject == Z_NET_NFS_EVENT) {	// net to fs
		CHECK(to == FS_PID && obj.type == Z_UINT32);
		nfs_events++;
		nfs_event_val = obj.val.uint32;
		fs_test_nfs(obj.val.uint32 != 0);
		return Z_OK;
	}
	if (subject == Z_NET_NFS_REPLY) {	// net's answer, to fs or the "shell"
		m.from = NET_PID;
		reply = m;
		have_reply = true;
		return Z_OK;
	}
	if (to == FS_PID) {
		CHECK(subject == Z_FS_REQ);
		reqs++;
		m.from = client;
		in_fs = true;
		fs_test_serve(&m);
		in_fs = false;
		return Z_OK;
	}
	if (subject == Z_FS_CHANGED) {	// fs to a watcher (test_watch())
		CHECK(obj.type == Z_NONE);
		if (watch_mailbox_full) return Z_FAIL;
		changed_n++;
		changed_to = to;
		changed_tag = tag;
		return Z_OK;
	}
	CHECK(to == client && subject == Z_FS_REPLY);
	CHECK(!grant_open_now);			// fs closed the window before replying
	// the reply is a word-aligned blob in fs's memory
	CHECK(obj.type == Z_BLOB && ((uintptr_t)((z_blob_t *)obj.val.ptr)->data & 3u) == 0);
	m.from = FS_PID;
	if (hold_next) {			// this one comes late (test_stale())
		held = m;
		hold_next = false;
		return Z_OK;
	}
	reply = m;
	have_reply = true;
	return Z_OK;
}

// zfsc's pause before asking again (-Z_EAGAIN). The first one lets
// the client test_slots() chose die, which frees its slot.
static uint32_t waits, die_on_wait, tick_on_wait;
void k_proc_wait_ticks(uint32_t t);
void z_proc_wait(uint32_t t) {
	waits++;
	if (tick_on_wait) ticks += t;
	if (die_on_wait) { alive[die_on_wait] = false; die_on_wait = 0; }
}

void k_proc_wait_ticks(uint32_t t) { z_proc_wait(t); }

z_rv z_msg_take_match(z_msg_t *msg, uint32_t subject, uint32_t tag) {
	takes++;
	if (!have_reply || reply.subject != subject || reply.tag != tag) return Z_FAIL;
	*msg = reply;
	have_reply = false;
	return Z_OK;
}

z_rv z_msg_recv_match(z_msg_t *msg, uint32_t subject, uint32_t tag, uint32_t t) {
	(void)t;
	while (!have_reply && net_pump()) { }	// net works while fs waits for it
	if (!have_reply || reply.subject != subject || reply.tag != tag) return Z_FAIL;
	*msg = reply;
	have_reply = false;
	return Z_OK;
}

bool z_pid_lookup(const char *name, uint32_t *pid) {
	if (!strcmp(name, "net0")) { *pid = NET_PID; return true; }
	if (strcmp(name, Z_FS_SERVICE)) return false;
	*pid = FS_PID;
	return true;
}

z_rv z_grant_drivers(bool on) { (void)on; return Z_OK; }
z_rv z_grant_open(uint32_t pid, uint32_t *base, uint32_t *size) {
	if (grant_open_now) grant_misuse++;
	if (pid != client || !grant_allowed) return Z_FAIL;
	grant_open_now = true;
	grant_opens++;
	*base = 0; *size = 0xffffffffu;
	return Z_OK;
}
z_rv z_grant_close(void) {
	if (!grant_open_now) grant_misuse++;
	grant_open_now = false;
	return Z_OK;
}

// Z_SYS_PROC_SERIAL: a dead pid has none; respawn() starts another
// program at a pid, as the kernel does with the lowest free one.
static uint32_t generation[64];
uint32_t z_proc_serial(uint32_t pid) {
	return (pid < 64 && alive[pid]) ? pid * 1000u + generation[pid] + 1u : 0;
}
static void respawn(uint32_t pid) { generation[pid]++; alive[pid] = true; }

z_rv z_proc_status(uint32_t pid, uint32_t *state, int32_t *status) {
	(void)status;
	if (state) *state = (pid < 64 && alive[pid]) ? Z_PROC_STATE_RUNNING : Z_PROC_STATE_EXITED;
	return Z_OK;
}

uint32_t z_uptime_ticks(void) { return ticks; }

z_obj_t *z_list_get(z_obj_t *obj, uint32_t i) {
	if (!obj || obj->type != Z_LIST) return NULL;
	z_obj_table_t *t = obj->val.ptr;
	return i < t->len ? &t->a[i] : NULL;
}
uint32_t z_blob_len(const z_obj_t *o) {
	return (o && o->type == Z_BLOB && o->val.ptr) ? ((z_blob_t *)o->val.ptr)->len : 0;
}
void *z_blob_data(const z_obj_t *o) {
	return (o && o->type == Z_BLOB && o->val.ptr) ? ((z_blob_t *)o->val.ptr)->data : NULL;
}
z_obj_t z_obj_uint32(uint32_t u) { z_obj_t o; memset(&o, 0, sizeof o); o.type = Z_UINT32; o.val.uint32 = u; return o; }
z_obj_t z_obj_none(void) { z_obj_t o; memset(&o, 0, sizeof o); o.type = Z_NONE; return o; }

#include "nfs_fake.h"

// -- the disks -----------------------------------------------------------

#define CARD_SECTORS	8192		// 4MB
#define RAM_SECTORS		2048		// 1MB
static uint8_t *card, *ram;

DSTATUS sd_disk_status(BYTE d) { (void)d; return 0; }
DSTATUS sd_disk_initialize(BYTE d) { return sd_disk_status(d); }
DRESULT sd_disk_read(BYTE d, BYTE *b, LBA_t s, UINT n) {
	(void)d;
	if (s + n > CARD_SECTORS) return RES_PARERR;
	memcpy(b, card + s * 512, n * 512);
	return RES_OK;
}
DRESULT sd_disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n) {
	(void)d;
	if (s + n > CARD_SECTORS) return RES_PARERR;
	memcpy(card + s * 512, b, n * 512);
	return RES_OK;
}
DRESULT sd_disk_ioctl(BYTE d, BYTE cmd, void *buf) {
	(void)d;
	switch (cmd) {
	case CTRL_SYNC: return RES_OK;
	case GET_SECTOR_COUNT: *(LBA_t *)buf = CARD_SECTORS; return RES_OK;
	case GET_SECTOR_SIZE: *(WORD *)buf = 512; return RES_OK;
	case GET_BLOCK_SIZE: *(DWORD *)buf = 1; return RES_OK;
	}
	return RES_PARERR;
}

z_rv z_blk(z_blk_args_t *a) {
	if (a->dev != Z_BLK_RAM) return Z_FAIL;	// the kernel refuses USB now
	switch (a->op) {
	case Z_BLK_OP_INFO:
		a->sectors = RAM_SECTORS; a->gen = 1;
		a->flags = Z_BLK_PRESENT | Z_BLK_READY;
		return Z_OK;
	case Z_BLK_OP_START: return Z_OK;
	case Z_BLK_OP_READ:
		if (!a->count || a->lba + a->count > RAM_SECTORS) return Z_FAIL;
		memcpy(a->buf, ram + a->lba * 512, a->count * 512);
		return Z_OK;
	case Z_BLK_OP_WRITE:
		if (!a->count || a->lba + a->count > RAM_SECTORS) return Z_FAIL;
		memcpy(ram + a->lba * 512, a->buf, a->count * 512);
		return Z_OK;
	}
	return Z_FAIL;
}

// -- the `usb` driver's drive (sw/common/zusbc.h), plugged in by tests --

// Overridable: sw/apps/backup/tests/test_fat.c includes this file for its
// kernel stubs, with a drive large enough for FAT32 and a hook that can
// cut the power at a sector write.
#ifndef USB_SECTORS
#define USB_SECTORS 2048
#endif
static uint8_t *usbd;
static bool usb_present, usb_ready;
static uint32_t usb_gen, usb_starts, usb_reads;

bool zusb_msc_info(z_usb_msc_info_t *mi) {
	memset(mi, 0, sizeof(*mi));
	mi->present = usb_present;
	mi->ready = usb_present && usb_ready;
	mi->sectors = mi->ready ? USB_SECTORS : 0;
	mi->gen = usb_gen;
	return true;
}
int32_t zusb_msc_start(void) {
	usb_starts++;
	if (!usb_present) return -Z_ENODEV;
	usb_ready = true;
	return 0;
}
int32_t zusb_msc_read(uint32_t lba, uint32_t count, void *buf) {
	if (!usb_present || !usb_ready || lba + count > USB_SECTORS) return -Z_EIO;
	usb_reads++;
	memcpy(buf, usbd + lba * 512, count * 512);
	return 0;
}
int32_t zusb_msc_write(uint32_t lba, uint32_t count, const void *buf) {
	if (!usb_present || !usb_ready || lba + count > USB_SECTORS) return -Z_EIO;
#ifdef USB_WRITE_HOOK
	if (!USB_WRITE_HOOK(lba, count, buf)) return -Z_EIO;
#endif
	memcpy(usbd + lba * 512, buf, count * 512);
	return 0;
}

// -- a flash archive: /apps/wm, /apps/term, /docs/welcome.txt ------------

static const struct { const char *path; const char *data; } zar_files[] = {
	{ "/apps/wm",          "WMWMWMWMWMWMWMWMWMWM" },
	{ "/apps/term",        "TERMTERM" },
	{ "/docs/welcome.txt", "Welcome to Zeitlos, from flash.\n" },
};
#define N_ZAR (sizeof(zar_files) / sizeof(zar_files[0]))

static bool ieq(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return false;
	}
	return *a == *b;
}

static void parent_of(const char *p, char *out) {	// "/a/b" -> "/a", "/a" -> "/"
	strcpy(out, p);
	char *s = strrchr(out, '/');
	if (s == out) out[1] = 0; else *s = 0;
}

static void trimmed(const char *p, char *out) {	// no trailing '/', but "/"
	strcpy(out, p);
	size_t n = strlen(out);
	if (n > 1 && out[n - 1] == '/') out[n - 1] = 0;
}

z_rv z_zar(z_zar_args_t *a) {
	char d[64], want[64];
	switch (a->op) {
	case Z_ZAR_OP_FILE:
		for (unsigned i = 0; i < N_ZAR; i++)
			if (ieq(a->path, zar_files[i].path)) {
				a->off = i * 1000;
				a->size = (uint32_t)strlen(zar_files[i].data);
				return Z_OK;
			}
		return Z_FAIL;
	case Z_ZAR_OP_READ: {
		unsigned i = a->off / 1000, o = a->off % 1000;
		CHECK(i < N_ZAR && o + a->len <= strlen(zar_files[i].data));
		memcpy(a->buf, zar_files[i].data + o, a->len);
		return Z_OK;
	}
	case Z_ZAR_OP_ISDIR:
		trimmed(a->path, want);
		if (!strcmp(want, "/")) return Z_OK;
		for (unsigned i = 0; i < N_ZAR; i++) {
			parent_of(zar_files[i].path, d);
			if (ieq(d, want)) return Z_OK;
		}
		return Z_FAIL;
	case Z_ZAR_OP_CHILD:
		// iter counts files; a top directory is reported at its first
		trimmed(a->path, want);
		for (unsigned i = a->iter; i < N_ZAR; i++) {
			parent_of(zar_files[i].path, d);
			if (!strcmp(want, "/")) {
				bool first = true;
				for (unsigned j = 0; j < i; j++) {
					char dj[64];
					parent_of(zar_files[j].path, dj);
					if (!strcmp(dj, d)) first = false;
				}
				if (!first) continue;
				strcpy(a->name, d + 1);
				a->is_dir = 1; a->size = 0; a->iter = i + 1;
				return Z_OK;
			}
			if (ieq(d, want)) {
				strcpy(a->name, strrchr(zar_files[i].path, '/') + 1);
				a->is_dir = 0;
				a->size = (uint32_t)strlen(zar_files[i].data);
				a->iter = i + 1;
				return Z_OK;
			}
		}
		return Z_FAIL;
	}
	return Z_FAIL;
}

// The kernel's build of the client (ZFSC_KERNEL, sw/os/kfs.c) has no
// heap: zfsapp's listing helpers want zfs_opendir(), so the test gives
// them one. Reads in that build never use the grant window.
#ifdef ZFSC_KERNEL
typedef struct { zfs_dir_t d; uint32_t page[128]; } kdir_t;
zfs_dir_t *zfs_opendir(const char *path) {
	kdir_t *k = malloc(sizeof(*k));
	if (zfs_opendir_at(&k->d, path, k->page, sizeof(k->page)) < 0) { free(k); return NULL; }
	return &k->d;
}
int32_t zfs_closedir(zfs_dir_t *d) { int32_t r = zfs_closedir_at(d); free(d); return r; }
#define GRANT_READS	0
#else
#define GRANT_READS	1
#endif

// -- the tests -------------------------------------------------------------

static void fill(uint8_t *b, uint32_t n, uint32_t seed) {
	for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 7 + seed + (i >> 9));
}

// Names and types in one listing of `path`, via zfsapp.
static int listing(const char *path, char names[][40], uint8_t *types, int max) {
	static char buf[4096];
	uint8_t t[64];
	uint32_t count = 0;
	if (!fs_list_into(path, buf, sizeof buf, t, 64, &count, NULL)) return -1;
	char *q = buf;
	for (uint32_t i = 0; i < count && (int)i < max; i++) {
		const char *base = strrchr(q, '/') + 1;
		snprintf(names[i], 40, "%s", base);
		types[i] = t[i];
		q += strlen(q) + 1;
	}
	return (int)count;
}

static int find(char names[][40], int n, const char *name) {
	for (int i = 0; i < n; i++) if (!strcmp(names[i], name)) return i;
	return -1;
}

static void test_boot(void) {
	z_fs_info_t fi;
	// The card is blank: fs started without it. /ram works anyway, and
	// the flash files are there.
	CHECK(zfs_stat("/ram", &fi) == 0 && fi.type == Z_FS_TYPE_DIR);
	CHECK(zfs_stat("/sys", &fi) == -Z_ENOENT);
	CHECK(zfs_stat("/docs/welcome.txt", &fi) == 0 && fi.size == 32 &&
		(fi.attr & Z_FS_ATTR_RDONLY));
	CHECK(zfs_stat("/docs", &fi) == 0 && fi.type == Z_FS_TYPE_DIR);
	CHECK(zfs_stat("/usb", &fi) == -Z_ENOENT);		// nothing plugged in
	// format the card: fs mounts it and lays it out
	CHECK(zfs_mkfs("/") == 0);
	CHECK(zfs_stat("/sys", &fi) == 0 && fi.type == Z_FS_TYPE_DIR);
	CHECK(zfs_stat("/home", &fi) == 0 && zfs_stat("/data", &fi) == 0);
	// with a RAM disk, /tmp is it, not a card directory
	CHECK(fs_write_file("/tmp/t.txt", "tmp", 3) == 3);
	CHECK(zfs_stat("/ram/t.txt", &fi) == 0 && fi.size == 3);
	CHECK(fs_unlink("/tmp/t.txt") == 1);
	z_fs_vfs_t v;
	CHECK(zfs_statvfs("/", &v) == 0 && v.total_kb > 3000 && v.free_kb <= v.total_kb);
	CHECK(zfs_statvfs("/ram", &v) == 0 && v.total_kb > 800 && v.total_kb < 1100);
}

static void test_basic(void) {
	z_fs_info_t fi;
	CHECK(zfs_stat("/nope", &fi) == -Z_ENOENT);
	CHECK(zfs_open("/nope", Z_O_RDONLY) == -Z_ENOENT);

	int32_t fd = zfs_open("/a.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(fd >= 0);
	CHECK(zfs_write(fd, "hello world", 11) == 11);
	CHECK(zfs_fstat(fd, &fi) == 0 && fi.size == 11);
	CHECK(zfs_close(fd) == 0);
	CHECK(zfs_close(fd) == -Z_EBADF);

	char b[32] = {0};
	fd = zfs_open_info("/A.TXT", Z_O_RDONLY, &fi);		// FAT is case-blind
	CHECK(fd >= 0 && fi.size == 11);
	CHECK(zfs_read(fd, b, sizeof b) == 11 && !memcmp(b, "hello world", 11));
	CHECK(zfs_read(fd, b, sizeof b) == 0);
	CHECK(zfs_lseek(fd, 6, Z_SEEK_SET) == 6);
	CHECK(zfs_read(fd, b, 5) == 5 && !memcmp(b, "world", 5));
	CHECK(zfs_lseek(fd, -5, Z_SEEK_END) == 6);
	CHECK(zfs_lseek(fd, -1, Z_SEEK_SET) == -Z_EINVAL);
	CHECK(zfs_write(fd, "x", 1) == -Z_EACCES);			// opened read-only
	CHECK(zfs_close(fd) == 0);

	fd = zfs_open("/a.txt", Z_O_WRONLY | Z_O_APPEND);
	CHECK(fd >= 0 && zfs_write(fd, "!", 1) == 1 && zfs_close(fd) == 0);
	CHECK(zfs_stat("/a.txt", &fi) == 0 && fi.size == 12);
	CHECK(zfs_open("/a.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_EXCL) == -Z_EEXIST);
	CHECK(zfs_open("/new.txt", Z_O_WRONLY | Z_O_TRUNC) == -Z_ENOENT);
	fd = zfs_open("/a.txt", Z_O_RDWR | Z_O_TRUNC);
	CHECK(fd >= 0 && zfs_fstat(fd, &fi) == 0 && fi.size == 0 && zfs_close(fd) == 0);

	fd = zfs_open("/a.txt", Z_O_RDWR);
	CHECK(zfs_write(fd, "0123456789", 10) == 10);
	CHECK(zfs_ftruncate(fd, 4) == 0);
	CHECK(zfs_fstat(fd, &fi) == 0 && fi.size == 4);
	CHECK(zfs_fsync(fd) == 0);
	CHECK(zfs_close(fd) == 0);

	CHECK(zfs_mkdir("/d") == 0);
	CHECK(zfs_mkdir("/d") == -Z_EEXIST);
	CHECK(zfs_rename("/a.txt", "/d/b.txt") == 0);
	CHECK(zfs_stat("/a.txt", &fi) == -Z_ENOENT);
	CHECK(zfs_stat("/d/b.txt", &fi) == 0 && fi.size == 4);
	CHECK(zfs_rename("/d/b.txt", "/ram/b.txt") == -Z_EXDEV);
	CHECK(zfs_rename("/d", "/d/inside") == -Z_EINVAL);		// into itself
	CHECK(zfs_rename("/", "/x") == -Z_EINVAL);
	CHECK(zfs_unlink("/d") == -Z_ENOTEMPTY);		// not empty
	CHECK(zfs_unlink("/d/b.txt") == 0);
	CHECK(zfs_unlink("/d") == 0);

	char lp[300];
	memset(lp, 'a', sizeof lp); lp[0] = '/'; lp[299] = 0;
	CHECK(zfs_open(lp, Z_O_RDONLY) == -Z_ENAMETOOLONG);
}

static void test_rename_busy(void) {
	int32_t fd = zfs_open("/busy.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	int err;
	CHECK(fd >= 0);
	CHECK(zfs_rename("/busy.txt", "/moved.txt") == -Z_EBUSY);
	CHECK(fs_rename("/busy.txt", "/moved.txt", &err) == 0 && err == Z_FS_ERR_BUSY);
	CHECK(zfs_close(fd) == 0);
	CHECK(zfs_rename("/busy.txt", "/moved.txt") == 0);
	CHECK(zfs_unlink("/moved.txt") == 0);
}

static void test_flash(void) {
	char names[64][40];
	uint8_t types[64];
	z_fs_info_t fi;
	char b[64] = {0};

	// a file in flash reads like any other, and only reads
	int32_t fd = zfs_open_info("/docs/welcome.txt", Z_O_RDONLY, &fi);
	CHECK(fd >= 0 && fi.size == 32);
	CHECK(zfs_read(fd, b, 7) == 7 && !memcmp(b, "Welcome", 7));
	CHECK(zfs_lseek(fd, 100, Z_SEEK_SET) == 32);	// past the end stops there
	CHECK(zfs_read(fd, b, 10) == 0);
	CHECK(zfs_write(fd, "x", 1) == -Z_EROFS);
	CHECK(zfs_close(fd) == 0);
	// ... and not where the kernel asks for the card only
	CHECK(zfs_open("/apps/wm", Z_O_RDONLY | Z_O_NOFLASH) == -Z_ENOENT);
	fd = zfs_open("/apps/wm", Z_O_RDONLY);
	CHECK(fd >= 0 && zfs_close(fd) == 0);
	// the root lists the volumes, the card and what is only in flash
	int n = listing("/", names, types, 64);
	CHECK(n >= 6);
	CHECK(find(names, n, "ram") >= 0 && find(names, n, "tmp") >= 0);
	CHECK(find(names, n, "usb") < 0);
	CHECK(find(names, n, "sys") >= 0);
	int ia = find(names, n, "apps"), id = find(names, n, "docs");
	CHECK(ia >= 0 && types[ia] == Z_FS_TYPE_DIR);
	CHECK(id >= 0 && types[id] == Z_FS_TYPE_DIR);
	// /apps: no such directory on the card yet -- flash's
	n = listing("/apps", names, types, 64);
	CHECK(n == 2 && find(names, n, "wm") >= 0 && find(names, n, "term") >= 0);
	// a card copy shadows the flash one, listed once
	CHECK(zfs_mkdir("/apps") == 0);
	CHECK(fs_write_file("/apps/wm", "card wm", 7) == 7);
	n = listing("/apps", names, types, 64);
	CHECK(n == 2);
	CHECK(zfs_stat("/apps/wm", &fi) == 0 && fi.size == 7 && !(fi.attr & Z_FS_ATTR_RDONLY));
	fd = zfs_open("/apps/wm", Z_O_RDONLY | Z_O_NOFLASH);
	CHECK(fd >= 0 && zfs_close(fd) == 0);
	// "apps" at the root is listed once, as the card's
	n = listing("/", names, types, 64);
	int c = 0;
	for (int i = 0; i < n; i++) c += !strcmp(names[i], "apps");
	CHECK(c == 1);
	CHECK(fs_unlink("/apps/wm") == 1);
	// no /docs on the card: a write there fails rather than landing in flash
	CHECK(fs_write_file("/docs/mine.txt", "x", 1) == 0);
}

static void test_bulk(bool grant) {
	const uint32_t n = 300000;			// not a multiple of the chunk
	uint8_t *w = malloc(n), *r = malloc(n + 100);
	fill(w, n, grant ? 3 : 11);
	grant_allowed = grant;
	uint32_t opens0 = grant_opens;

	int32_t fd = zfs_open("/big.bin", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(zfs_write(fd, w, n) == (int32_t)n);
	CHECK(zfs_close(fd) == 0);
	memset(r, 0, n + 100);
	fd = zfs_open("/big.bin", Z_O_RDONLY);
	CHECK(zfs_read(fd, r, n + 100) == (int32_t)n);
	CHECK(!memcmp(r, w, n));
	CHECK(r[n] == 0);
	CHECK(zfs_close(fd) == 0);
	if (grant && GRANT_READS) CHECK(grant_opens - opens0 == (n + Z_FS_CHUNK - 1) / Z_FS_CHUNK);
	else CHECK(grant_opens == opens0);

	CHECK(fs_size("/big.bin") == (int)n);
	uint32_t r0 = reqs;
	char *m = fs_mallocfile("/big.bin");
	CHECK(m && !memcmp(m, w, n) && m[n] == 0);
	if (grant && GRANT_READS) CHECK(reqs - r0 == 2 + (n + Z_FS_CHUNK - 1) / Z_FS_CHUNK);
	free(m);
	int h = fs_open_read("/big.bin");
	CHECK(h >= 0x4000);
	CHECK(fs_seek(h, 100000) == 1);
	CHECK(fs_read_chunk(h, r, 8192) == 8192 && !memcmp(r, w + 100000, 8192));
	CHECK(fs_seek(h, 10) == 1);					// backwards, on the cluster map
	CHECK(fs_read_chunk(h, r, 100) == 100 && !memcmp(r, w + 10, 100));
	CHECK(fs_close_handle(h) == 1);
	// the same on the RAM disk
	CHECK(fs_write_file("/ram/big.bin", (char *)w, 200000) == 200000);
	m = fs_mallocfile("/ram/big.bin");
	CHECK(m && !memcmp(m, w, 200000));
	free(m);
	CHECK(fs_unlink("/ram/big.bin") == 1);
	CHECK(fs_unlink("/big.bin") == 1);
	free(w); free(r);
	grant_allowed = true;
}

static void test_listing(void) {
	char p[64], buf[4096];
	z_fs_info_t info[64];
	uint8_t types[64];
	uint32_t count, trunc;
	CHECK(fs_mkdir("/L") == 1);
	for (int i = 0; i < 40; i++) {
		snprintf(p, sizeof p, "/L/a_rather_long_file_name_%02d.txt", i);
		CHECK(fs_write_file(p, p, (int)strlen(p)) == (int)strlen(p));
	}
	CHECK(fs_mkdir("/L/sub") == 1);
	uint32_t r0 = reqs;
	CHECK(fs_list_ex("/L", buf, sizeof buf, info, 64, &count, &trunc) == 1);
	CHECK(count == 41 && trunc == 0);
	CHECK(GRANT_READS ? reqs - r0 == 2 : reqs - r0 > 2);	// OPENDIR (first page) + one READDIR; more with 512-byte pages
	int files = 0, dirs = 0, named = 1;
	char *q = buf;
	for (uint32_t i = 0; i < count; i++) {
		if (info[i].type == Z_FS_TYPE_DIR) dirs++;
		else { files++; if (info[i].size != strlen(q)) named = 0; }
		if (strncmp(q, "/L/", 3)) named = 0;
		q += strlen(q) + 1;
	}
	CHECK(files == 40 && dirs == 1 && named);
	CHECK(fs_list_into("/L/", buf, sizeof buf, types, 10, &count, &trunc) == 1);
	CHECK(count == 10 && trunc == 1 && !strncmp(buf, "/L/", 3) && buf[3] != '/');
	CHECK(fs_list_into("/L", buf, 200, NULL, 0, &count, &trunc) == 1);
	CHECK(count > 0 && count < 10 && trunc == 1);
	// stopping early closes it, and a slot is never leaked
	r0 = reqs;
	for (int i = 0; i < 8; i++)
		CHECK(fs_list_into("/L", buf, sizeof buf, types, 3, &count, &trunc) == 1 && count == 3);
	CHECK(reqs - r0 == 16);
	// a small page (the kernel's 512 bytes) still lists everything
	zfs_dir_t d;
	uint32_t page[128];
	int seen = 0;
	CHECK(zfs_opendir_at(&d, "/L", page, sizeof page) == 0);
	while (zfs_readdir(&d)) seen++;
	CHECK(seen == 41 && zfs_dir_error(&d) == 0);
	CHECK(zfs_closedir_at(&d) == 0);
	uint32_t n;
	char **names = fs_list("/L", 0, &n);
	CHECK(names && n == 41);
	for (uint32_t i = 0; names && i < n; i++) free(names[i]);
	free(names);
	for (int i = 0; i < 40; i++) {
		snprintf(p, sizeof p, "/L/a_rather_long_file_name_%02d.txt", i);
		CHECK(fs_unlink(p) == 1);
	}
	CHECK(fs_unlink("/L/sub") == 1 && fs_unlink("/L") == 1);
}

static void test_contention(void) {
	const uint32_t n = 100000;
	uint8_t *w = malloc(n), *r = malloc(n);
	fill(w, n, 29);
	grant_allowed = false;
	CHECK(fs_write_file("/c.bin", (char *)w, (int)n) == (int)n);
	client = 5;
	int32_t a = zfs_open("/c.bin", Z_O_RDONLY);
	CHECK(zfs_read(a, r, Z_FS_CHUNK) == Z_FS_CHUNK);	// 5 holds the bulk buffer
	client = 6;
	int32_t b = zfs_open("/c.bin", Z_O_RDONLY);
	int32_t got = zfs_read(b, r, Z_FS_CHUNK);
	CHECK(got > 0 && got <= Z_FS_DIR_BYTES && !memcmp(r, w, (size_t)got));
	ticks += 732;
	CHECK(zfs_read(b, r + got, Z_FS_CHUNK) == Z_FS_CHUNK);
	CHECK(!memcmp(r, w, (size_t)got + Z_FS_CHUNK));
	client = 5;
	got = zfs_read(a, r, Z_FS_CHUNK);
	CHECK(got > 0 && got <= Z_FS_DIR_BYTES && !memcmp(r, w + Z_FS_CHUNK, (size_t)got));
	CHECK(zfs_close(a) == 0);
	client = 6;
	CHECK(zfs_close(b) == 0);
	char *m = fs_mallocfile("/c.bin");
	CHECK(m && !memcmp(m, w, n));
	free(m);
	client = 5;
	CHECK(fs_unlink("/c.bin") == 1);
	free(w); free(r);
	grant_allowed = true;
}

// A writer is killed and another program is given its pid at once, as
// the kernel does: pid alone says the owner is still running. Before
// owners were recorded with their serial, its descriptors stayed open
// for good -- a slot gone for every such kill, the file "in use" for a
// rename -- and an unlink left the late close to write into a deleted
// entry.
static void test_reuse(void) {
	z_fs_info_t fi;
	client = 6;
	int32_t w = zfs_open("/reuse.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(w >= 0);
	CHECK(zfs_write(w, "half written", 12) == 12);
	respawn(6);					// killed; pid 6 is a new program now
	client = 5;
	// rename is not refused for a writer that has gone
	CHECK(zfs_rename("/reuse.txt", "/reused.txt") == 0);
	CHECK(zfs_stat("/reused.txt", &fi) == 0 && fi.size == 12);
	// and the new pid-6 program cannot use the dead one's descriptor
	// (fstat goes straight to fs; a small write would sit in zfsc's
	// buffer, which every simulated client here shares)
	client = 6;
	CHECK(zfs_fstat(w, &fi) == -Z_EBADF);

	// unlink straight after a kill: the handle is closed first
	int32_t u = zfs_open("/gone.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(u >= 0);
	CHECK(zfs_write(u, "abc", 3) == 3);
	CHECK(zfs_fsync(u) == 0);	// out of zfsc's buffer, which clients here share
	respawn(6);
	client = 5;
	CHECK(zfs_unlink("/gone.txt") == 0);
	CHECK(zfs_stat("/gone.txt", &fi) == -Z_ENOENT);
	// nothing is left open: all sixteen descriptors are there to take
	int32_t fds[16]; int got = 0;
	for (int i = 0; i < 16; i++) if ((fds[got] = zfs_open("/reused.txt", Z_O_RDONLY)) >= 0) got++;
	CHECK(got == 16);
	for (int i = 0; i < got; i++) CHECK(zfs_close(fds[i]) == 0);
	// a file made since reads back, so the late close did not land
	int32_t n = zfs_open("/after.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(n >= 0 && zfs_write(n, "fresh", 5) == 5 && zfs_close(n) == 0);
	char *m = fs_mallocfile("/after.txt");
	CHECK(m && !memcmp(m, "fresh", 5));
	free(m);

	// directory handles too: all four taken by a program that is then
	// killed, each part-way through a listing longer than one page (a
	// listing that fits one page is closed by fs as it is sent)
	CHECK(zfs_mkdir("/many") == 0);
	char nm[40];
	for (int i = 0; i < 60; i++) {
		snprintf(nm, sizeof nm, "/many/a fairly long file name %02d.txt", i);
		int32_t t = zfs_open(nm, Z_O_WRONLY | Z_O_CREAT);
		CHECK(t >= 0 && zfs_close(t) == 0);
	}
	client = 6;
	zfs_dir_t *d[4];
	for (int i = 0; i < 4; i++) CHECK((d[i] = zfs_opendir("/many")) != NULL);
	client = 5;
	CHECK(zfs_opendir("/many") == NULL);	// all four are taken
	respawn(6);
	zfs_dir_t *mine = zfs_opendir("/many");
	CHECK(mine != NULL);
	if (mine) CHECK(zfs_closedir(mine) == 0);
	// fs closed the dead program's listings; free the client's pages
	for (int i = 0; i < 4; i++) free(d[i]);
	for (int i = 0; i < 60; i++) {
		snprintf(nm, sizeof nm, "/many/a fairly long file name %02d.txt", i);
		CHECK(zfs_unlink(nm) == 0);
	}
	CHECK(zfs_unlink("/many") == 0);

	CHECK(zfs_unlink("/reused.txt") == 0);
	CHECK(zfs_unlink("/after.txt") == 0);
}

static void test_owners(void) {
	z_fs_info_t fi;
	client = 5;
	int32_t fd = zfs_open("/own.txt", Z_O_RDWR | Z_O_CREAT);
	CHECK(fd >= 0);
	client = 6;
	CHECK(zfs_write(fd, "x", 1) == -Z_EBADF);
	CHECK(zfs_close(fd) == -Z_EBADF);
	int32_t fds[16]; int got = 0;
	for (int i = 0; i < 16; i++) if ((fds[got] = zfs_open("/own.txt", Z_O_RDONLY)) >= 0) got++;
	CHECK(got == 15);
	CHECK(zfs_open("/own.txt", Z_O_RDONLY) == -Z_EMFILE);
	alive[6] = false;
	fs_test_sweep();
	alive[6] = true;
	CHECK((fds[0] = zfs_open("/own.txt", Z_O_RDONLY)) >= 0);
	CHECK(zfs_close(fds[0]) == 0);
	client = 5;
	// a volume with a file open will not unmount
	CHECK(zfs_umount("/") == -Z_EBUSY);
	CHECK(zfs_syncall() >= 1);
	CHECK(zfs_close(fd) == 0);
	CHECK(zfs_unlink("/own.txt") == 0);
	// unmounted, the card is gone until mounted; flash files stay
	CHECK(zfs_umount("/") == 0);
	CHECK(zfs_stat("/sys", &fi) == -Z_ENOENT);
	CHECK(zfs_stat("/docs/welcome.txt", &fi) == 0);
	CHECK(zfs_mount("/") == 0);
	CHECK(zfs_stat("/sys", &fi) == 0);
}

// -- the C library's file calls (sw/common/zposix.c), renamed zp_* --

#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
// zposix's struct dirent and DIR, by other names: the host's own are
// in scope here (sw/common/posix/dirent.h has the real ones).
struct zp_dirent { uint32_t d_ino; uint8_t d_type; char d_name[256]; };
typedef struct _zeitlos_DIR ZDIR;
int zp__open(const char *path, int flags, ...);
ssize_t zp__read(int fd, void *buf, size_t len);
ssize_t zp__write(int fd, const void *buf, size_t len);
int zp__close(int fd);
off_t zp__lseek(int fd, off_t off, int whence);
int zp__fstat(int fd, struct stat *st);
int zp_stat(const char *path, struct stat *st);
int zp_access(const char *path, int mode);
int zp_unlink(const char *path);
int zp_rmdir(const char *path);
int zp_mkdir(const char *path, mode_t mode);
int zp_rename(const char *from, const char *to);
int zp_chdir(const char *path);
char *zp_getcwd(char *buf, size_t size);
int zp_ftruncate(int fd, off_t len);
ZDIR *zp_opendir(const char *path);
struct zp_dirent *zp_readdir(ZDIR *dp);
int zp_closedir(ZDIR *dp);

static int term_reads, term_writes;
int32_t z_term_read(int fd, void *ptr, size_t len) { (void)fd; (void)ptr; term_reads++; return (int32_t)len; }
int32_t z_term_write(int fd, const void *ptr, size_t len) { (void)fd; (void)ptr; term_writes++; return (int32_t)len; }

static void test_posix(void) {
	struct stat st;
	char b[64], cwd[64];

	CHECK(zp_mkdir("/ram/px", 0755) == 0);
	CHECK(zp_mkdir("/ram/px", 0755) == -1 && errno == EEXIST);
	CHECK(zp_chdir("/ram/px") == 0);
	CHECK(zp_getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/ram/px"));
	CHECK(zp_getcwd(cwd, 3) == NULL && errno == ERANGE);

	// Relative paths, against the working directory.
	int fd = zp__open("a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 3);
	CHECK(zp__write(fd, "hello, ", 7) == 7);
	CHECK(zp__write(fd, "world", 5) == 5);
	CHECK(zp__fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 12);
	CHECK(zp__close(fd) == 0);
	CHECK(zp__close(fd) == -1 && errno == EBADF);
	CHECK(zp_stat("/ram/px/a.txt", &st) == 0 && st.st_size == 12);
	CHECK(zp_stat("../px/./a.txt", &st) == 0 && st.st_size == 12);
	CHECK(zp_stat("nope", &st) == -1 && errno == ENOENT);
	CHECK(zp_access("a.txt", R_OK) == 0 && zp_access("nope", R_OK) == -1);

	// Reading, seeking from each end.
	fd = zp__open("a.txt", O_RDONLY);
	CHECK(fd >= 3);
	CHECK(zp__read(fd, b, 5) == 5 && !memcmp(b, "hello", 5));
	CHECK(zp__lseek(fd, -5, SEEK_END) == 7);
	CHECK(zp__read(fd, b, sizeof b) == 5 && !memcmp(b, "world", 5));
	CHECK(zp__read(fd, b, sizeof b) == 0);
	CHECK(zp__lseek(fd, 2, SEEK_SET) == 2 && zp__lseek(fd, 3, SEEK_CUR) == 5);
	CHECK(zp__write(fd, "x", 1) == -1 && errno == EACCES);	// read-only
	CHECK(zp__close(fd) == 0);

	// O_APPEND and O_RDWR without truncation.
	fd = zp__open("a.txt", O_WRONLY | O_APPEND);
	CHECK(fd >= 3 && zp__write(fd, "!", 1) == 1 && zp__close(fd) == 0);
	CHECK(zp_stat("a.txt", &st) == 0 && st.st_size == 13);
	fd = zp__open("a.txt", O_RDWR);
	CHECK(fd >= 3 && zp__write(fd, "J", 1) == 1);
	CHECK(zp__lseek(fd, 0, SEEK_SET) == 0 && zp__read(fd, b, 13) == 13 &&
		!memcmp(b, "Jello, world!", 13));
	CHECK(zp_ftruncate(fd, 5) == 0 && zp__fstat(fd, &st) == 0 && st.st_size == 5);
	CHECK(zp__close(fd) == 0);
	CHECK(zp__open("a.txt", O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);

	// The terminal is not fs's.
	term_reads = term_writes = 0;
	CHECK(zp__write(1, "x", 1) == 1 && zp__read(0, b, 1) == 1);
	CHECK(term_reads == 1 && term_writes == 1);
	CHECK(zp__fstat(1, &st) == 0 && S_ISCHR(st.st_mode));

	// Directories.
	CHECK(zp_mkdir("sub", 0755) == 0);
	CHECK(fs_write_file("/ram/px/sub/z", "z", 1) == 1);
	ZDIR *d = zp_opendir(".");
	CHECK(d != NULL);
	int seen = 0, dirs = 0;
	struct zp_dirent *e;
	while (d && (e = zp_readdir(d)) != NULL) {
		seen++;
		if (e->d_type == 4) { dirs++; CHECK(!strcmp(e->d_name, "sub")); }
		else CHECK(!strcmp(e->d_name, "a.txt"));
	}
	CHECK(seen == 2 && dirs == 1);
	CHECK(d && zp_closedir(d) == 0);
	CHECK(zp_opendir("/ram/nothere") == NULL && errno == ENOENT);
	CHECK(zp_stat("sub", &st) == 0 && S_ISDIR(st.st_mode));

	// Rename, unlink, rmdir.
	CHECK(zp_rename("a.txt", "sub/b.txt") == 0);
	CHECK(zp_stat("a.txt", &st) == -1 && zp_stat("sub/b.txt", &st) == 0);
	// errno is fs's number, which is newlib's (90), not the host's
	CHECK(zp_rmdir("sub") == -1 && errno == Z_ENOTEMPTY);
	CHECK(zp_rmdir("sub/b.txt") == -1 && errno == ENOTDIR);
	CHECK(zp_unlink("sub/b.txt") == 0 && zp_unlink("sub/z") == 0);
	CHECK(zp_rmdir("sub") == 0);
	CHECK(zp_chdir("/") == 0 && zp_rmdir("/ram/px") == 0);
	CHECK(zp_chdir("/ram/px") == -1 && errno == ENOENT);
	CHECK(zp_getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/"));
}

// /usb follows the `usb` driver's drive: mounted when one comes (fs is
// told by a Z_USB_EVENT, or notices on its idle wake), released when it
// goes or is replaced.
static void test_usb(void) {
	z_fs_info_t fi;
	MKFS_PARM opt;
	static BYTE wk[FF_MAX_SS];
	char b[16];

	usbd = calloc(USB_SECTORS, 512);
	fs_test_usb_poll();
	CHECK(zfs_stat("/usb", &fi) == -Z_ENOENT);

	// A formatted stick, plugged in: present, not yet started.
	usb_present = true; usb_ready = true;
	memset(&opt, 0, sizeof(opt));
	opt.fmt = FM_FAT;
	CHECK(f_mkfs("2:", &opt, wk, sizeof wk) == FR_OK);
	usb_ready = false;
	usb_gen++;
	// Not ready yet: `usb` brings it up itself and says so. fs waits
	// rather than asking for the whole wait (MSC_START).
	fs_test_usb_poll();
	CHECK(usb_starts == 0 && zfs_stat("/usb", &fi) == -Z_ENOENT);
	usb_ready = true;				// usb: it is ready (Z_USB_EVENT)
	fs_test_usb_poll();
	CHECK(usb_starts == 1);
	CHECK(zfs_stat("/usb", &fi) == 0 && fi.type == Z_FS_TYPE_DIR);
	CHECK(fs_write_file("/usb/hello.txt", "from usb", 8) == 8);
	int32_t fd = zfs_open("/usb/hello.txt", Z_O_RDONLY);
	CHECK(fd >= 0 && zfs_read(fd, b, sizeof b) == 8 && !memcmp(b, "from usb", 8));
	CHECK(zfs_close(fd) == 0);
	fs_test_usb_poll();				// nothing changed: no new start
	CHECK(usb_starts == 1);

	// Pulled: released.
	usb_present = false; usb_ready = false; usb_gen++;
	fs_test_usb_poll();
	CHECK(zfs_stat("/usb", &fi) == -Z_ENOENT);
	CHECK(zfs_open("/usb/hello.txt", Z_O_RDONLY) < 0);

	// Back, and `usbmount` before usb has it ready: the whole wait,
	// asked for (MSC_START); mounted again, the file still there.
	usb_present = true; usb_gen++;
	CHECK(zfs_mount("/usb") == 0);
	CHECK(usb_starts == 2 && zfs_stat("/usb/hello.txt", &fi) == 0 && fi.size == 8);
	usb_present = false; usb_ready = false; usb_gen++;
	fs_test_usb_poll();
	free(usbd);
}

// A reply that comes after its caller gave up is thrown away at the
// start of the next call, not left in the mailbox for good.
static void test_stale(void) {
	z_fs_info_t fi;
	hold_next = true;
	CHECK(zfs_stat("/ram", &fi) == -Z_ETIMEDOUT);
	reply = held;					// it arrives, late
	have_reply = true;
	takes = 0;
	CHECK(zfs_stat("/ram", &fi) == 0 && fi.type == Z_FS_TYPE_DIR);
	CHECK(takes == 1 && !have_reply);
	takes = 0;
	CHECK(zfs_stat("/ram", &fi) == 0 && takes == 0);	// nothing left to look for
}

// More clients than reply slots, all with replies unread: the next is
// told to try again rather than given a slot whose reply nobody has
// copied, and gets one when a slot frees.
static void test_slots(void) {
	z_fs_info_t fi;
	uint32_t me = client;
	// Age out the slots of every earlier client but this one.
	for (int i = 0; i < 300; i++) zfs_stat("/ram", &fi);
	for (uint32_t p = 40; p < 45; p++) {		// five more, one call each
		client = p;
		CHECK(zfs_stat("/ram", &fi) == 0);
	}
	client = 46;					// a seventh client
	waits = 0;
	die_on_wait = 40;				// ...until the first of them dies
	CHECK(zfs_stat("/ram", &fi) == 0 && fi.type == Z_FS_TYPE_DIR);
	CHECK(waits == 1);
	alive[40] = true;

	// Nobody dies and nothing else uses fs: the slots still free once
	// their replies are a second old. Before, a quiet machine with six
	// processes that had each touched a file once refused the seventh
	// for good (`cat` in the languages demo, docs/demo.md).
	client = me;
	for (int i = 0; i < 300; i++) zfs_stat("/ram", &fi);
	for (uint32_t p = 40; p < 45; p++) {		// this one and five more
		client = p;
		CHECK(zfs_stat("/ram", &fi) == 0);
	}
	client = 47;
	waits = 0;
	tick_on_wait = 1;
	CHECK(zfs_stat("/ram", &fi) == 0 && fi.type == Z_FS_TYPE_DIR);
	CHECK(waits >= 700 && waits <= 740);		// one second of ticks
	tick_on_wait = 0;
	client = me;
}

// fs started again (after a crash, restarted by the kernel): /ram is
// mounted as it was, not formatted.
static void test_restart(void) {
	z_fs_info_t fi;
	CHECK(fs_write_file("/ram/keep.txt", "kept", 4) == 4);
	fs_test_start(true);
	CHECK(zfs_stat("/ram/keep.txt", &fi) == 0 && fi.size == 4);
	CHECK(fs_unlink("/ram/keep.txt") == 1);
}

// zfsc.c's write-behind: small writes to one file go as one request.
static void test_writeback(void) {
	static uint8_t blk[1468], back[1468 * 20];
	z_fs_info_t fi;
	int32_t fd = zfs_open("/wb.bin", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(fd >= 0);
	uint32_t r0 = reqs;
	for (int i = 0; i < 20; i++) {
		for (int k = 0; k < 1468; k++) blk[k] = (uint8_t)(i * 31 + k);
		CHECK(zfs_write(fd, blk, sizeof blk) == (int32_t)sizeof blk);
	}
	uint32_t sent = reqs - r0;
	// 29,360 bytes: the first write direct, the rest in ~8KB requests.
	CHECK(sent >= 3 && sent <= 6);
	CHECK(zfs_fstat(fd, &fi) == 0 && fi.size == 20 * 1468);	// flushed first
	CHECK(zfs_write(fd, "tail", 4) == 4);
	CHECK(zfs_close(fd) == 0);				// sends "tail"
	CHECK(zfs_stat("/wb.bin", &fi) == 0 && fi.size == 20 * 1468 + 4);
	fd = zfs_open("/wb.bin", Z_O_RDONLY);
	CHECK(zfs_read(fd, back, sizeof back) == (int32_t)sizeof back);
	bool same = true;
	for (int i = 0; i < 20 && same; i++)
		for (int k = 0; k < 1468; k++)
			if (back[i * 1468 + k] != (uint8_t)(i * 31 + k)) { same = false; break; }
	CHECK(same);
	CHECK(zfs_close(fd) == 0);

	// Read your own writes. (Through a second descriptor FatFs shows
	// them only after an fsync or close, buffered here or not.)
	fd = zfs_open("/wb2.txt", Z_O_RDWR | Z_O_CREAT | Z_O_TRUNC);
	CHECK(zfs_write(fd, "ab", 2) == 2);			// direct
	CHECK(zfs_write(fd, "cd", 2) == 2);			// buffered
	CHECK(zfs_fstat(fd, &fi) == 0 && fi.size == 4);	// sent before the fstat
	CHECK(zfs_write(fd, "ef", 2) == 2);
	CHECK(zfs_lseek(fd, 0, Z_SEEK_SET) == 0);		// sent before the seek
	char b[8] = {0};
	CHECK(zfs_read(fd, b, sizeof b) == 6 && !memcmp(b, "abcdef", 6));
	CHECK(zfs_write(fd, "gh", 2) == 2);
	CHECK(zfs_fsync(fd) == 0);
	int32_t fd2 = zfs_open("/wb2.txt", Z_O_RDONLY);
	CHECK(zfs_read(fd2, b, sizeof b) == 8 && !memcmp(b, "abcdefgh", 8));
	CHECK(zfs_close(fd2) == 0 && zfs_close(fd) == 0);

	// The first write to a descriptor is never buffered: errors at once.
	fd = zfs_open("/wb2.txt", Z_O_RDONLY);
	CHECK(zfs_write(fd, "x", 1) == -Z_EACCES);
	CHECK(zfs_write(fd, "x", 1) == -Z_EACCES);
	CHECK(zfs_close(fd) == 0);
	CHECK(zfs_unlink("/wb.bin") == 0 && zfs_unlink("/wb2.txt") == 0);
}

// The "kernel shell" asking net (Z_NFSOP_MOUNT and friends): the reply.
static int32_t shell_nfs(uint32_t op, const char *path, char *out, size_t outcap) {
	z_nfsreq_t rq;
	z_blob_t b;
	z_obj_t o;
	z_msg_t m;
	memset(&rq, 0, sizeof rq);
	rq.op = op;
	if (path) strcpy(rq.path, path);
	b.len = sizeof rq; b.data = (uint8_t *)&rq;
	o.type = Z_BLOB; o.val.ptr = &b;
	z_msg_new_send(NET_PID, Z_NET_NFS_REQ, 77, o);
	if (z_msg_recv_match(&m, Z_NET_NFS_REPLY, 77, 0) != Z_OK) return -999;
	const z_nfsrep_t *r = z_blob_data(&m.obj);
	if (out && outcap) {
		size_t n = r->len < outcap - 1 ? r->len : outcap - 1;
		memcpy(out, r + 1, n);
		out[n] = 0;
	}
	return r->result;
}

static bool listed(const char *dir, const char *name, uint32_t *count, z_fs_info_t *fi_out) {
	zfs_dir_t d;
	static uint32_t page[512];
	const z_fs_dirent_t *e;
	bool hit = false;
	uint32_t n = 0;
	if (zfs_opendir_at(&d, dir, page, 2048) != 0) return false;
	while ((e = zfs_readdir(&d)) != NULL) {
		n++;
		if (!strcmp(e->name, name)) { hit = true; if (fi_out) *fi_out = e->info; }
	}
	zfs_closedir_at(&d);
	if (count) *count = n;
	return hit;
}

static void test_nfs(void) {
	z_fs_info_t fi;
	static uint8_t buf[12000];
	char spec[200];
	uint32_t n;
	int32_t fd;

	tree_init();
	// Not mounted: nothing under /nfs, and fs does not even ask net.
	CHECK(zfs_stat("/nfs", &fi) == -Z_ENOENT);
	CHECK(zfs_open("/nfs/hello.txt", Z_O_RDONLY) == -Z_ENOENT);
	CHECK(!listed("/", "nfs", NULL, NULL));
	CHECK(shell_nfs(Z_NFSOP_STATUS, NULL, spec, sizeof spec) == 0);

	// Mounting: rpcbind, MOUNT, from a port below 1024; fs is told.
	CHECK(shell_nfs(Z_NFSOP_MOUNT, "10.0.0.9:/nothere", NULL, 0) == -Z_EIO);
	CHECK(nfs_events == 0);
	CHECK(shell_nfs(Z_NFSOP_MOUNT, "nfsbox:/export", NULL, 0) == -Z_EINVAL);
	CHECK(shell_nfs(Z_NFSOP_MOUNT, "10.0.0.9:/export", NULL, 0) == 0);
	CHECK(nfs_events == 1 && nfs_event_val == 1);
	CHECK(nfs_low_ports >= 3);
	CHECK(shell_nfs(Z_NFSOP_STATUS, NULL, spec, sizeof spec) == 1 && !strcmp(spec, "10.0.0.9:/export"));

	// The volume
	CHECK(listed("/", "nfs", NULL, &fi) && fi.type == Z_FS_TYPE_DIR);
	CHECK(zfs_stat("/nfs", &fi) == 0 && fi.type == Z_FS_TYPE_DIR && (fi.attr & Z_FS_ATTR_RDONLY));
	CHECK(zfs_stat("/NFS/Hello.txt", &fi) == -Z_ENOENT);	// the server's names are its own
	CHECK(zfs_stat("/nfs/hello.txt", &fi) == 0 && fi.size == 5 && fi.type == Z_FS_TYPE_FILE);
	CHECK(fi.fdate == (uint16_t)(fs_fattime_of(1700000000u) >> 16));
	CHECK(zfs_stat("/nfs/missing", &fi) == -Z_ENOENT);
	CHECK(zfs_stat("/nfs/hello.txt/x", &fi) == -Z_ENOTDIR);
	CHECK(zfs_stat("/nfs/noattr", &fi) == 0 && fi.size == 3);	// GETATTR after LOOKUP

	// Reading: a 10000-byte file, three 4KB requests to the server in
	// one read; then from the middle.
	fd = zfs_open("/nfs/apps/text", Z_O_RDONLY);
	CHECK(fd >= 0);
	memset(nfs_calls, 0, sizeof nfs_calls);
	int32_t got = zfs_read(fd, buf, sizeof buf);
	CHECK(got == 10000 && nfs_calls[6] == 3);
	bool same = true;
	node_t *tn = node(3);
	for (int i = 0; i < 10000; i++) if (buf[i] != file_byte(tn, (uint32_t)i)) same = false;
	CHECK(same);
	CHECK(zfs_read(fd, buf, 10) == 0);
	CHECK(zfs_lseek(fd, 9990, Z_SEEK_SET) == 9990);
	CHECK(zfs_read(fd, buf, 100) == 10 && buf[0] == file_byte(tn, 9990));
	CHECK(zfs_close(fd) == 0);

	// The cache: a second file in /apps is one LOOKUP, not a walk.
	memset(nfs_calls, 0, sizeof nfs_calls);
	CHECK(zfs_stat("/nfs/apps/text", &fi) == 0 && fi.size == 10000);
	CHECK(nfs_calls[3] == 1);

	// Read-only, all of it
	CHECK(zfs_open("/nfs/new.txt", Z_O_WRONLY | Z_O_CREAT) == -Z_EROFS);
	CHECK(zfs_open("/nfs/hello.txt", Z_O_RDWR) == -Z_EROFS);
	CHECK(zfs_unlink("/nfs/hello.txt") == -Z_EROFS);
	CHECK(zfs_mkdir("/nfs/d") == -Z_EROFS);
	CHECK(zfs_rename("/nfs/hello.txt", "/nfs/h2") == -Z_EROFS);
	CHECK(zfs_rename("/hello.txt", "/nfs/h2") == -Z_EROFS);
	CHECK(zfs_open("/nfs/apps", Z_O_RDONLY) == -Z_EISDIR);

	// Listings: the root's, and one of 70 files over several pages,
	// from a server that leaves out the directory's own attributes.
	CHECK(listed("/nfs", "apps", &n, &fi) && n == 4 && fi.type == Z_FS_TYPE_DIR);
	CHECK(listed("/nfs/big", "file_69.txt", &n, &fi) && n == 70 && fi.size == 69);
	CHECK(listed("/nfs/big", "file_00.txt", NULL, NULL));
	{
		zfs_dir_t d;
		uint32_t page[64];
		CHECK(zfs_opendir_at(&d, "/nfs/hello.txt", page, sizeof page) == -Z_ENOTDIR);
	}

	// A program's way in: open with NOFLASH, the header, then the rest.
	fd = zfs_open("/nfs/apps/text", Z_O_RDONLY | Z_O_NOFLASH);
	CHECK(fd >= 0 && zfs_read(fd, buf, 32) == 32);
	CHECK(zfs_close(fd) == 0);

	// A server that stops answering: fs gives up (net's 5 s), and net,
	// once its own deadline passes, is free again.
	srv_silent = true;
	CHECK(zfs_stat("/nfs/hello.txt", &fi) == -Z_ETIMEDOUT);
	ticks += 10 * 732;
	nfs_poll();
	srv_silent = false;
	CHECK(zfs_stat("/nfs/hello.txt", &fi) == 0);

	// Unmounted: gone again.
	CHECK(shell_nfs(Z_NFSOP_UMOUNT, NULL, NULL, 0) == 0);
	CHECK(nfs_events == 2 && nfs_event_val == 0);
	CHECK(zfs_stat("/nfs/hello.txt", &fi) == -Z_ENOENT);
	CHECK(!listed("/", "nfs", NULL, NULL));

	// net.cfg's mount: a server not answering yet is tried again every
	// 30 s until the mount is made ...
	srv_silent = true;
	nfs_mount_at_boot("10.0.0.9:/export");
	while (net_pump()) ;
	ticks += 6 * 732;
	nfs_poll();								// the first try gives up
	CHECK(!nfs_test_up() && nfs_events == 2);
	srv_silent = false;
	ticks += 10 * 732;
	nfs_poll();
	while (net_pump()) ;
	CHECK(!nfs_test_up());					// not yet: 30 s
	ticks += 21 * 732;
	nfs_poll();
	while (net_pump()) nfs_poll();
	CHECK(nfs_test_up() && nfs_events == 3 && nfs_event_val == 1);
	CHECK(zfs_stat("/nfs/hello.txt", &fi) == 0);
	CHECK(shell_nfs(Z_NFSOP_UMOUNT, NULL, NULL, 0) == 0);
	// ... but not once the shell has unmounted or mounted something.
	srv_silent = true;
	nfs_mount_at_boot("10.0.0.9:/export");
	while (net_pump()) ;
	ticks += 6 * 732;
	nfs_poll();
	CHECK(shell_nfs(Z_NFSOP_UMOUNT, NULL, NULL, 0) == 0);
	srv_silent = false;
	ticks += 31 * 732;
	nfs_poll();
	while (net_pump()) nfs_poll();
	CHECK(!nfs_test_up() && nfs_events == 4);
}

// -- watching (zfsproto.h, "Watching") ---------------------------------
// (apps' client only: the kernel's has no watch calls)
#ifndef ZFSC_KERNEL

static void wput(const char *path, const char *text) {
	int32_t fd = zfs_open(path, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(fd >= 0);
	CHECK(zfs_write(fd, text, (uint32_t)strlen(text)) == (int32_t)strlen(text));
	CHECK(zfs_close(fd) == 0);
}

// What watch `wd` (client `who`) has heard: the count, paths in `out`.
static int32_t heard(uint32_t who, int32_t wd, z_fs_watch_t *h, char *out, uint32_t cap) {
	uint32_t was = client;
	client = who;
	int32_t n = zfs_watch_read(wd, h, out, cap);
	client = was;
	return n;
}

static bool has_path(const char *paths, int32_t n, const char *p) {
	for (int32_t i = 0; i < n; i++, paths += strlen(paths) + 1)
		if (!strcmp(paths, p)) return true;
	return false;
}

static void test_watch(void) {
	z_fs_watch_t h;
	char paths[Z_FS_WATCH_PATHS * Z_FS_PATH_MAX];
	const uint32_t W = 11, OTHER = 5;
	client = OTHER;
	zfs_mkdir("/home");
	CHECK(zfs_mkdir("/home/w") == 0);
	client = W;
	int32_t wd = zfs_watch("/home/w/");
	CHECK(wd >= 0 && wd < Z_FS_WATCHES);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 0 && h.writers == 0 && h.flags == 0);

	// a file written there: a notice at the open, then nothing until read
	changed_n = 0;
	client = OTHER;
	int32_t fd = zfs_open("/home/w/a.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	CHECK(fd >= 0);
	CHECK(changed_n == 1 && changed_to == W && changed_tag == (uint32_t)wd);
	CHECK(zfs_write(fd, "abc", 3) == 3);
	CHECK(changed_n == 1);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/home/w/a.txt"));
	CHECK(h.writers == 1);				// still being written
	client = OTHER;
	CHECK(zfs_close(fd) == 0);
	CHECK(changed_n == 2);				// the close is a change
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 0 && h.writers == 0);

	// reading it, its own changes, and elsewhere: nothing
	changed_n = 0;
	client = OTHER;
	fd = zfs_open("/home/w/a.txt", Z_O_RDONLY);
	CHECK(fd >= 0 && zfs_close(fd) == 0);
	wput("/home/x.txt", "x");
	wput("/home/wx.txt", "x");			// a name that starts the same
	wput("/ram/w.txt", "x");
	client = W;
	wput("/home/w/mine.txt", "x");		// the watcher's own
	CHECK(changed_n == 0);

	// case does not matter, as on FAT; paths come back as written
	client = OTHER;
	wput("/HOME/W/Big.TXT", "x");
	CHECK(changed_n == 1);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/HOME/W/Big.TXT"));

	// rename in and out, and unlink
	changed_n = 0;
	client = OTHER;
	CHECK(zfs_rename("/home/x.txt", "/home/w/x.txt") == 0);
	CHECK(changed_n == 1);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/home/w/x.txt"));
	client = OTHER;
	CHECK(zfs_rename("/home/w/x.txt", "/home/x2.txt") == 0);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/home/w/x.txt"));
	client = OTHER;
	CHECK(zfs_unlink("/home/w/a.txt") == 0);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/home/w/a.txt"));
	// a save as text does it: a new copy, renamed over the old
	client = OTHER;
	wput("/home/w/book.new", "v2");
	CHECK(zfs_rename("/home/w/book.new", "/home/w/book.txt") == 0);
	int32_t n = heard(W, wd, &h, paths, sizeof paths);
	CHECK(n == 2 && has_path(paths, n, "/home/w/book.new") && has_path(paths, n, "/home/w/book.txt"));
	CHECK(changed_n == 4);			// one per read: rename in, out, unlink, the save
	// a failed change is none
	changed_n = 0;
	client = OTHER;
	CHECK(zfs_unlink("/home/w/nope") < 0);
	CHECK(changed_n == 0);

	// the same path twice is once; past Z_FS_WATCH_PATHS, "more"
	client = OTHER;
	wput("/home/w/1", "a"); wput("/home/w/1", "b");
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && h.flags == 0);
	client = OTHER;
	for (int i = 0; i < Z_FS_WATCH_PATHS + 2; i++) {
		char p[32];
		snprintf(p, sizeof p, "/home/w/m%d", i);
		wput(p, "m");
	}
	n = heard(W, wd, &h, paths, sizeof paths);
	CHECK(n == Z_FS_WATCH_PATHS && (h.flags & Z_FS_WATCH_MORE));
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 0 && h.flags == 0);
	// a buffer too small for them all: the rest count as dropped
	client = OTHER;
	wput("/home/w/m0", "z"); wput("/home/w/m1", "z");
	n = heard(W, wd, &h, paths, 12);
	CHECK(n == 1 && !strcmp(paths, "/home/w/m0") && (h.flags & Z_FS_WATCH_MORE));

	// a full mailbox: the notice goes later, still only one
	changed_n = 0;
	watch_mailbox_full = true;
	client = OTHER;
	wput("/home/w/late", "l");
	CHECK(changed_n == 0);
	watch_mailbox_full = false;
	fs_test_kick();						// fs's idle wake
	CHECK(changed_n == 1);
	fs_test_kick();
	CHECK(changed_n == 1);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/home/w/late"));

	// someone else's watch number, or none
	CHECK(heard(OTHER, wd, &h, paths, sizeof paths) == -Z_EBADF);
	CHECK(heard(W, 9, &h, paths, sizeof paths) == -Z_EBADF);
	client = OTHER;
	CHECK(zfs_unwatch(wd) == -Z_EBADF);

	// a file, /ram, and Z_FS_WATCHES at most
	client = W;
	int32_t wf = zfs_watch("/home/w/book.txt");
	int32_t wr = zfs_watch("/tmp");
	int32_t w4 = zfs_watch("/");
	CHECK(wf >= 0 && wr >= 0 && w4 >= 0);
	CHECK(zfs_watch("/home") == -Z_EMFILE);
	changed_n = 0;
	client = OTHER;
	wput("/home/w/book.txt", "v3");		// wd, wf and "/": three notices
	CHECK(changed_n == 3);
	CHECK(heard(W, wf, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/home/w/book.txt"));
	CHECK(heard(W, w4, &h, paths, sizeof paths) == 1);
	client = OTHER;
	wput("/home/w/other.txt", "o");		// not the file
	CHECK(heard(W, wf, &h, paths, sizeof paths) == 0);
	wput("/ram/r.txt", "r");				// /tmp is /ram
	CHECK(heard(W, wr, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/ram/r.txt"));
	CHECK(heard(W, w4, &h, paths, sizeof paths) == 1 && !strcmp(paths, "/home/w/other.txt"));
	client = W;
	CHECK(zfs_unwatch(w4) == 0);
	CHECK(zfs_unwatch(w4) == -Z_EBADF);
	CHECK((w4 = zfs_watch("/home")) >= 0);	// the slot again
	CHECK(zfs_unwatch(w4) == 0 && zfs_unwatch(wf) == 0 && zfs_unwatch(wr) == 0);

	// a writer that dies: its handle is closed by the sweep, and that
	// is a change; a watcher that dies: its watch ends
	heard(W, wd, &h, paths, sizeof paths);
	client = 8;
	fd = zfs_open("/home/w/dying.txt", Z_O_WRONLY | Z_O_CREAT);
	CHECK(fd >= 0);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 1 && h.writers == 1);
	changed_n = 0;
	alive[8] = false; generation[8]++;
	fs_test_sweep();
	fs_test_kick();						// the idle wake: sweep, then notices
	alive[8] = true;
	CHECK(changed_n == 1);
	CHECK(heard(W, wd, &h, paths, sizeof paths) == 0 && h.writers == 0);
	alive[W] = false; generation[W]++;
	fs_test_sweep();
	alive[W] = true;
	CHECK(heard(W, wd, &h, paths, sizeof paths) == -Z_EBADF);
	changed_n = 0;
	client = OTHER;
	wput("/home/w/after.txt", "a");
	CHECK(changed_n == 0);
	// four again, now that it is gone
	client = W;
	int32_t all[Z_FS_WATCHES];
	for (int i = 0; i < Z_FS_WATCHES; i++) CHECK((all[i] = zfs_watch("/home/w")) >= 0);
	for (int i = 0; i < Z_FS_WATCHES; i++) CHECK(zfs_unwatch(all[i]) == 0);
	// A watcher that dies holding all four, its pid given to a new
	// program before any sweep: the new one cannot read or end the old
	// watches, and the next watch, anyone's, sweeps first rather than
	// being refused until the idle sweep.
	for (int i = 0; i < Z_FS_WATCHES; i++) CHECK((all[i] = zfs_watch("/home/w")) >= 0);
	generation[W]++;
	CHECK(heard(W, all[0], &h, paths, sizeof paths) == -Z_EBADF);
	client = W;
	CHECK(zfs_unwatch(all[1]) == -Z_EBADF);
	client = OTHER;
	int32_t wo = zfs_watch("/home");
	CHECK(wo >= 0);
	CHECK(zfs_unwatch(wo) == 0);
	// ... and a change is not told to whoever has a dead watcher's pid:
	// the watch ends instead
	client = W;
	int32_t wk = zfs_watch("/home/w");
	CHECK(wk >= 0);
	generation[W]++;
	changed_n = 0;
	client = OTHER;
	wput("/home/w/after2.txt", "a");
	CHECK(changed_n == 0);
	client = W;
	CHECK(zfs_unwatch(wk) == -Z_EBADF);
	for (int i = 0; i < Z_FS_WATCHES; i++) CHECK((all[i] = zfs_watch("/home/w")) >= 0);
	for (int i = 0; i < Z_FS_WATCHES; i++) CHECK(zfs_unwatch(all[i]) == 0);
	// paths fs cannot watch
	CHECK(zfs_watch("/nfs/x") == -Z_EINVAL);
	client = OTHER;
}
#endif

int main(void) {
	card = calloc(CARD_SECTORS, 512);
	ram = calloc(RAM_SECTORS, 512);
	for (int i = 0; i < 64; i++) alive[i] = true;
	fs_test_start(true);
	test_boot();
	test_basic();
	test_rename_busy();
	test_flash();
	test_bulk(true);
	test_bulk(false);
	test_listing();
	test_contention();
	test_owners();
	test_reuse();
	test_usb();
	test_posix();
	test_stale();
	test_slots();
	test_restart();
	test_writeback();
	test_nfs();
#ifndef ZFSC_KERNEL
	test_watch();
#endif
	CHECK(grant_misuse == 0 && !grant_open_now);
	printf("test_fs: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
