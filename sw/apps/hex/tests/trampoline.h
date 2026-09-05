#ifndef HEX_TESTS_TRAMPOLINE_H
#define HEX_TESTS_TRAMPOLINE_H

/*
 * A syscall trampoline for host builds of sw/apps/hex's tests.
 *
 * -- why this is needed --
 *
 * zrender.h makes an app's DRAWING run on a host by mapping real memory
 * at VRAM's fixed address. Drawing is not the only thing an app does at
 * a fixed address: a message send is an indirect call through
 * `reg_kernel`, a pointer the kernel plants at 0x0000000c (zeitlos.h).
 *
 * That never mattered while renders only called layout and repaint. It
 * matters here because hex's EDIT path sends messages -- put_byte()
 * updates the modified marker, which calls z_win_set_title() -- so any
 * host test that types a byte dereferences a null function pointer and
 * dies before asserting anything.
 *
 * The fix is the same trick zrender.h already uses, applied to the
 * other fixed address the runtime assumes: map a page at 0 and put a
 * stub function pointer there. The app's real message-sending code then
 * runs unmodified and the calls land here.
 *
 * Kept in sw/apps/hex/tests rather than folded into zrender.h: no other
 * app's render needs it today, and a helper nothing calls produces a
 * -Wunused-function warning in every render that includes the header.
 * Worth promoting if a second app wants it.
 *
 * -- two host requirements --
 *
 * Both are CHECKED rather than assumed, because each fails as a wild
 * jump into unmapped memory rather than as an error:
 *
 *   - vm.mmap_min_addr must allow mapping page 0. Distributions set it
 *     to 4096 so null-pointer bugs stay fatal, which is a good default
 *     and exactly what is in the way:
 *
 *         sudo sysctl -w vm.mmap_min_addr=0
 *
 *   - The build must be NON-PIE (-no-pie). reg_kernel is a uint32_t,
 *     so the stub's address is truncated to 32 bits on the way in;
 *     under the usual PIE layout the host's text sits well above 4GB
 *     and the truncated value points at nothing. -no-pie puts it near
 *     0x400000, which survives the round trip.
 *
 * A caller that cannot meet either should exit 77 so CI skips rather
 * than fails, the same convention zrender.h uses.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "zobj.h"
#include "zeitlos.h"

static z_obj_t z_tramp_ok, z_tramp_fail;

// MSG_READ answers "mailbox empty". Anything else and a drain loop
// (pump_redraws(), z_msg_wait()) would never terminate.
static uint32_t *z_tramp_syscall(uint32_t id, uint32_t *args, uint32_t b) {

	(void)args;
	(void)b;

	if (id == Z_SYS_MSG_READ) return (uint32_t *)&z_tramp_fail;

	return (uint32_t *)&z_tramp_ok;

}

static bool z_tramp_install(void) {

	if ((uintptr_t)(void *)z_tramp_syscall > 0xFFFFFFFFu) {
		printf("trampoline: build with -no-pie (stub at %p does not fit "
			"in reg_kernel's uint32_t)\n", (void *)z_tramp_syscall);
		return false;
	}

	void *page = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	if (page == MAP_FAILED) {
		printf("trampoline: cannot map page 0 -- needs "
			"vm.mmap_min_addr=0\n");
		return false;
	}

	z_tramp_ok.type = Z_UINT32;
	z_tramp_ok.val.uint32 = Z_OK;
	z_tramp_fail.type = Z_UINT32;
	z_tramp_fail.val.uint32 = Z_FAIL;

	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)z_tramp_syscall;

	return true;

}

#endif
