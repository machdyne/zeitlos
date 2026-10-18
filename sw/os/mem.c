/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Memory management.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#include "kernel.h"
#include "../common/zeitlos.h"
#include "../common/zproc.h"
#include "mem.h"

volatile static __attribute__((section(".bss")))
	k_mem_block_t* block_list = NULL; // Start of memory block list

// Metadata pool
//
// mem_block_count in particular -- a tiny static int, checked on
// every single k_mem_alloc() call via alloc_metadata() -- needs the
// same __attribute__((section(".bss"))) treatment kernel.c's
// z_pid/z_procs[]/z_kernel_ticks already have, and block_list just
// above already has, for the same reason documented at that first
// site: "__global_pointer$ will be wrong in the interrupt handler".
// More precisely (having chased this down for real this time): it's
// wrong whenever kernel code is reached via a SYSCALL specifically
// (an app calling through reg_kernel is a plain jalr from the app's
// own compiled code -- gp isn't part of the C ABI's caller/
// callee-saved convention, so nothing changes it, and kernel code
// ends up executing with whatever gp the calling app had, not the
// kernel's own). The scheduler's own context-switch path (real
// hardware IRQs, sw/bios/boot_picorv32.S's irq_vec) is NOT affected
// the same way -- it saves/restores all 32 GPRs, gp included, so
// each process's own gp survives a preemption correctly -- but a
// syscall is a different mechanism entirely (see docs/app_runtime.md,
// "The syscall trampoline"), with no such save/restore. Without this
// tag, a small enough global here can get linker-relaxed into a
// gp-relative load/store, which resolves through whichever process's
// gp happens to still be loaded -- silently reading/writing the
// WRONG process's memory instead of trapping or erroring. This one
// was reachable only from sh.c (kernel code, always correct gp) until
// Z_SYS_PROC_RUN (see k_proc_run() in kernel.c) gave an app a way to
// reach k_mem_alloc() for the first time, via wm's dock -- see
// docs/window_manager.md's "The dock". mem_blocks[] itself likely
// isn't small enough to be at real risk (Z_MEM_MAX_BLOCKS * sizeof
// (k_mem_block_t) is a few KB), but it's tagged the same way anyway,
// for the same reason block_list already was: not worth leaving to
// chance based on a size threshold that depends on the toolchain's
// own small-data cutoff, which nothing here actually pins down.
static __attribute__((section(".bss"))) k_mem_block_t mem_blocks[Z_MEM_MAX_BLOCKS];
static __attribute__((section(".bss"))) int mem_block_count = 0;

// Descriptors given back. The pool used to be take-only: a free that
// coalesced unlinked a neighbour's descriptor from block_list and
// nothing ever used it again, while mem_block_count only grew. Every
// k_mem_alloc() that splits a free block takes one, so after 255
// allocations since boot -- each app launch is one -- alloc_metadata()
// failed for good and nothing could be started any more, however much
// memory was free (sw/os/tests/test_mem.c, "churn"). Unlinked
// descriptors now go on this list, chained through `next`, and are
// taken from it first.
static __attribute__((section(".bss"))) k_mem_block_t *mem_desc_free = NULL;
static __attribute__((section(".bss"))) int mem_desc_live = 0;	// in block_list

// Allocate a new metadata block from the fixed pool
static k_mem_block_t* alloc_metadata(void) {
    k_mem_block_t *d;
    if (mem_desc_free) {
        d = mem_desc_free;
        mem_desc_free = d->next;
    } else {
        if (mem_block_count >= Z_MEM_MAX_BLOCKS)
            return NULL;
        d = &mem_blocks[mem_block_count++];
    }
    mem_desc_live++;
    return d;
}

// A descriptor that has just been unlinked from block_list.
static void release_metadata(k_mem_block_t *d) {
    d->used = false;
    d->size = 0;
    d->next = mem_desc_free;
    mem_desc_free = d;
    mem_desc_live--;
}

uint32_t k_mem_align_up(uint32_t val, uint32_t align) {
	return (val + align - 1) & ~(align - 1);
}

