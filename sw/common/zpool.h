#ifndef ZPOOL_H
#define ZPOOL_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpool -- one memory pool for everything an app holds: in slide the
 * deck's objects, strings, fonts, images, glyph cache and undo history;
 * in print the job, its fonts and pictures, and the bands.
 *
 * The pool is a single block (on the board: the process's arena, set
 * once at startup and never moved, so pointers into it stay good). A
 * first-fit free list with boundary tags, coalescing on free. Not fast
 * in theory, fast enough in practice: a deck is a few thousand blocks.
 *
 * docs/slide_app.md, "Memory"; docs/print.md, "Memory".
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void pool_init(void *mem, size_t bytes);

// NULL when there is no room. Blocks are 8-byte aligned.
void *pool_alloc(size_t n);
void *pool_zalloc(size_t n);
void pool_free(void *p);
// Like realloc: keeps min(old, new) bytes; NULL (and p untouched) on failure.
void *pool_realloc(void *p, size_t n);
char *pool_strdup(const char *s);
char *pool_strndup(const char *s, size_t n);

size_t pool_free_bytes(void);	// total free
size_t pool_largest(void);		// largest free block
size_t pool_size(void);

// Walks every block and checks the tags agree; for the tests.
bool pool_check(void);

#endif
