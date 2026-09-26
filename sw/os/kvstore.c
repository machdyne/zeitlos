/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The flash key/value store, the kernel's side: kvstore.h. The log
 * format and its operations are kvlog.c, which knows nothing of this
 * file; docs/kvstore.md is the design.
 *
 * -- who may write --
 *
 * One writer at a time, system-wide. The lock is taken with preemption
 * deferred for the few instructions of the test-and-set (k_fs_enter(),
 * as the filesystem does for longer), and it records the holder, so a
 * process killed while inside Z_SYS_KV leaves nothing held: the reaper
 * calls k_kv_release_pid() through k_flash_release_pid(). The log
 * tolerates a writer stopping anywhere, exactly as it tolerates a power
 * cut, so releasing is all there is to do.
 *
 * The store also takes the flash controller's write session, as its
 * own owner (K_FLASH_KV). While an app holds a Z_SYS_FLASH session --
 * zfpga writing gateware, say -- store writes fail with Z_KV_E_BUSY
 * rather than interleave commands with it.
 *
 * Reads take no lock at all: kvlog.c's kv_get() is correct against a
 * writer running concurrently, which its host test checks by compacting
 * the store in the middle of reads.
 *
 * -- waiting --
 *
 * A write waits for the flash inside the syscall: up to 400 ms for a
 * sector erase, a compaction being two erases and some pages. Syscalls
 * other than the filesystem's are preemptible (docs/kernel.md), so the
 * rest of the system keeps running while this one waits.
 */
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "flashapi.h"
#include "kvstore.h"
#include "../common/zsoc.h"
#include "../common/zflash.h"
#include "../common/zkv.h"

#define REG(o) (*(volatile uint32_t *)(Z_SPIFLASH_BASE + (o)))

// Kept out of gp-relative reach, as every kernel global a syscall
// touches is (docs/kernel.md, "The gp hazard").
static uint32_t __attribute__((section(".bss"))) kv_base;
static uint32_t __attribute__((section(".bss"))) kv_span;      // chip size, <= 16 MB
static uint32_t __attribute__((section(".bss"))) kv_writable;
static uint32_t __attribute__((section(".bss"))) kv_size_known;
static uint32_t __attribute__((section(".bss"))) kv_lock;      // holder pid + 1; 0 free
static kv_work_t __attribute__((section(".bss"))) kv_work;

// -- the flash, as kvlog.c sees it --

static uint8_t dev_read(void *ctx, uint32_t off) {
	(void)ctx;
	return k_flash_window(kv_base + off);
}

// Waits for the operation just started. Bounded twice over: by ticks,
// and by a loop count in case ticks are not advancing.
static int dev_wait(void) {
	uint32_t t0 = z_kernel_ticks, n = 0, st;
	while ((st = k_flash_hw_status()) & Z_SPIFLASH_BUSY) {
		if (++n > 1000u && z_kernel_ticks - t0 > Z_TICK_HZ) return -1;
		if (n > 50000000u) return -1;
	}
	return (st & Z_SPIFLASH_DONE) ? 0 : -1;
}

static int dev_erase(void *ctx, uint32_t off) {
	(void)ctx;
	if (k_flash_hw_erase(kv_base + off)) return -1;
	return dev_wait();
}

static int dev_program(void *ctx, uint32_t off, const uint8_t *p, uint32_t n) {
	(void)ctx;
	if (k_flash_hw_program(kv_base + off, p, n)) return -1;
	return dev_wait();
}

static const kv_dev_t kv_dev = { dev_read, dev_erase, dev_program, NULL };

// -- where --

void k_kv_init(void) {
	uint32_t size = 0, n = 0;
	kv_stat_t st;

	kv_lock = 0;
	kv_writable = k_flash_present();
	if (kv_writable) {
		while ((k_flash_hw_status() & Z_SPIFLASH_BUSY) && n < 3000000u) n++;
		uint32_t cap = REG(Z_SPIFLASH_ID) & 0xFFu;
		// JEDEC capacity byte: 2^n bytes on Winbond and most others
		if (cap >= 16 && cap <= 28) size = 1u << cap;
	}
	kv_size_known = size != 0;
	if (!size) size = Z_KV_DEFAULT_FLASH_SIZE;
	if (size > Z_KV_FLASH_MAX) size = Z_KV_FLASH_MAX;
	kv_span = size;
	kv_base = size - Z_KV_SIZE;

	printf(" - kv: store at 0x%06lx", (unsigned long)kv_base);
	if (!kv_size_known)
		printf(" (flash size unknown, %lu KB assumed)", (unsigned long)(size / 1024));
	k_kv_stat(&st);
	if (st.active < 0) printf(", empty");
	else printf(", %lu key%s, %lu of %u bytes", (unsigned long)st.keys,
		st.keys == 1 ? "" : "s", (unsigned long)st.used, KV_SECTOR);
	printf("%s\n", kv_writable ? "" : "; READ-ONLY, this bitstream cannot write the flash");
}

