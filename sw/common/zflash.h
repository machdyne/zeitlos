#ifndef Z_FLASH_H
#define Z_FLASH_H
/*
 * Writing the flash, for apps: Z_SYS_FLASH. docs/spiflash.md.
 *
 * The flash is read through its memory-mapped window as always
 * (0x1000_0000 + offset). Writing goes through the kernel, which owns
 * rtl/spiflash.v's registers and adds two things the hardware cannot:
 *
 *   - a SESSION: one process at a time may erase or program. Begin one,
 *     do the work, end it. A process that exits or is killed releases
 *     its session.
 *   - while a session is open, flash apps are not launched: the archive
 *     they come from may be half-written.
 *
 * What the hardware adds that software cannot get around: nothing below
 * the lock (0x040000, the DFU bootloader) can be erased or programmed,
 * whoever asks. z_flash_erase() and z_flash_program() return the
 * controller's refusal bits (Z_SPIFLASH_E_* in zsoc.h) when it refuses.
 *
 * Operations START and return: poll z_flash_status() for
 * Z_SPIFLASH_BUSY to clear -- a 4 KB erase takes 45 ms typically and up
 * to 400 ms, a page program under 3 ms -- from your event loop rather
 * than spinning.
 */
#include <stdint.h>
#include <stdbool.h>
#include "zeitlos.h"
#include "zsoc.h"

enum {
	Z_FLASH_INFO = 0,       // -> result = JEDEC ID, len = size in bytes, addr = lock end
	Z_FLASH_STATUS = 1,     // -> result = rtl/spiflash.v's STATUS
	Z_FLASH_BEGIN = 2,      // claim the session
	Z_FLASH_END = 3,        // release it
	Z_FLASH_ERASE = 4,      // addr: erase the 4 KB sector containing it
	Z_FLASH_PROGRAM = 5,    // addr, buf, len (1..256, within one 256-byte page)
};

// Refused by the kernel rather than the controller: the range holds the
// key/value store (the last Z_KV_SIZE bytes of the chip, zsoc.h), which
// only the kernel writes -- docs/kvstore.md. Returned in `result` by
// z_flash_erase() and z_flash_program(), like the controller's bits.
#define Z_FLASH_E_RESERVED   (1u << 16)

typedef struct {
	uint32_t op;
	uint32_t addr;          // a flash offset, not a window address
	const void *buf;
	uint32_t len;
	uint32_t result;
} z_flash_args_t;

static inline bool z_flash_call(z_flash_args_t *a) {
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)k(Z_SYS_FLASH, (uint32_t *)a, 0);
	return rv && rv->val.uint32 == Z_OK;
}

// false if this bitstream's flash cannot be written (FEATURES2 bit 6)
static inline bool z_flash_info(uint32_t *id, uint32_t *size, uint32_t *lock_end) {
	z_flash_args_t a = { Z_FLASH_INFO, 0, 0, 0, 0 };
	if (!z_flash_call(&a)) return false;
	if (id) *id = a.result;
	if (size) *size = a.len;
	if (lock_end) *lock_end = a.addr;
	return true;
}

static inline uint32_t z_flash_status(void) {
	z_flash_args_t a = { Z_FLASH_STATUS, 0, 0, 0, 0 };
	z_flash_call(&a);
	return a.result;
}

static inline bool z_flash_begin(void) {
	z_flash_args_t a = { Z_FLASH_BEGIN, 0, 0, 0, 0 };
	return z_flash_call(&a);
}

static inline void z_flash_end(void) {
	z_flash_args_t a = { Z_FLASH_END, 0, 0, 0, 0 };
	z_flash_call(&a);
}

// 0 if started; otherwise the refusal bits, or ~0 if there is no session
static inline uint32_t z_flash_erase(uint32_t addr) {
	z_flash_args_t a = { Z_FLASH_ERASE, addr, 0, 0, 0 };
	z_flash_call(&a);
	return a.result;
}

static inline uint32_t z_flash_program(uint32_t addr, const void *buf, uint32_t len) {
	z_flash_args_t a = { Z_FLASH_PROGRAM, addr, buf, len, 0 };
	z_flash_call(&a);
	return a.result;
}

#endif
