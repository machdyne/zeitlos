/*
 * Included ahead of upd.c in the host tests (tests/Makefile, -include):
 * upd.c's allocations go through a counting allocator, so a test can
 * see the peak heap a run needs and make one fail when it passes a
 * limit -- the board's heap is the app's APP_STACK, less its stack.
 * Host malloc never says no, which is how "out of memory writing the
 * manifest" reached a board.
 */
#ifndef ALLOC_HOOK_H
#define ALLOC_HOOK_H
#include <stddef.h>
void *t_malloc(size_t n);
void *t_calloc(size_t n, size_t m);
void *t_realloc(void *p, size_t n);
void t_free(void *p);
#define malloc t_malloc
#define calloc t_calloc
#define realloc t_realloc
#define free t_free
#endif
