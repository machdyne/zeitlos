// This is free and unencumbered software released into the public domain.
//
// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.

#include <stdint.h>

#define reg_kernel (*(volatile uint32_t*)0x0000000c)

uint32_t *(*kernel_ptr)(uint32_t, uint32_t *, uint32_t);

uint32_t *irq(uint32_t *regs, uint32_t irqs)
{

	kernel_ptr =
		(uint32_t *(*)(uint32_t, uint32_t *, uint32_t))(uintptr_t)reg_kernel;

	if (reg_kernel != 0x00000000) {
		regs = (uint32_t *)kernel_ptr(0, regs, irqs);	// Z_SYSCALL_NONE
	}

	return regs;

}

