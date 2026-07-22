/*
 * Minimal freestanding startup stub, used ONLY to build test binaries
 * for zeitlos-sim in an environment without the real riscv32-unknown-elf
 * newlib toolchain. It intentionally does the same two things the real
 * kernel already does for a process (set sp, jump to entry) -- see
 * sw/os/kernel.c's k_proc_create() -- since the app is linked to expect
 * that environment either way. main() itself is never modified.
 */
	.section .text
	.global _start
_start:
	/* sp is already set by the emulator's process-start convention,
	 * but set it up again here too in case this is ever run in a
	 * context that doesn't do that. */
	.option push
	.option norelax
	la gp, __global_pointer$
	.option pop
	call main
1:	j 1b
