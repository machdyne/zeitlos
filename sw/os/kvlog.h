#ifndef KVLOG_H
#define KVLOG_H

/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The flash key/value store's on-flash format and every operation on
 * it. docs/kvstore.md is the design; this header is the contract.
 *
 * NO PLATFORM DEPENDENCIES, on purpose -- no zeitlos.h, no kernel.h,
 * no flash registers. The store reaches the flash only through the
 * three callbacks in kv_dev_t, which is what lets sw/os/tests/
 * test_kvlog.c run every operation on the host against a simulated
 * NOR flash that loses power at every possible point. Same rule as
 * sw/apps/net/ssh/ssh_proto.c: if something in here ever needs the
 * time or a register, it takes a callback rather than an include.
 *
 * -- the shape --
 *
 * Two 4 KB sectors. At most one is ACTIVE: the valid header with the
 * higher generation. Records are appended to it; the latest valid
 * record for a key wins. When a write does not fit, the live records
 * are compacted into the other sector, whose header is written LAST --
 * that header is the commit point, so a power cut anywhere leaves one
 * complete sector to mount from.
 *
 * -- stateless --
 *
 * Nothing is cached between calls. Every call reads the flash, which
 * is the only source of truth. A writer that dies halfway (killed,
 * power cut) is therefore the same event to the next call, and a
 * reader never needs a lock: at every instant the flash holds either
 * the old state or the new one.
 */

#include <stdint.h>

#define KV_SECTOR        4096u
#define KV_REGION        (2u * KV_SECTOR)
#define KV_PAGE          256u
#define KV_HDR_LEN       16u
#define KV_REC_HDR       8u
#define KV_KEY_MAX       31u
#define KV_VAL_MAX       256u
#define KV_REC_MAX       ((KV_REC_HDR + KV_KEY_MAX + KV_VAL_MAX + 3u) & ~3u)
// smallest record: 8 + 1-byte key + empty value, padded to 12
#define KV_MAX_RECS      ((KV_SECTOR - KV_HDR_LEN) / 12u)

// results -- the same numbers as Z_KV_* in sw/common/zkv.h
#define KV_OK             0
#define KV_ENOENT        -1     // no such key
#define KV_EINVAL        -2     // bad key or value length
#define KV_ENOSPC        -3     // the live data would not fit in a sector
#define KV_EIO           -4     // the flash refused, or read back wrong
#define KV_EBUFFER       -5     // value longer than the caller's buffer

// set/del flags
#define KV_SCRUB          1u    // compact even if it would fit: the old
                                // records are physically erased

// Offsets are within the 8 KB region: 0..8191. erase() takes a sector
// offset (0 or 4096); program() never crosses a 256-byte page. Both
// return 0 on success and must not return until the operation is over.
typedef struct {
	uint8_t (*read)(void *ctx, uint32_t off);
	int (*erase)(void *ctx, uint32_t off);
	int (*program)(void *ctx, uint32_t off, const uint8_t *p, uint32_t n);
	void *ctx;
} kv_dev_t;

// Scratch space for the operations that write or enumerate. The caller
// owns it and must not share it between concurrent calls; kv_get()
// needs none, which is what keeps reads lock-free.
typedef struct {
	uint16_t off[KV_MAX_RECS];     // record offsets in the active sector
	uint8_t hash[KV_MAX_RECS];     // key hash, to skip most comparisons
	uint8_t state[KV_MAX_RECS];    // KV_ST_* below
	uint16_t n;
	uint8_t rec[KV_REC_MAX];       // one record, being written or copied
} kv_work_t;

typedef struct {
	int active;            // 0 or 1, or -1: no valid sector (empty store)
	uint32_t generation;
	uint32_t used;         // bytes of the active sector in use, header included
	uint32_t live_bytes;   // what compaction would keep, header included
	uint32_t records;      // records in the log, valid or not
	uint32_t keys;         // live keys
	int dirty;             // the log ends in something unreadable: the next
	                       // write compacts rather than appends
} kv_stat_t;

// 1 if `key` is a legal key: 1..31 characters of [A-Za-z0-9._-].
int kv_key_ok(const char *key);

// Copies the value into buf. *len gets the value's length; with
// KV_EBUFFER nothing is copied and *len says how much room it needs.
int kv_get(const kv_dev_t *d, const char *key, uint8_t *buf, uint32_t cap,
	uint32_t *len);

int kv_set(const kv_dev_t *d, kv_work_t *w, const char *key,
	const uint8_t *val, uint32_t len, uint32_t flags);

// KV_ENOENT, and nothing written, if the key is not there.
int kv_del(const kv_dev_t *d, kv_work_t *w, const char *key, uint32_t flags);

// Compacts unconditionally. On an empty store, does nothing.
int kv_compact(const kv_dev_t *d, kv_work_t *w);

void kv_stat(const kv_dev_t *d, kv_work_t *w, kv_stat_t *st);

// The index'th live key, in log order: its name (NUL-terminated,
// KV_KEY_MAX + 1 bytes) and value length. KV_ENOENT past the end.
int kv_entry(const kv_dev_t *d, kv_work_t *w, uint32_t index,
	char *key, uint32_t *vlen);

#endif
