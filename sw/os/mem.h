#ifndef Z_MEM_H
#define Z_MEM_H

#define Z_MEM_BASE			0x40000000
#define Z_MEM_SIZE			(1024 * 1024 * 1)
#define Z_MEM_MAX_BLOCKS	256

#define Z_MEM_ALIGNMENT				4096
#define Z_MEM_MIN_BLOCK_SIZE		32768

typedef struct k_mem_block {

   uint32_t					start;
   uint32_t					size;
   bool						used;
   struct k_mem_block	*next;

} k_mem_block_t;

void k_mem_init(void);
void *k_mem_alloc(uint32_t size);
void k_mem_free(void *ptr);
uint32_t k_mem_align_up(uint32_t val, uint32_t align);

#endif
