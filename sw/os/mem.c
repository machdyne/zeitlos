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

// Allocate a new metadata block from the fixed pool
static k_mem_block_t* alloc_metadata(void) {
    if (mem_block_count >= Z_MEM_MAX_BLOCKS)
        return NULL;
    return &mem_blocks[mem_block_count++];
}

uint32_t k_mem_align_up(uint32_t val, uint32_t align) {
	return (val + align - 1) & ~(align - 1);
}

void k_mem_init(uint32_t total_size) {
    mem_block_count = 0;

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

void k_mem_free(void* ptr) {
    uint32_t addr = (uint32_t)(uintptr_t)ptr;
    k_mem_block_t* blk = block_list;
    k_mem_block_t* prev = NULL;

    while (blk) {
        if (blk->used && blk->start == addr) {
            blk->used = false;

            // Try coalescing with next block if free
            if (blk->next && !blk->next->used) {
                blk->size += blk->next->size;
                blk->next = blk->next->next;
            }

            // Try coalescing with previous block if free
            if (prev && !prev->used) {
                prev->size += blk->size;
                prev->next = blk->next;
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
z_rv k_mem_dump(void) {

	uint32_t total = 0, used = 0, largest_free = 0;
	int used_blocks = 0, free_blocks = 0;

	k_mem_block_t *blk = block_list;
	while (blk) {
		total += blk->size;
		if (blk->used) {
			used += blk->size;
			used_blocks++;
		} else {
			free_blocks++;
			if (blk->size > largest_free) largest_free = blk->size;
		}
		blk = blk->next;
	}

	uint32_t free_total = total - used;

	printf(" total: %6ld KB\n", (long)(total / 1024));
	printf("  used: %6ld KB (%d block%s)\n",
		(long)(used / 1024), used_blocks, used_blocks == 1 ? "" : "s");
	printf("  free: %6ld KB (%d block%s, largest %ld KB)\n",
		(long)(free_total / 1024), free_blocks, free_blocks == 1 ? "" : "s",
		(long)(largest_free / 1024));
	printf("  meta: %d/%d block descriptors in use\n",
		mem_block_count, Z_MEM_MAX_BLOCKS);

	return Z_OK;

}
