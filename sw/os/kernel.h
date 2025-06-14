#ifndef Z_KERNEL_H
#define Z_KERNEL_H

#define Z_OK   0
#define Z_FAIL 1

#define Z_IRQ_KTIMER			3
#define Z_IRQ_UART			4
#define Z_IRQ_HID				5

typedef uint32_t z_rv;

typedef struct {

	uint32_t		base;
	uint32_t		size;
	uint32_t		flags;
	uint32_t		regs[32];

} z_proc;

#define Z_PROC_FLAG_ACTIVE	0x000000001
#define Z_PROC_FLAG_DIE		0x000000002

// --

uint32_t k_proc_create(uint32_t size);
uint32_t k_proc_base(uint32_t pid);
z_rv k_proc_start(uint32_t pid);
z_rv k_proc_stop(uint32_t pid);
z_rv k_proc_dump(void);
z_rv k_proc_kill(uint32_t pid);
z_rv k_kernel_dump(void);

// --

static inline uint32_t maskirq(uint32_t new_mask) {
    uint32_t old_mask;

    __asm__ volatile (
        ".insn r 0x0B, 0x6, 0x03, %0, %1, zero"
        : "=r"(old_mask)      // output: destination register
        : "r"(new_mask)       // input: source register
        : "memory"
    );

    return old_mask;
}

#endif
