#ifndef Z_MEM_H
#define Z_MEM_H

#define Z_MEM_BASE			0x40000000

// fallback/default total pool size, used only if the SOC capability
// CSRs (rtl/csrs.v, sw/common/zsoc.h, docs/csrs.md) aren't present in
// the running bitstream -- an older build that predates rtl/csrs.v
// entirely has no way to report its real amount of main RAM, so this
// keeps k_mem_init()'s caller (kernel.c) working exactly as before on
// those builds: 1MB matched every board's actual RAM before `MEM
// (rtl/boards.vh) existed, since Obst (1MB) was the only board this
// ever ran on at the time. On any build WITH CSRs, kernel.c reads the
// real per-board total (Lakritz/mozart_ml1: 32MB, some boards more)
// instead of this constant -- see k_mem_init()'s own comment below.
#define Z_MEM_SIZE_DEFAULT	(1024 * 1024 * 1)

#define Z_MEM_MAX_BLOCKS	256

#define Z_MEM_ALIGNMENT				4096
#define Z_MEM_MIN_BLOCK_SIZE		32768

typedef struct k_mem_block {

   uint32_t					start;
   uint32_t					size;
   bool						used;
   struct k_mem_block	*next;

} k_mem_block_t;

// `total_size` is the actual size of the pool to manage, in bytes --
// see this file's own Z_MEM_SIZE_DEFAULT comment and kernel.c's own
// call site for where that number comes from (a CSR read when
// available, that constant otherwise).
void k_mem_init(uint32_t total_size);
void *k_mem_alloc(uint32_t size);
void k_mem_free(void *ptr);
uint32_t k_mem_align_up(uint32_t val, uint32_t align);
z_rv k_mem_dump(void);	// `free` in sh.c -- see its own comment in mem.c

#endif
