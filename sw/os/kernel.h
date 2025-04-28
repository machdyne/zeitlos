#ifndef KERNEL_H
#define KERNEL_H

typedef uint32_t z_rv;

typedef struct {

   uint32_t    addr;
   uint32_t    size;
	uint32_t		flags;
   uint32_t    regs[32];

} z_proc;

#define Z_PROC_FLAG_ACTIVE	0x000000001

// --

z_rv z_proc_create(uint32_t addr, uint32_t size);
z_rv z_proc_dump(void);

#endif
