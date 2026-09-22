#ifndef FLASHAPI_H
#define FLASHAPI_H
// The kernel's side of writing the flash: rtl/spiflash.v's registers,
// the one-writer session, and Z_SYS_FLASH. docs/spiflash.md.
#include <stdint.h>
#include <stdbool.h>
#include "../common/zeitlos.h"

// Also used directly by the serial shell's `flash` and `flashtest`.
bool k_flash_present(void);
uint32_t k_flash_hw_status(void);
uint32_t k_flash_hw_erase(uint32_t addr);         // 0 if started, else refusal bits
uint32_t k_flash_hw_program(uint32_t addr, const uint8_t *buf, uint32_t len);

uint8_t k_flash_window(uint32_t off);            // a byte of the flash, through the window

#define K_FLASH_KERNEL 0xFFFFFFFFu               // the session owner when the shell writes
bool k_flash_begin(uint32_t owner);
void k_flash_end(uint32_t owner);
bool k_flash_session_active(void);               // zar.c: no flash-app launches while true
void k_flash_release_pid(uint32_t pid);          // process exit (interrupt path: no I/O)

z_obj_t *k_flash(z_obj_t *args);                 // Z_SYS_FLASH

// The jumploader at Z_JUMP_FLASH_OFFSET (docs/zboot.md sec. 5):
// k_jump_read: 0 and its target if one is there, -1 if not, -2 if there
// is a jumploader whose header was stripped (a .bit through openFPGALoader);
// k_jump_explain: say which, as `who`;
// k_jump_point: re-point it, rewriting the one or two sectors that
// hold its address bits and their CRCs. 0 on success (or if it already
// pointed there); negative, and a message, if not.
int k_jump_read(uint32_t *target);
void k_jump_explain(const char *who, int rc);
int k_jump_point(uint32_t target);

#endif
