/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * This is the Zeitlos microkernel.
 *
 * The kernel is loaded at the beginning of main memory (0x4000_0000).
 *
 */

#include <stdio.h>
#include <stdbool.h>

#include "kernel.h"
#include "../common/zeitlos.h"
#include "api.h"

#define Z_PROCS_MAX 16
#define Z_PROC_STACK_SIZE	8*1024

extern char _start, _end;

// force bss because __global_pointer$ will be wrong in the interrupt handler
uint32_t __attribute__((section(".bss"))) z_pid = 0;
z_proc __attribute__((section(".bss"))) z_procs[Z_PROCS_MAX];

// --

void sh(void);
uint32_t *z_kernel_entry(uint32_t cmd, uint32_t *args, uint32_t val);
uint32_t z_proc_active_count(void);
void *z_proc_exit_stub(void);

uint32_t z_kernel_ticks;

// --

int main(void) {

	printf("ZEITLOS\n");
	printf("END: %8lx\n", (uint32_t)&_end);

	// set all processes as available
	for (int p = 0; p < Z_PROCS_MAX; p++) {
		z_procs[p].addr = 0x00000000;
		z_procs[p].flags = 0x00000000;
	}

	// create process zero (this process):
	z_proc_create(0x40000000,
		(uint32_t)&_end - (uint32_t)&_start + Z_PROC_STACK_SIZE);

	// set the kernel register so the irq handler knows who to call
	reg_kernel = (uint32_t)(uintptr_t)z_kernel_entry;

	printf("ENTRY: %8lx\n", reg_kernel);

	// the kernel shell is process zero
	sh();

}

// this is called by the BIOS interrupt handler
// it uses the interrupt stack
uint32_t *z_kernel_entry(uint32_t cmd, uint32_t *regs, uint32_t irqs) {

	++z_kernel_ticks;

	if (cmd != Z_CMD_IRQ) {

		switch (cmd) {
			case Z_CMD_GET_API_MAP:
				return (uint32_t *)z_api_map();
				break;
			default:
				return regs;
		}

	}

	// swap process on KTIMER interrupt
//	if ((irqs & (1 << Z_IRQ_KTIMER)) != 0) {

		// don't switch if there's only one process
		if (z_proc_active_count() < 2) return regs;

		// save current process registers
  		for (int i = 0; i < 32; i++) {
			z_procs[z_pid].regs[i] = *(regs + i);
		}

		// find next active process (round-robin scheduling)
		next_process:
		z_pid++;

		if (z_pid >= Z_PROCS_MAX) z_pid = 0;

		if ((z_procs[z_pid].flags & Z_PROC_FLAG_ACTIVE) != Z_PROC_FLAG_ACTIVE)
			goto next_process;
//	}


	//if ((irqs & (1 << Z_IRQ_UART)) != 0) {
	//	z_uart_int();
	//}

	//if ((irqs & (1 << Z_IRQ_HID)) != 0) {
	// z_ui_int();
	//}
	
	return z_procs[z_pid].regs;

}

uint32_t z_proc_active_count(void) {

	uint32_t count = 0;

	for (int i = 0; i < Z_PROCS_MAX; i++)
		if ((z_procs[i].flags & Z_PROC_FLAG_ACTIVE) == Z_PROC_FLAG_ACTIVE)
			count++;

	return(count);

}

z_rv z_proc_create(uint32_t addr, uint32_t size) {

	// find first available process
	for (int p = 0; p < Z_PROCS_MAX; p++) {

		if (z_procs[p].addr == 0x00000000) {
			z_procs[p].addr = addr;
			z_procs[p].size = size;
			for (int i = 0; i < 32; i++) {
				z_procs[p].regs[i] = 0x00000000;
			}
			z_procs[p].regs[0] = addr;	// pc
			z_procs[p].regs[1] = (uint32_t)&z_proc_exit_stub; // ra

			// sp:
			//  sp for kernel is set in boot code
			//  sp for apps is set in crt0
			//  so this init value shouldn't be used is probably unnecessary
			z_procs[p].regs[2] = 0x40100000;
		
			// set the active flag last because it may cause an immediate switch
			z_procs[p].flags = Z_PROC_FLAG_ACTIVE;
			return(Z_OK);
		}

	}

	return(Z_FAIL);
}

z_rv z_kernel_dump(void) {
	printf(" kticks: %lu\n", z_kernel_ticks);
	return(Z_OK);
}

z_rv z_proc_dump(void) {
	for (int i = 0; i < Z_PROCS_MAX; i++) {
		printf(" pid: %2i addr: %.8lx size: %.8lx pc %.8lx sp: %.8lx flags: %.8lx\n",
			i, z_procs[i].addr, z_procs[i].size,
			z_procs[i].regs[0], z_procs[i].regs[2], z_procs[i].flags);
	}
	return(Z_OK);
}

void *z_proc_exit_stub(void) {
	printf("PEXIT.\n");
	while (1);
}