void k_mem_init(uint32_t total_size) {
    mem_block_count = 0;
    mem_desc_free = NULL;
    mem_desc_live = 0;

    k_mem_block_t* first = alloc_metadata();
    first->start = Z_MEM_BASE;
    first->size = total_size;
    first->used = false;
    first->next = NULL;

    block_list = first;
}

void* k_mem_alloc(uint32_t size) {
    size = k_mem_align_up(size, Z_MEM_ALIGNMENT);
    if (size < Z_MEM_MIN_BLOCK_SIZE)
        size = Z_MEM_MIN_BLOCK_SIZE;

    k_mem_block_t* prev = NULL;
    k_mem_block_t* blk = block_list;

    while (blk) {
        if (!blk->used) {
            uint32_t aligned_start = k_mem_align_up(blk->start, Z_MEM_ALIGNMENT);
            uint32_t padding = aligned_start - blk->start;
            uint32_t total_required = size + padding;

            if (blk->size >= total_required) {
                // If padding > 0, create a free block for it before blk
                if (padding > 0) {
                    k_mem_block_t* pre = alloc_metadata();
                    if (!pre) return NULL;

                    pre->start = blk->start;
                    pre->size = padding;
                    pre->used = false;
                    pre->next = blk;

                    if (prev)
                        prev->next = pre;
                    else
                        block_list = pre;

                    blk->start = aligned_start;
                    blk->size -= padding;

                    prev = pre;
                }

                // Split remaining free space after allocated block
                if (blk->size > size) {
                    k_mem_block_t* rem = alloc_metadata();
                    if (!rem) return NULL;

                    rem->start = blk->start + size;
                    rem->size = blk->size - size;
                    rem->used = false;
                    rem->next = blk->next;

                    blk->next = rem;
                    blk->size = size;
                }

                blk->used = true;
                return (void*)(uintptr_t)blk->start;
            }
        }
        prev = blk;
        blk = blk->next;
    }

    return NULL; // Out of memory
}

// -- for arenas (Z_SYS_ARENA, kernel.c; docs/kernel.md, "Arenas") --

// Descriptors that can still be taken, from the free list or the table.
static int meta_avail(void) {
    int n = Z_MEM_MAX_BLOCKS - mem_block_count;
    for (k_mem_block_t *d = mem_desc_free; d; d = d->next) n++;
    return n;
}

static k_mem_block_t *find_used(uint32_t addr, k_mem_block_t **prevp) {
    k_mem_block_t *prev = NULL;
    for (k_mem_block_t *b = (k_mem_block_t *)block_list; b; prev = b, b = b->next)
        if (b->used && b->start == addr) {
            if (prevp) *prevp = prev;
            return b;
        }
    return NULL;
}

// From the TOP of the highest free block that holds `size`: arenas are
// placed from the top of the pool down while process images are placed
// from the bottom up, so the two do not interleave and leave each other
// holes. `size` is rounded up to Z_MEM_ALIGNMENT; there is no 32KB
// minimum, an arena is as small as it is.
void *k_mem_alloc_top(uint32_t size) {
    size = k_mem_align_up(size ? size : 1, Z_MEM_ALIGNMENT);

    k_mem_block_t *best = NULL, *bprev = NULL, *prev = NULL;
    for (k_mem_block_t *b = (k_mem_block_t *)block_list; b; prev = b, b = b->next) {
        if (b->used) continue;
        uint32_t top = (b->start + b->size) & ~(uint32_t)(Z_MEM_ALIGNMENT - 1);
        if (top >= b->start + size) { best = b; bprev = prev; }
    }
    if (!best) return NULL;

    uint32_t end = best->start + best->size;
    uint32_t top = end & ~(uint32_t)(Z_MEM_ALIGNMENT - 1);
    uint32_t start = top - size;
    int need = (start > best->start) + (end > top);
    if (meta_avail() < need) return NULL;

    k_mem_block_t *blk = best;
    if (start > best->start) {
        // best keeps the free space below; a new descriptor is the block
        blk = alloc_metadata();
        blk->start = start;
        blk->size = end - start;
        blk->next = best->next;
        best->size = start - best->start;
        best->next = blk;
    }
    (void)bprev;
    if (end > top) {
        k_mem_block_t *t = alloc_metadata();
        t->start = top;
        t->size = end - top;
        t->used = false;
        t->next = blk->next;
        blk->next = t;
    }
    blk->start = start;
    blk->size = size;
    blk->used = true;
    return (void *)(uintptr_t)start;
}

