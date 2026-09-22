/*
 * The jumploader at Z_JUMP_FLASH_OFFSET: reading it and re-pointing it.
 * docs/zboot.md sec. 5.
 *
 * Only flashapi.h's functions touch the hardware, so the host test
 * (sw/apps/zfpga/tests/kjump.c) links this very file against a
 * simulated flash.
 */
#include <stdint.h>
#include <stdio.h>

#include "flashapi.h"
#include "mem.h"			// k_mem_alloc(): see JSECT below
#include "../common/zsoc.h"
#include "../common/zjump.h"

/* -- the jumploader ----------------------------------------------------------
 *
 * sw/common/zjump.c does the bit-level work; this gives it the flash.
 * Reads come from the memory-mapped window, except from the (at most
 * two) sectors being rewritten, which are loaded into RAM first and
 * patched there. Then each is erased and programmed back -- blank pages
 * skipped -- and the jumploader is read again to check.
 *
 * A power cut between the erase and the program leaves the jumploader
 * broken. The machine still recovers -- power-on is address 0, the
 * bootloader, then Zeitlos -- but reboot and jump then refuse, saying
 * so, until it is rewritten (`make flash_jump`, or a release image).
 */


#define JSECT 4096u
// The two sector buffers come from the kernel's memory pool, and only
// while re-pointing: in .bss they would be 8 KB of kernel.bin, which is
// within a few KB of its 256 KB region. NOT malloc(): the kernel's
// _sbrk() grows up from _end towards the stack pointer -- from the
// serial shell that is the kernel's own stack, with no room, and from a
// syscall it is the APP's stack, so malloc() "succeeds" by growing into
// the memory the kernel stack grows down into.
//
// __attribute__((section(".bss"))): these are reached from syscalls,
// which run with the calling app's gp -- a small global the linker
// relaxed to gp-relative would resolve through the wrong gp. See
// mem.c's comment on its metadata pool.
static uint8_t (* __attribute__((section(".bss"))) jbuf)[JSECT];
static uint32_t __attribute__((section(".bss"))) jsec[2];
static int __attribute__((section(".bss"))) njsec;

static uint8_t jrd(void *ctx, uint32_t off) {
	uint32_t a = Z_JUMP_FLASH_OFFSET + off;
	int k;
	(void)ctx;
	for (k = 0; k < njsec; k++)
		if (a >= jsec[k] && a < jsec[k] + JSECT) return jbuf[k][a - jsec[k]];
	return k_flash_window(a);
}

static void jwr(void *ctx, uint32_t off, uint8_t v) {
	uint32_t a = Z_JUMP_FLASH_OFFSET + off;
	int k;
	(void)ctx;
	for (k = 0; k < njsec; k++)
		if (a >= jsec[k] && a < jsec[k] + JSECT) { jbuf[k][a - jsec[k]] = v; return; }
}

static int jwait(void) {
	uint32_t st, n = 0;
	while (((st = k_flash_hw_status()) & Z_SPIFLASH_BUSY) && n < 3000000u) n++;
	return (st & Z_SPIFLASH_BUSY) ? -1 : 0;
}

static int jparse(zjump_t *j) {
	njsec = 0;
	return zjump_parse(jrd, 0, 4096, j);
}

/* A bitstream at the region's start with its header stripped: the
 * preamble within the first bytes, but not the FF 00 header, so no
 * ZJUMP1 line. openFPGALoader does exactly this to a .bit it writes to
 * flash -- it writes only what follows the header -- which is why the
 * jumploader is flashed as a .bin (`make flash_jump`). */
static int stripped(void) {
	uint32_t o;
	njsec = 0;
	for (o = 0; o < 64; o++)
		if (jrd(0, o) == 0xFF && jrd(0, o + 1) == 0xFF && jrd(0, o + 2) == 0xBD && jrd(0, o + 3) == 0xB3)
			return 1;
	return 0;
}

void k_jump_explain(const char *who, int rc) {
	if (rc == -2)
		printf("%s: 0x%06lx holds a jumploader whose header was stripped -- "
			"a .bit written by openFPGALoader loses it, and with it the ZJUMP1 "
			"line this needs. Write the raw jump.bin instead: `make flash_jump`\n",
			who, (unsigned long)Z_JUMP_FLASH_OFFSET);
	else
		printf("%s: no jumploader at 0x%06lx (`make flash_jump` writes one)\n",
			who, (unsigned long)Z_JUMP_FLASH_OFFSET);
}

int k_jump_read(uint32_t *target) {
	zjump_t j;
	if (jparse(&j)) return stripped() ? -2 : -1;
	if (target) *target = zjump_target(&j, jrd, 0);
	return 0;
}

int k_jump_point(uint32_t target) {
	zjump_t j;
	uint32_t first, last, s, now;
	int k, rc = 0;

	if (target & 0xFFFFu || target > 0xFF0000u) {
		printf("jump: 0x%06lx is not a boot address (64 KB aligned, below 16 MB)\n", (unsigned long)target);
		return -1;
	}
	if (jparse(&j)) {
		k_jump_explain("jump", stripped() ? -2 : -1);
		return -1;
	}
	if (zjump_target(&j, jrd, 0) == target) return 0;
	if (!k_flash_present()) {
		printf("jump: the flash is not writable on this bitstream\n");
		return -1;
	}
	zjump_span(&j, &first, &last);
	first += Z_JUMP_FLASH_OFFSET;
	last += Z_JUMP_FLASH_OFFSET;
	if ((last & ~(JSECT - 1)) - (first & ~(JSECT - 1)) > JSECT) {
		printf("jump: the jumploader's address spans more than two sectors\n");
		return -1;
	}
	if (!(jbuf = k_mem_alloc(2 * JSECT))) {
		printf("jump: no memory for the sectors to rewrite\n");
		return -1;
	}
	if (!k_flash_begin(K_FLASH_KERNEL)) {
		printf("jump: another program is writing the flash\n");
		k_mem_free(jbuf);
		jbuf = 0;
		return -1;
	}

	// load the sectors, patch them in RAM
	njsec = 0;
	for (s = first & ~(JSECT - 1); s <= last; s += JSECT) {
		jsec[njsec] = s;
		for (k = 0; k < (int)JSECT; k++) jbuf[njsec][k] = k_flash_window(s + k);
		njsec++;
	}
	zjump_set(&j, target, jrd, jwr, 0);

	// write them back
	for (k = 0; k < njsec && !rc; k++) {
		uint32_t p;
		if (k_flash_hw_erase(jsec[k]) || jwait()) { rc = -1; break; }
		for (p = 0; p < JSECT; p += 256) {
			uint32_t i;
			for (i = 0; i < 256 && jbuf[k][p + i] == 0xFF; i++) ;
			if (i == 256) continue;
			if (k_flash_hw_program(jsec[k] + p, &jbuf[k][p], 256) || jwait()) { rc = -1; break; }
		}
	}
	k_flash_end(K_FLASH_KERNEL);
	njsec = 0;              // reads come from the flash again
	k_mem_free(jbuf);
	jbuf = 0;

	// and read it back from the flash
	if (!rc && (jparse(&j) || (now = zjump_target(&j, jrd, 0)) != target)) rc = -1;
	if (rc) printf("jump: rewriting the jumploader failed; it may now be broken\n");
	return rc;
}
