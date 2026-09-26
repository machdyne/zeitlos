#ifndef KVSTORE_H
#define KVSTORE_H
/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The kernel's side of the flash key/value store: where it lives, who
 * may write it, Z_SYS_KV, and the `kv` console command. The format and
 * every operation on it are kvlog.c. docs/kvstore.md.
 */
#include <stdint.h>
#include <stdbool.h>
#include "../common/zobj.h"
#include "kvlog.h"

// Finds the store (the last Z_KV_SIZE bytes of the chip) and prints a
// one-line summary. Reads only. Call once at boot, before any process
// can reach Z_SYS_KV.
void k_kv_init(void);

// Where the store is, as a flash offset.
uint32_t k_kv_base(void);

// True if [addr, addr + len) touches the store, including through the
// chip's address wrap-around -- an address one chip-size higher lands
// on the same bytes. Z_SYS_FLASH refuses those (flashapi.c).
bool k_kv_overlaps(uint32_t addr, uint32_t len);

// Kernel-internal access. No key is protected here: the auth.*/sys.*
// rule is applied at Z_SYS_KV and in the `kv` command, and kernel code
// (phase B's passwd) is what those keys exist for. Results are KV_*
// (kvlog.h) plus Z_KV_E_BUSY and Z_KV_E_READONLY (zkv.h).
int k_kv_get(const char *key, void *buf, uint32_t cap, uint32_t *len);
int k_kv_set(const char *key, const void *val, uint32_t len, uint32_t flags);
int k_kv_del(const char *key, uint32_t flags);
int k_kv_compact(void);
int k_kv_stat(kv_stat_t *st);
int k_kv_entry(uint32_t index, char *key, uint32_t *vlen);

// True if this bitstream can write the flash, and so the store.
bool k_kv_writable(void);

// True for keys only the kernel may write: auth.* and sys.*
bool k_kv_protected(const char *key);

const char *k_kv_strerror(int rc);

// A process is being reaped while it held the store's write lock (it was
// killed inside a Z_SYS_KV call). Interrupt path: forgets the lock and
// nothing else -- the log is built to survive a writer stopping at any
// point, so there is nothing to repair.
void k_kv_release_pid(uint32_t pid);

z_obj_t *k_kv(z_obj_t *args);          // Z_SYS_KV

// The `kv` console command: kv, kv get|set|del|compact|test
void k_kv_shell(const char *sub, const char *a1, const char *a2);

#endif