// The size of the used block at `ptr`, or 0.
uint32_t k_mem_block_size(void *ptr) {
    k_mem_block_t *b = find_used((uint32_t)(uintptr_t)ptr, NULL);
    return b ? b->size : 0;
}

// Resizes the used block at `ptr` IN PLACE if it can, keeping its
// start: always when shrinking -- the tail goes back to the pool,
// joining a free neighbour -- and when growing if the block after it is
// free and large enough. Returns false, changing nothing, if not.
bool k_mem_resize(void *ptr, uint32_t size) {
    k_mem_block_t *b = find_used((uint32_t)(uintptr_t)ptr, NULL);
    if (!b || !size) return false;
    size = k_mem_align_up(size, Z_MEM_ALIGNMENT);
    if (size == b->size) return true;

    k_mem_block_t *n = b->next;
    if (size < b->size) {
        uint32_t tail = b->size - size;
        if (n && !n->used) {
            n->start -= tail;
            n->size += tail;
        } else {
            if (meta_avail() < 1) return false;
            k_mem_block_t *t = alloc_metadata();
            t->start = b->start + size;
            t->size = tail;
            t->used = false;
            t->next = n;
            b->next = t;
        }
        b->size = size;
        return true;
    }

    uint32_t extra = size - b->size;
    if (!n || n->used || n->size < extra) return false;
    if (n->size == extra) {
        b->next = n->next;
        release_metadata(n);
    } else {
        n->start += extra;
        n->size -= extra;
    }
    b->size = size;
    return true;
}

// Grows the used block at `ptr` DOWNWARD into a free block below it,
// keeping its end: the new start is returned, and the caller moves the
// contents down to it (they are still at `ptr`). 0 if the free block
// below is missing or too small.
uint32_t k_mem_grow_down(void *ptr, uint32_t size) {
    k_mem_block_t *prev;
    k_mem_block_t *b = find_used((uint32_t)(uintptr_t)ptr, &prev);
    if (!b || !prev || prev->used) return 0;
    size = k_mem_align_up(size, Z_MEM_ALIGNMENT);
    if (size <= b->size) return 0;
    uint32_t extra = size - b->size;
    if (prev->size < extra) return 0;
    if (prev->size == extra) {
        // prev is used up: find the one before it to unlink it
        k_mem_block_t *pp = NULL;
        for (k_mem_block_t *x = (k_mem_block_t *)block_list; x && x != prev; x = x->next) pp = x;
        if (pp) pp->next = b;
        else block_list = b;
        release_metadata(prev);
    } else
        prev->size -= extra;
    b->start -= extra;
    b->size = size;
    return b->start;
}

// Bytes in free blocks, all together.
uint32_t k_mem_free_bytes(void) {
    uint32_t n = 0;
    for (k_mem_block_t *b = (k_mem_block_t *)block_list; b; b = b->next)
        if (!b->used) n += b->size;
    return n;
}

void k_mem_free(void* ptr) {
    uint32_t addr = (uint32_t)(uintptr_t)ptr;
    k_mem_block_t* blk = block_list;
    k_mem_block_t* prev = NULL;

    while (blk) {
        if (blk->used && blk->start == addr) {
            blk->used = false;

            // Try coalescing with next block if free
            if (blk->next && !blk->next->used) {
                k_mem_block_t *gone = blk->next;
                blk->size += gone->size;
                blk->next = gone->next;
                release_metadata(gone);
            }

            // Try coalescing with previous block if free
            if (prev && !prev->used) {
                prev->size += blk->size;
                prev->next = blk->next;
                release_metadata(blk);
            }

            return;
        }
        prev = blk;
        blk = blk->next;
    }
}