bool k_kv_writable(void) {
	return kv_writable != 0;
}

uint32_t k_kv_base(void) {
	return kv_base;
}

bool k_kv_overlaps(uint32_t addr, uint32_t len) {
	uint32_t a;
	if (!kv_span || !len) return false;
	// the chip ignores address bits above its size, so every multiple
	// of the chip size aliases the store
	a = addr % kv_span;
	if (a < kv_base + Z_KV_SIZE && a + len > kv_base) return true;
	// a range that wraps past the end of the chip
	if (a + len > kv_span && (a + len - kv_span) > kv_base) return true;
	return false;
}

bool k_kv_protected(const char *key) {
	return !strncmp(key, "auth.", 5) || !strncmp(key, "sys.", 4);
}

// -- the lock --

static int lock(void) {
	bool got;
	if (!kv_writable) return Z_KV_E_READONLY;
	k_fs_enter();
	got = (kv_lock == 0);
	if (got) kv_lock = z_pid + 1;
	k_fs_leave();
	if (!got) return Z_KV_E_BUSY;
	if (!k_flash_begin(K_FLASH_KV)) {
		kv_lock = 0;
		return Z_KV_E_BUSY;
	}
	return 0;
}

static void unlock(void) {
	k_flash_end(K_FLASH_KV);
	kv_lock = 0;
}

void k_kv_release_pid(uint32_t pid) {
	if (kv_lock == pid + 1) {
		k_flash_end(K_FLASH_KV);
		kv_lock = 0;
	}
}

// -- kernel-internal access --

int k_kv_get(const char *key, void *buf, uint32_t cap, uint32_t *len) {
	return kv_get(&kv_dev, key, (uint8_t *)buf, cap, len);
}

int k_kv_set(const char *key, const void *val, uint32_t len, uint32_t flags) {
	int rc = lock();
	if (rc) return rc;
	rc = kv_set(&kv_dev, &kv_work, key, (const uint8_t *)val, len, flags);
	unlock();
	return rc;
}

int k_kv_del(const char *key, uint32_t flags) {
	int rc = lock();
	if (rc) return rc;
	rc = kv_del(&kv_dev, &kv_work, key, flags);
	unlock();
	return rc;
}

int k_kv_compact(void) {
	int rc = lock();
	if (rc) return rc;
	rc = kv_compact(&kv_dev, &kv_work);
	unlock();
	return rc;
}

// Stat and entry need the work area but write nothing, so they take the
// store's lock without the flash session -- a read-only bitstream can
// still list its store.
static int lock_ro(void) {
	bool got;
	k_fs_enter();
	got = (kv_lock == 0);
	if (got) kv_lock = z_pid + 1;
	k_fs_leave();
	return got ? 0 : Z_KV_E_BUSY;
}

int k_kv_stat(kv_stat_t *st) {
	int rc = lock_ro();
	if (rc) { memset(st, 0, sizeof(*st)); st->active = -1; return rc; }
	kv_stat(&kv_dev, &kv_work, st);
	kv_lock = 0;
	return 0;
}

int k_kv_entry(uint32_t index, char *key, uint32_t *vlen) {
	int rc = lock_ro();
	if (rc) return rc;
	rc = kv_entry(&kv_dev, &kv_work, index, key, vlen);
	kv_lock = 0;
	return rc;
}

const char *k_kv_strerror(int rc) {
	switch (rc) {
	case Z_KV_OK:          return "ok";
	case Z_KV_E_NOENT:     return "no such key";
	case Z_KV_E_INVAL:     return "bad key, or value over 256 bytes";
	case Z_KV_E_NOSPC:     return "the store is full";
	case Z_KV_E_IO:        return "the flash refused, or read back wrong";
	case Z_KV_E_BUFFER:    return "value longer than the buffer";
	case Z_KV_E_BUSY:      return "busy";
	case Z_KV_E_PROTECTED: return "auth.* and sys.* keys belong to the kernel";
	case Z_KV_E_READONLY:  return "this bitstream cannot write the flash";
	default:               return "error";
	}
}

// -- Z_SYS_KV --

