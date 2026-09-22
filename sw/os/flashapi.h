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

#define K_FLASH_KERNEL 0xFFFFFFFFu               // the session owner when the shell writes
bool k_flash_begin(uint32_t owner);
void k_flash_end(uint32_t owner);
bool k_flash_session_active(void);               // zar.c: no flash-app launches while true
void k_flash_release_pid(uint32_t pid);          // process exit (interrupt path: no I/O)

z_obj_t *k_flash(z_obj_t *args);                 // Z_SYS_FLASH

#endif