// prints a summary of the k_mem_alloc() pool -- `free` in sh.c. Added
// specifically to debug a real-hardware "runs out of memory, but no
// error shown" report: k_proc_create()/k_mem_alloc() DO check for and
// report allocation failure already (see k_mem_alloc()'s own `return
// NULL` and k_proc_create()'s `if (!mem) return(0);`, checked by
// every caller in sh.c) -- so a genuinely silent crash suggests
// something is exhausting memory in a way that doesn't go through
// that path cleanly, or memory is tighter than expected in the first
// place (see mem.h's own Z_MEM_SIZE_DEFAULT/docs/csrs.md for how the
// pool's total size is determined). Reports the largest single free
// block separately from total free bytes, since k_mem_alloc() needs
// ONE block big enough, not just enough free bytes in aggregate --
// fragmentation (many small free blocks, no single large one) would
// show up as "plenty of free KB" but still fail every real
// allocation, which total-free alone would hide.
// One walk of the block list, filling in everything both k_mem_dump()
// (below) and k_mem_stats() (below that) report. Factored out when the
// second caller arrived rather than left duplicated: the two would
// otherwise drift the moment anything about the pool's bookkeeping
// changes, and a `free` command that disagrees with itself depending
// on whether you typed it at the kernel shell or from Scheme is
// exactly the kind of thing nobody notices until it matters.
//
// Fills a z_mem_stats_args_t (sw/common/zproc.h) purely because that
// struct already names every field this needs; k_mem_dump() just reads
// the fields back out and prints them.
static void mem_collect(z_mem_stats_args_t *s) {

	s->total = 0;
	s->used = 0;
	s->largest_free = 0;
	s->used_blocks = 0;
	s->free_blocks = 0;

	k_mem_block_t *blk = block_list;
	while (blk) {
		s->total += blk->size;
		if (blk->used) {
			s->used += blk->size;
			s->used_blocks++;
		} else {
			s->free_blocks++;
			if (blk->size > s->largest_free) s->largest_free = blk->size;
		}
		blk = blk->next;
	}

	s->free = s->total - s->used;
	s->blocks_used = (uint32_t)mem_desc_live;
	s->blocks_max = Z_MEM_MAX_BLOCKS;

}

// Z_SYS_MEM_STATS -- the same numbers k_mem_dump() prints, returned to
// a caller instead. See sw/common/zproc.h for why this exists and why
// it reports bytes rather than the KB the dump rounds to.
//
// Note this reports the KERNEL POOL (k_mem_alloc(), this file), which
// is a completely different thing from the per-process heap an app
// grows with sbrk()/malloc() inside its own already-allocated block --
// sw/apps/repl's (free) reports both, clearly separated, precisely
// because conflating them is easy and misleading.
z_obj_t *k_mem_stats(z_obj_t *args) {

	z_mem_stats_args_t *a = (z_mem_stats_args_t *)args;

	if (!a) return (&z_fail);

	mem_collect(a);

	return (&z_ok);

}

z_rv k_mem_dump(void) {

	z_mem_stats_args_t s;
	mem_collect(&s);

	uint32_t total = s.total;
	uint32_t used = s.used;
	uint32_t largest_free = s.largest_free;
	int used_blocks = (int)s.used_blocks;
	int free_blocks = (int)s.free_blocks;
	uint32_t free_total = s.free;

	printf(" total: %6ld KB\n", (long)(total / 1024));
	printf("  used: %6ld KB (%d block%s)\n",
		(long)(used / 1024), used_blocks, used_blocks == 1 ? "" : "s");
	printf("  free: %6ld KB (%d block%s, largest %ld KB)\n",
		(long)(free_total / 1024), free_blocks, free_blocks == 1 ? "" : "s",
		(long)(largest_free / 1024));
	printf("  meta: %d/%d block descriptors in use\n",
		mem_desc_live, Z_MEM_MAX_BLOCKS);

	return Z_OK;

}