// Copies an app's key into kernel memory. Reading app memory is always
// allowed (docs/mpu.md); what this guards against is a key that is not
// terminated, or changes while the store is working on it.
static bool copy_key(const char *src, char *dst) {
	uint32_t i;
	if (!src) return false;
	for (i = 0; i <= KV_KEY_MAX; i++) {
		dst[i] = src[i];
		if (!dst[i]) return true;
	}
	return false;
}

z_obj_t *k_kv(z_obj_t *args) {
	z_kv_args_t *a = (z_kv_args_t *)args;
	char key[KV_KEY_MAX + 1];
	uint32_t len = 0;
	int rc;

	if (!a || !k_user_ok_words(a, sizeof(*a))) return &z_fail;

	switch (a->op) {
	case Z_KV_GET:
		if (!copy_key(a->key, key) || !kv_key_ok(key)) { rc = Z_KV_E_INVAL; break; }
		if (k_kv_protected(key)) { rc = Z_KV_E_PROTECTED; break; }
		if (a->len && (!a->val || !k_user_ok(a->val, a->len))) { rc = Z_KV_E_INVAL; break; }
		rc = k_kv_get(key, a->val, a->len, &len);
		a->len = len;
		break;
	case Z_KV_SET:
	case Z_KV_DEL:
		if (!copy_key(a->key, key) || !kv_key_ok(key)) { rc = Z_KV_E_INVAL; break; }
		if (k_kv_protected(key)) { rc = Z_KV_E_PROTECTED; break; }
		if (a->op == Z_KV_DEL) { rc = k_kv_del(key, a->flags & Z_KV_SCRUB); break; }
		if (a->len > KV_VAL_MAX || (a->len && !a->val)) { rc = Z_KV_E_INVAL; break; }
		rc = k_kv_set(key, a->val, a->len, a->flags & Z_KV_SCRUB);
		break;
	case Z_KV_INFO: {
		z_kv_info_t *info = (z_kv_info_t *)a->val;
		kv_stat_t st;
		if (!info || !k_user_ok_words(info, sizeof(*info))) { rc = Z_KV_E_INVAL; break; }
		rc = k_kv_stat(&st);
		if (rc) break;
		info->base = kv_base;
		info->size = Z_KV_SIZE;
		info->flags = kv_writable ? Z_KV_INFO_WRITABLE : 0;
		info->active = st.active;
		info->generation = st.generation;
		info->used = st.used;
		info->live = st.live_bytes;
		info->records = st.records;
		info->keys = st.keys;
		break;
	}
	case Z_KV_ENTRY:
		if (!a->name || !k_user_ok(a->name, KV_KEY_MAX + 1)) { rc = Z_KV_E_INVAL; break; }
		rc = k_kv_entry(a->index, key, &len);
		if (rc == Z_KV_OK) {
			memcpy(a->name, key, strlen(key) + 1);
			a->len = len;
		}
		break;
	default:
		rc = Z_KV_E_INVAL;
	}
	a->result = rc;
	return rc == Z_KV_OK ? &z_ok : &z_fail;
}

// -- the `kv` console command --

// One value buffer for everything below: .bss is flash image here
// (docs/kernel.md, "The 256KB image budget"), and the console runs one
// command at a time.
static uint8_t __attribute__((section(".bss"))) sh_val[KV_VAL_MAX];

static void print_value(const uint8_t *v, uint32_t len) {
	uint32_t i;
	bool text = true;
	for (i = 0; i < len; i++)
		if (v[i] < 0x20 || v[i] > 0x7e) { text = false; break; }
	if (text) {
		printf("\"");
		for (i = 0; i < len; i++) printf("%c", v[i]);
		printf("\"");
	} else {
		for (i = 0; i < len; i++) printf("%02x", v[i]);
	}
}

static void shell_list(void) {
	kv_stat_t st;
	char key[KV_KEY_MAX + 1];
	uint8_t *val = sh_val;
	uint32_t i, vlen;

	printf("kv: flash 0x%06lx-0x%06lx%s%s\n", (unsigned long)kv_base,
		(unsigned long)(kv_base + Z_KV_SIZE - 1),
		kv_size_known ? "" : " (flash size assumed)",
		kv_writable ? "" : ", read-only on this bitstream");
	if (k_kv_stat(&st)) { printf("kv: busy\n"); return; }
	if (st.active < 0) { printf("kv: empty\n"); return; }
	printf("kv: sector %d, generation %lu: %lu key%s, %lu records, %lu of %u "
		"bytes used, %lu live%s\n", st.active, (unsigned long)st.generation,
		(unsigned long)st.keys, st.keys == 1 ? "" : "s", (unsigned long)st.records,
		(unsigned long)st.used, KV_SECTOR, (unsigned long)st.live_bytes,
		st.dirty ? "; the log ends in a cut-off write (the next write compacts)" : "");
	for (i = 0; k_kv_entry(i, key, &vlen) == Z_KV_OK; i++) {
		printf("  %-31s %3lu  ", key, (unsigned long)vlen);
		if (k_kv_protected(key)) printf("(kernel)");
		else if (k_kv_get(key, val, KV_VAL_MAX, &vlen) == Z_KV_OK) print_value(val, vlen);
		printf("\n");
	}
}

