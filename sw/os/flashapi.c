/*
 * Writing the flash: the kernel's side of rtl/spiflash.v.
 * docs/spiflash.md.
 *
 * The controller does the dangerous part in hardware -- the command
 * sequences, and refusing anything below the lock. This file adds what
 * hardware cannot:
 *
 *   - one writer at a time: a session, owned by a pid (or by the kernel
 *     shell), released on end or when the owner dies;
 *   - no flash-app launches while a session is open (zar.c asks), since
 *     the archive they are copied from may be half-written;
 *   - the byte-to-word packing of the page buffer.
 *
 * Nothing here waits for an operation to finish. Erase and program
 * start one and return; callers poll the status.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "kernel.h"
#include "flashapi.h"
#include "../common/zsoc.h"
#include "../common/zflash.h"
#include "kvstore.h"

#define REG(o) (*(volatile uint32_t *)(Z_SPIFLASH_BASE + (o)))

// Reached from Z_SYS_FLASH, which runs with the calling app's gp: kept
// out of gp-relative reach, as kernel.c's globals are (see mem.c).
static bool __attribute__((section(".bss"))) session;
static uint32_t __attribute__((section(".bss"))) session_owner;

bool k_flash_present(void) {
	if (!z_soc_has_feature2(Z_FEATURE2_FLASHW)) return false;
	return REG(Z_SPIFLASH_MAGIC) == Z_SPIFLASH_MAGIC_VAL;
}

uint32_t k_flash_hw_status(void) {
	return REG(Z_SPIFLASH_STATUS);
}

// Arm, command, and report what the controller said. The controller
// checks the lock and the arming itself; these only report it.
static uint32_t start(uint32_t cmd) {
	REG(Z_SPIFLASH_ARM) = Z_SPIFLASH_ARM_KEY;
	REG(Z_SPIFLASH_CMD) = cmd;
	return REG(Z_SPIFLASH_STATUS) & Z_SPIFLASH_ERRORS;
}

uint32_t k_flash_hw_erase(uint32_t addr) {
	if (REG(Z_SPIFLASH_STATUS) & Z_SPIFLASH_BUSY) return Z_SPIFLASH_E_BUSY;
	REG(Z_SPIFLASH_ADDR) = addr;
	return start(Z_SPIFLASH_CMD_ERASE);
}

uint32_t k_flash_hw_program(uint32_t addr, const uint8_t *buf, uint32_t len) {
	if (REG(Z_SPIFLASH_STATUS) & Z_SPIFLASH_BUSY) return Z_SPIFLASH_E_BUSY;
	if (len == 0 || len > 256 || (addr & 0xFFu) + len > 256) return Z_SPIFLASH_E_LEN;
	// the buffer's byte i is programmed at addr + i: word i/4, byte
	// lane i%4, little-endian -- as reads return it
	for (uint32_t w = 0; w < (len + 3) / 4; w++) {
		uint32_t v = 0xFFFFFFFFu;
		for (uint32_t b = 0; b < 4 && w * 4 + b < len; b++) {
			v &= ~(0xFFu << (8 * b));
			v |= (uint32_t)buf[w * 4 + b] << (8 * b);
		}
		REG(Z_SPIFLASH_BUF + 4 * w) = v;
	}
	REG(Z_SPIFLASH_ADDR) = addr;
	REG(Z_SPIFLASH_LEN) = len;
	return start(Z_SPIFLASH_CMD_PROG);
}

bool k_flash_begin(uint32_t owner) {
	if (!k_flash_present()) return false;
	if (session && session_owner != owner) return false;
	session = true;
	session_owner = owner;
	return true;
}

void k_flash_end(uint32_t owner) {
	if (session && session_owner == owner) session = false;
}

bool k_flash_session_active(void) {
	return session;
}

// Interrupt path (kernel.c, as a process is reaped): just forget the
// session. An operation still running finishes on its own.
void k_flash_release_pid(uint32_t pid) {
	if (session && session_owner == pid) session = false;
	// the store's write lock, if this process died inside Z_SYS_KV
	k_kv_release_pid(pid);
}

z_obj_t *k_flash(z_obj_t *args) {
	z_flash_args_t *a = (z_flash_args_t *)args;
	if (!a) return &z_fail;
	a->result = 0;
	if (!k_flash_present()) return &z_fail;

	switch (a->op) {
	case Z_FLASH_INFO: {
		uint32_t id = REG(Z_SPIFLASH_ID);
		a->result = id;
		// JEDEC capacity byte: 2^n bytes on Winbond and most others
		a->len = ((id & 0xFFu) >= 16 && (id & 0xFFu) <= 28) ? (1u << (id & 0xFFu)) : 0;
		a->addr = REG(Z_SPIFLASH_LOCK);
		return &z_ok;
	}
	case Z_FLASH_STATUS:
		a->result = REG(Z_SPIFLASH_STATUS);
		return &z_ok;
	case Z_FLASH_BEGIN:
		return k_flash_begin(z_pid) ? &z_ok : &z_fail;
	case Z_FLASH_END:
		k_flash_end(z_pid);
		return &z_ok;
	case Z_FLASH_ERASE:
	case Z_FLASH_PROGRAM:
		// The key/value store is the kernel's (kvstore.c,
		// docs/kvstore.md). Checked before the session, so the refusal
		// is the same whoever asks, and modulo the chip size inside
		// k_kv_overlaps(), because the chip ignores the address bits
		// above its size.
		if (a->op == Z_FLASH_ERASE ? k_kv_overlaps(a->addr & ~0xFFFu, 4096u)
				: k_kv_overlaps(a->addr, a->len)) {
			a->result = Z_FLASH_E_RESERVED;
			return &z_fail;
		}
		if (!session || session_owner != z_pid) {
			a->result = 0xFFFFFFFFu;
			return &z_fail;
		}
		if (a->op == Z_FLASH_ERASE)
			a->result = k_flash_hw_erase(a->addr);
		else if (!a->buf)
			a->result = Z_SPIFLASH_E_LEN;
		else
			a->result = k_flash_hw_program(a->addr, (const uint8_t *)a->buf, a->len);
		return a->result ? &z_fail : &z_ok;
	default:
		return &z_fail;
	}
}

// One byte of the flash, through the memory-mapped window.
uint8_t k_flash_window(uint32_t off) {
	return *(volatile const uint8_t *)(0x10000000u + off);
}
