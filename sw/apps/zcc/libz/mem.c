/*
 * libz -- memory.
 *
 * A first-fit free list with coalescing, over a heap that starts at
 * the end of the image and grows toward the stack.
 *
 * -- Where the heap is --
 *
 * `libz_heap_start` is a word inside the blob that zcc patches at
 * output time with the image's `_end`. The blob cannot know it: the
 * blob is built once and every program that uses it has a different
 * amount of code and data after it.
 *
 * The top is the CURRENT stack pointer, read fresh on every request,
 * minus a margin. That is the same soft check sw/common/zeitlos.c's
 * _sbrk() makes and it is worth being clear about what it is: with no
 * MMU this protects a process from colliding with ITS OWN stack, and
 * from nothing else. See docs/app_runtime.md, "Trust model".
 *
 * -- Why not a bump allocator --
 *
 * Because free() has to work. A compiler is the motivating client
 * (docs/posix.md, Phase 3) and a compiler allocates and releases
 * per-function structures; a bump allocator would make peak use equal
 * total use, which is the difference between fitting in 4MB and not.
 */

#include "libz.h"
#include "libz_int.h"

#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((size_t)(a) - 1))

/*
 * 512 bytes of headroom between the heap top and the stack pointer.
 *
 * Not a guess at how deep the stack will get -- it cannot be, since
 * the check happens at whatever depth the caller happens to be. It is
 * the margin for the frames between the check and the caller's next
 * few calls, which is what turns "malloc succeeded and then the stack
 * scribbled on it" into "malloc returned NULL".
 */
#define STACK_MARGIN 512

typedef struct block {
    size_t size;            /* payload bytes, not counting this header */
    struct block *next;     /* free list, address-ordered */
    int free;
} block_t;

#define HDR ALIGN_UP(sizeof(block_t), 8)

static block_t *heap_head;
static char *heap_brk;
static size_t heap_total;

static char *stack_ptr(void) {
    register char *sp __asm__("sp");
    return sp;
}

static void *heap_grow(size_t n) {
    char *limit = stack_ptr() - STACK_MARGIN;

    if (!heap_brk) heap_brk = (char *)(unsigned long)libz_heap_start;
    if (!heap_brk) return NULL;

    if (heap_brk + n >= limit) return NULL;

    void *p = heap_brk;
    heap_brk += n;
    heap_total += n;
    return p;
}

size_t z_heap_used(void) { return heap_total; }

void *malloc(size_t n) {

    if (!n) n = 1;
    n = ALIGN_UP(n, 8);

    block_t *prev = NULL;
    for (block_t *b = heap_head; b; prev = b, b = b->next) {
        if (!b->free || b->size < n) continue;

        /* Split only when the remainder can hold a header plus
         * something useful. Splitting off an 8-byte fragment costs a
         * 16-byte header to describe it, which is how a free list ends
         * up with more metadata than memory. */
        if (b->size >= n + HDR + 32) {
            block_t *rest = (block_t *)((char *)b + HDR + n);
            rest->size = b->size - n - HDR;
            rest->free = 1;
            rest->next = b->next;
            b->next = rest;
            b->size = n;
        }
        b->free = 0;
        return (char *)b + HDR;
    }

    block_t *b = heap_grow(HDR + n);
    if (!b) return NULL;
    b->size = n;
    b->free = 0;
    b->next = NULL;
    if (prev) prev->next = b;
    else heap_head = b;
    return (char *)b + HDR;
}

/* Coalescing runs on free rather than on alloc, and forward only.
 * Address order is guaranteed because blocks are only ever appended at
 * the break, so a single forward pass is enough to merge a run of
 * adjacent free blocks. */
void free(void *p) {
    if (!p) return;

    block_t *b = (block_t *)((char *)p - HDR);
    b->free = 1;

    for (block_t *c = heap_head; c; c = c->next) {
        while (c->free && c->next && c->next->free &&
               (char *)c + HDR + c->size == (char *)c->next) {
            c->size += HDR + c->next->size;
            c->next = c->next->next;
        }
    }
}

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    /* The overflow check that makes calloc worth calling instead of
     * malloc(n * sz): a wrapped product allocates a small block and
     * the caller then writes a large one. */
    if (sz && total / sz != n) return NULL;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }

    block_t *b = (block_t *)((char *)p - HDR);
    if (b->size >= n) return p;

    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

void *memcpy(void *d, const void *s, size_t n) {
    char *dp = d;
    const char *sp = s;

    /* Word at a time when both ends are aligned, which is the common
     * case and roughly four times faster on this core. Bytes
     * otherwise: RV32 without misaligned-access support does not fault
     * on picorv32, it silently returns the wrong bytes, which is worse
     * than faulting. */
    if ((((unsigned long)dp | (unsigned long)sp) & 3) == 0) {
        unsigned *dw = (unsigned *)dp;
        const unsigned *sw = (const unsigned *)sp;
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        dp = (char *)dw;
        sp = (const char *)sw;
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    char *dp = d;
    const char *sp = s;
    if (dp == sp || !n) return d;
    if (dp < sp) return memcpy(d, s, n);
    dp += n;
    sp += n;
    while (n--) *--dp = *--sp;
    return d;
}

void *memset(void *d, int c, size_t n) {
    char *dp = d;
    unsigned char v = (unsigned char)c;

    if (((unsigned long)dp & 3) == 0 && n >= 4) {
        unsigned w = (unsigned)v;
        w |= w << 8;
        w |= w << 16;
        unsigned *dw = (unsigned *)dp;
        while (n >= 4) { *dw++ = w; n -= 4; }
        dp = (char *)dw;
    }
    while (n--) *dp++ = (char)v;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}