#ifndef KV_TEST
#define KV_TEST 0
#endif
#if KV_TEST
static void shell_test(void);
#endif

void k_kv_shell(const char *sub, const char *a1, const char *a2) {
	uint8_t *val = sh_val;
	uint32_t len;
	int rc;

	if (!sub || !*sub) { shell_list(); return; }

	if (!strcmp(sub, "get") && a1) {
		if (k_kv_protected(a1)) { printf("kv: %s\n", k_kv_strerror(Z_KV_E_PROTECTED)); return; }
		rc = k_kv_get(a1, val, KV_VAL_MAX, &len);
		if (rc) { printf("kv: %s: %s\n", a1, k_kv_strerror(rc)); return; }
		print_value(val, len);
		printf("  (%lu bytes)\n", (unsigned long)len);
	} else if (!strcmp(sub, "set") && a1 && a2) {
		if (k_kv_protected(a1)) { printf("kv: %s\n", k_kv_strerror(Z_KV_E_PROTECTED)); return; }
		rc = k_kv_set(a1, a2, (uint32_t)strlen(a2), 0);
		if (rc) printf("kv: %s: %s\n", a1, k_kv_strerror(rc));
	} else if (!strcmp(sub, "del") && a1) {
		if (k_kv_protected(a1)) { printf("kv: %s\n", k_kv_strerror(Z_KV_E_PROTECTED)); return; }
		rc = k_kv_del(a1, 0);
		if (rc) printf("kv: %s: %s\n", a1, k_kv_strerror(rc));
	} else if (!strcmp(sub, "compact")) {
		uint32_t t0 = z_kernel_ticks;
		rc = k_kv_compact();
		if (rc) printf("kv: %s\n", k_kv_strerror(rc));
		else printf("kv: compacted in %lu ms\n",
			(unsigned long)((z_kernel_ticks - t0) * 1000u / Z_TICK_HZ));
#if KV_TEST
	} else if (!strcmp(sub, "test")) {
		shell_test();
#endif
	} else {
		printf("usage: kv                      the store and its keys\n"
			"       kv get <key>\n"
			"       kv set <key> <value>      quote a value with spaces\n"
			"       kv del <key>\n"
			"       kv compact\n"
			"       kv test                   on-board test (test.kv.* keys)\n");
	}
}

#if KV_TEST
// -- `kv test`: the store on real flash --
//
// About 2.5 KB of kernel image with its strings. Build with
// `make KV_TEST=0` to leave it out once the store is proven on a board.
//
// The logic is tested on the host, power cuts and all (sw/os/tests/
// test_kvlog.c). What only a board can test is the binding: the real
// controller, the real timings, the syscall's refusals. Uses keys under
// test.kv.* and removes them; every other key is checked to be
// untouched at the end.

// An order-independent digest of every key but test.kv.*, and its
// value: a sum of per-key hashes, so no table of keys is needed.
static uint32_t kt_digest(uint32_t *count) {
	char key[KV_KEY_MAX + 1];
	uint32_t i, j, len, sum = 0;
	*count = 0;
	for (i = 0; k_kv_entry(i, key, &len) == Z_KV_OK; i++) {
		uint32_t h = 0x811C9DC5u;
		if (!strncmp(key, "test.kv.", 8)) continue;
		if (k_kv_get(key, sh_val, KV_VAL_MAX, &len) != Z_KV_OK) return 0;
		for (j = 0; key[j]; j++) h = (h ^ (uint8_t)key[j]) * 16777619u;
		for (j = 0; j < len; j++) h = (h ^ sh_val[j]) * 16777619u;
		sum += h;
		(*count)++;
	}
	return sum;
}

