#ifndef Z_KV_H
#define Z_KV_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The flash key/value store, for apps: Z_SYS_KV. docs/kvstore.md.
 *
 * A few KB of settings that must exist with or without an sdcard, kept
 * by the kernel in the last 8 KB of the configuration flash. It is for
 * small things that belong to the MACHINE rather than to a card --
 * identity and security settings -- not a second config file: ordinary
 * settings belong in /zeitlos.cfg (docs/config.md).
 *
 *   keys    1..31 characters of [A-Za-z0-9._-]; by convention
 *           "apps.<app>.<name>", as in /zeitlos.cfg
 *   values  0..256 bytes, binary is fine
 *   total   everything live must fit in 4 KB, headers included
 *
 * Writes are durable when the call returns, and atomic: after a power
 * cut a key holds its old value or its new one, never a mixture.
 * A write takes milliseconds when it appends and up to about a second
 * when it has to compact the store; reads are cheap.
 *
 * Keys beginning "auth." and "sys." belong to the kernel: apps can see
 * that they exist (z_kv_entry()) but get Z_KV_E_PROTECTED for anything
 * else. Note that this is an API boundary, not secrecy -- any app can
 * read any address, flash included (docs/mpu.md).
 */
#include <stdint.h>
#include "zeitlos.h"

#define Z_KV_KEY_MAX      31
#define Z_KV_VAL_MAX      256

// results
#define Z_KV_OK            0
#define Z_KV_E_NOENT      -1    // no such key
#define Z_KV_E_INVAL      -2    // bad key, or value longer than Z_KV_VAL_MAX
#define Z_KV_E_NOSPC      -3    // the store is full
#define Z_KV_E_IO         -4    // the flash refused, or read back wrong
#define Z_KV_E_BUFFER     -5    // value longer than your buffer; len says how long
#define Z_KV_E_BUSY       -6    // another writer, or an open Z_SYS_FLASH session
#define Z_KV_E_PROTECTED  -7    // an auth.* or sys.* key
#define Z_KV_E_READONLY   -8    // this bitstream cannot write the flash
#define Z_KV_E_NOSYS      -9    // the running kernel has no Z_SYS_KV

// flags for set and del
#define Z_KV_SCRUB         1u   // compact now: superseded values are physically
                                // erased, not merely superseded

enum {
	Z_KV_GET = 0,
	Z_KV_SET = 1,
	Z_KV_DEL = 2,
	Z_KV_INFO = 3,
	Z_KV_ENTRY = 4,
};

typedef struct {
	uint32_t op;
	const char *key;        // GET, SET, DEL: in
	void *val;              // GET: out; SET: in; INFO: a z_kv_info_t, out
	uint32_t len;           // GET: in capacity, out length; SET: in length
	uint32_t flags;         // SET, DEL: Z_KV_SCRUB
	uint32_t index;         // ENTRY: in
	char *name;             // ENTRY: out, Z_KV_KEY_MAX + 1 bytes
	int32_t result;         // out
} z_kv_args_t;

#define Z_KV_INFO_WRITABLE  1u

typedef struct {
	uint32_t base;          // flash offset of the store
	uint32_t size;          // Z_KV_SIZE
	uint32_t flags;         // Z_KV_INFO_*
	int32_t active;         // the sector in use, 0 or 1; -1: never written
	uint32_t generation;    // counts compactions
	uint32_t used;          // bytes of the 4 KB sector used, superseded records included
	uint32_t live;          // bytes compaction would keep
	uint32_t records;
	uint32_t keys;
} z_kv_info_t;

static inline int z_kv_call(z_kv_args_t *a) {
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	a->result = Z_KV_E_NOSYS;
	k(Z_SYS_KV, (uint32_t *)a, 0);
	return a->result;
}

// Z_KV_OK and the value in buf, its length in *len. With Z_KV_E_BUFFER,
// *len is the length needed and buf is untouched.
static inline int z_kv_get(const char *key, void *buf, uint32_t cap, uint32_t *len) {
	z_kv_args_t a = { Z_KV_GET, key, buf, cap, 0, 0, 0, 0 };
	int rc = z_kv_call(&a);
	if (len) *len = a.len;
	return rc;
}

static inline int z_kv_set_flags(const char *key, const void *val, uint32_t len,
		uint32_t flags) {
	z_kv_args_t a = { Z_KV_SET, key, (void *)(uintptr_t)val, len, flags, 0, 0, 0 };
	return z_kv_call(&a);
}

static inline int z_kv_set(const char *key, const void *val, uint32_t len) {
	return z_kv_set_flags(key, val, len, 0);
}

static inline int z_kv_del(const char *key) {
	z_kv_args_t a = { Z_KV_DEL, key, 0, 0, 0, 0, 0, 0 };
	return z_kv_call(&a);
}

static inline int z_kv_info(z_kv_info_t *info) {
	z_kv_args_t a = { Z_KV_INFO, 0, info, sizeof(*info), 0, 0, 0, 0 };
	return z_kv_call(&a);
}

// The index'th key (0, 1, ...) and its value's length; Z_KV_E_NOENT past
// the last. `name` must hold Z_KV_KEY_MAX + 1 bytes.
static inline int z_kv_entry(uint32_t index, char *name, uint32_t *vlen) {
	z_kv_args_t a = { Z_KV_ENTRY, 0, 0, 0, 0, index, name, 0 };
	int rc = z_kv_call(&a);
	if (vlen) *vlen = a.len;
	return rc;
}

#endif
