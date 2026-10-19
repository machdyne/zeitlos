/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpool -- the pool. See zpool.h.
 *
 * Every block has a header (its size, with the low bit "in use") and a
 * footer (the same word), so a freed block finds its neighbours in
 * O(1) and merges with any that are free. Free blocks keep a doubly
 * linked list in their first two words.
 */

#include "zpool.h"

#include <string.h>

typedef struct fblk {
	uint32_t		size;		// header: total bytes, low bit = used
	uint32_t		pad;
	struct fblk		*next, *prev;
} fblk_t;

#define HDR   8u		// header word + pad, keeps payload 8-aligned
#define FTR   4u
#define MINB  32u		// smallest block: header, links, footer

static uint8_t *p_base;
static size_t p_size;
static fblk_t *p_free;

static inline uint32_t bsize(const void *b) { return ((const fblk_t *)b)->size & ~7u; }
static inline bool bused(const void *b) { return (((const fblk_t *)b)->size & 1u) != 0; }
static inline uint32_t *footer(void *b) { return (uint32_t *)((uint8_t *)b + bsize(b) - FTR); }

static void set_block(void *b, uint32_t size, bool used) {
	((fblk_t *)b)->size = size | (used ? 1u : 0u);
	*(uint32_t *)((uint8_t *)b + size - FTR) = size | (used ? 1u : 0u);
}

static void list_remove(fblk_t *b) {
	if (b->prev) b->prev->next = b->next; else p_free = b->next;
	if (b->next) b->next->prev = b->prev;
}

static void list_push(fblk_t *b) {
	b->prev = NULL;
	b->next = p_free;
	if (p_free) p_free->prev = b;
	p_free = b;
}

void pool_init(void *mem, size_t bytes) {
	uintptr_t a = ((uintptr_t)mem + 7) & ~(uintptr_t)7;
	size_t lost = a - (uintptr_t)mem;
	p_free = NULL;
	p_base = NULL;
	p_size = 0;
	if (bytes <= lost + MINB + 8) return;
	bytes = (bytes - lost) & ~(size_t)7;
	if (bytes > 0x7FFFFFF8u) bytes = 0x7FFFFFF8u;
	p_base = (uint8_t *)a;
	p_size = bytes;
	set_block(p_base, (uint32_t)bytes, false);
	list_push((fblk_t *)p_base);
}

static uint32_t need_of(size_t n) {
	size_t t = (n + HDR + FTR + 7) & ~(size_t)7;
	if (t < MINB) t = MINB;
	if (t > 0x7FFFFFF0u) return 0;
	return (uint32_t)t;
}

void *pool_alloc(size_t n) {
	uint32_t need = need_of(n);
	fblk_t *b;
	if (!need) return NULL;
	for (b = p_free; b; b = b->next) {
		uint32_t sz = bsize(b);
		if (sz < need) continue;
		list_remove(b);
		if (sz - need >= MINB) {
			fblk_t *rest = (fblk_t *)((uint8_t *)b + need);
			set_block(rest, sz - need, false);
			list_push(rest);
			sz = need;
		}
		set_block(b, sz, true);
		return (uint8_t *)b + HDR;
	}
	return NULL;
}

void *pool_zalloc(size_t n) {
	void *p = pool_alloc(n);
	if (p) memset(p, 0, n);
	return p;
}

void pool_free(void *p) {
	uint8_t *b;
	uint32_t sz;
	if (!p) return;
	b = (uint8_t *)p - HDR;
	sz = bsize(b);
	// merge with the block after
	if (b + sz < p_base + p_size) {
		uint8_t *n = b + sz;
		if (!bused(n)) {
			list_remove((fblk_t *)n);
			sz += bsize(n);
		}
	}
	// and the one before
	if (b > p_base) {
		uint32_t pf = *(uint32_t *)(b - FTR);
		if (!(pf & 1u)) {
			uint8_t *pv = b - (pf & ~7u);
			list_remove((fblk_t *)pv);
			sz += bsize(pv);
			b = pv;
		}
	}
	set_block(b, sz, false);
	list_push((fblk_t *)b);
}

void *pool_realloc(void *p, size_t n) {
	uint8_t *b;
	size_t have;
	void *q;
	if (!p) return pool_alloc(n);
	b = (uint8_t *)p - HDR;
	have = bsize(b) - HDR - FTR;
	if (n <= have) return p;
	// grow in place into a free block after it
	{
		uint32_t need = need_of(n);
		uint8_t *nx = b + bsize(b);
		if (need && nx < p_base + p_size && !bused(nx) && bsize(b) + bsize(nx) >= need) {
			uint32_t total = bsize(b) + bsize(nx);
			list_remove((fblk_t *)nx);
			if (total - need >= MINB) {
				fblk_t *rest = (fblk_t *)(b + need);
				set_block(rest, total - need, false);
				list_push(rest);
				total = need;
			}
			set_block(b, total, true);
			return p;
		}
	}
	q = pool_alloc(n);
	if (!q) return NULL;
	memcpy(q, p, have);
	pool_free(p);
	return q;
}

char *pool_strndup(const char *s, size_t n) {
	char *d = pool_alloc(n + 1);
	if (!d) return NULL;
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

char *pool_strdup(const char *s) {
	return s ? pool_strndup(s, strlen(s)) : NULL;
}

size_t pool_free_bytes(void) {
	size_t t = 0;
	fblk_t *b;
	for (b = p_free; b; b = b->next) t += bsize(b);
	return t;
}

size_t pool_largest(void) {
	size_t m = 0;
	fblk_t *b;
	for (b = p_free; b; b = b->next) if (bsize(b) > m) m = bsize(b);
	return m > HDR + FTR ? m - HDR - FTR : 0;
}

size_t pool_size(void) { return p_size; }

bool pool_check(void) {
	uint8_t *b = p_base;
	size_t nfree = 0, lfree = 0;
	fblk_t *f;
	bool prev_free = false;
	if (!p_base) return true;
	while (b < p_base + p_size) {
		uint32_t sz = bsize(b);
		if (sz < MINB || (sz & 7) || b + sz > p_base + p_size) return false;
		if (*footer(b) != ((fblk_t *)b)->size) return false;
		if (!bused(b)) {
			if (prev_free) return false;	// two free neighbours: not merged
			nfree++;
		}
		prev_free = !bused(b);
		b += sz;
	}
	for (f = p_free; f; f = f->next) {
		if (bused(f)) return false;
		if (++lfree > nfree) return false;
	}
	return lfree == nfree;
}