static void shell_test(void) {
	uint8_t *val = sh_val, big[KV_VAL_MAX];
	uint32_t i, len, t0, before, n, n2;
	int fails = 0, rc;
	#define KT(ok, what) do { if (ok) printf("  ok   %s\n", what); \
		else { printf("  FAIL %s\n", what); fails++; } } while (0)

	if (!kv_writable) { printf("kv test: %s\n", k_kv_strerror(Z_KV_E_READONLY)); return; }
	printf("kv test: store at 0x%06lx\n", (unsigned long)kv_base);
	before = kt_digest(&n);

	// 1. refusals: Z_SYS_FLASH may not touch the store, however addressed
	{
		z_flash_args_t fa = { Z_FLASH_ERASE, kv_base, 0, 0, 0 };
		k_flash((z_obj_t *)&fa);
		KT(fa.result == Z_FLASH_E_RESERVED, "Z_SYS_FLASH erase of the store is refused");
		fa.op = Z_FLASH_ERASE; fa.addr = kv_base + Z_KV_SIZE / 2 + kv_span; fa.result = 0;
		k_flash((z_obj_t *)&fa);
		KT(fa.result == Z_FLASH_E_RESERVED, "... and through the address wrap-around");
		fa.op = Z_FLASH_PROGRAM; fa.addr = kv_base - 4; fa.buf = big; fa.len = 8; fa.result = 0;
		k_flash((z_obj_t *)&fa);
		KT(fa.result == Z_FLASH_E_RESERVED, "a program straddling its start is refused");
	}
	{
		z_kv_args_t ka = { Z_KV_SET, "auth.test", big, 4, 0, 0, 0, 0 };
		k_kv((z_obj_t *)&ka);
		KT(ka.result == Z_KV_E_PROTECTED, "Z_SYS_KV refuses to set an auth.* key");
		ka.op = Z_KV_GET; ka.key = "sys.test"; ka.val = val; ka.len = KV_VAL_MAX;
		k_kv((z_obj_t *)&ka);
		KT(ka.result == Z_KV_E_PROTECTED, "... and to read a sys.* key");
		ka.op = Z_KV_SET; ka.key = "bad key"; ka.val = big; ka.len = 1;
		k_kv((z_obj_t *)&ka);
		KT(ka.result == Z_KV_E_INVAL, "... and a key with a space");
	}

	// 2. write, read, overwrite, delete
	t0 = z_kernel_ticks;
	rc = k_kv_set("test.kv.a", "alpha", 5, 0);
	KT(rc == Z_KV_OK, "set");
	printf("       (%lu ms)\n", (unsigned long)((z_kernel_ticks - t0) * 1000u / Z_TICK_HZ));
	KT(k_kv_get("test.kv.a", val, KV_VAL_MAX, &len) == Z_KV_OK && len == 5 &&
		!memcmp(val, "alpha", 5), "read back");
	KT(k_kv_set("test.kv.a", "omega!", 6, 0) == Z_KV_OK &&
		k_kv_get("test.kv.a", val, KV_VAL_MAX, &len) == Z_KV_OK && len == 6 &&
		!memcmp(val, "omega!", 6), "overwrite, read back");
	for (i = 0; i < KV_VAL_MAX; i++) big[i] = (uint8_t)(i * 37u + 11u);
	KT(k_kv_set("test.kv.big", big, KV_VAL_MAX, 0) == Z_KV_OK &&
		k_kv_get("test.kv.big", val, KV_VAL_MAX, &len) == Z_KV_OK && len == KV_VAL_MAX &&
		!memcmp(val, big, KV_VAL_MAX), "256 bytes across a page boundary, read back");
	KT(k_kv_del("test.kv.a", 0) == Z_KV_OK &&
		k_kv_get("test.kv.a", val, KV_VAL_MAX, &len) == Z_KV_E_NOENT, "delete");

	// 3. a compaction: two erases and every live record copied
	t0 = z_kernel_ticks;
	rc = k_kv_set("test.kv.big", "scrubbed", 8, KV_SCRUB);
	KT(rc == Z_KV_OK, "scrubbing set (compacts)");
	printf("       (%lu ms)\n", (unsigned long)((z_kernel_ticks - t0) * 1000u / Z_TICK_HZ));
	{
		bool gone = true;
		for (i = 0; i + 8 <= Z_KV_SIZE && gone; i++)
			if (k_flash_window(kv_base + i) == big[0] && k_flash_window(kv_base + i + 1) == big[1] &&
					k_flash_window(kv_base + i + 2) == big[2] && k_flash_window(kv_base + i + 3) == big[3])
				gone = false;
		KT(gone, "the superseded 256 bytes are physically gone");
	}

	// 4. clean up, and nothing else changed
	KT(k_kv_del("test.kv.big", KV_SCRUB) == Z_KV_OK, "clean up");
	KT(kt_digest(&n2) == before && n2 == n, "every other key unchanged");

	printf("kv test: %s\n", fails ? "FAILED -- please report the lines above" : "passed");
	#undef KT
}
#endif
