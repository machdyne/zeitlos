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
#include <stdarg.h>
#include <stdbool.h>

#include "kernel.h"
#include "../common/zeitlos.h"
#include "uart.h"
#include "ui.h"

#define Z_PROCS_MAX 16
#define Z_PROC_STACK_SIZE	8*1024

typedef z_obj_t* (*z_syscall_t)(z_obj_t *args);

z_syscall_t z_syscall_table[Z_SYSCALL_COUNT] = {
#define Z_MKSYSCALL(name, fn) [Z_SYS_##name] = fn,
#include "../common/syscalls.def"
#undef Z_SYSCALL
};

extern char _start, _end;

// force bss because __global_pointer$ will be wrong in the interrupt handler
volatile uint32_t __attribute__((section(".bss"))) z_pid = 0;
volatile z_proc __attribute__((section(".bss"))) z_procs[Z_PROCS_MAX];
volatile uint32_t __attribute__((section(".bss"))) z_kernel_ticks = 0;

// --

void sh(void);
uint32_t *z_kernel_entry(uint32_t cmd, uint32_t *args, uint32_t val);
uint32_t z_proc_active_count(void);
void *z_proc_exit_stub(void);

void kprint(const char *s);
void kprint_hex32(uint32_t);

// --

int main(void) {

	kprint("\nZEITLOS\n");

	kprint("KENTRY: ");
	kprint_hex32((uint32_t)(uintptr_t)z_kernel_entry);
	kprint("\n");

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

	// init uart
	z_uart_init();
	printf("UART initialized.\n");

//	while (1) {
//		if ((z_kernel_ticks % 100) == 0) z_kernel_dump();
//	};

	// the kernel shell is process zero
	sh();

}

// - a pointer to the kernel entry function can be found at 0x0000000c
//   (if the kernel started)
// - this is called by the BIOS interrupt handler which uses the interrupt stack
// - it can also be called by apps to make system calls

uint32_t *z_kernel_entry(uint32_t syscall_id, uint32_t *regs, uint32_t irqs) {

	if (syscall_id != Z_SYSCALL_NONE) {

		if (syscall_id >= Z_SYSCALL_COUNT || !z_syscall_table[syscall_id]) {
			return ((uint32_t *)&z_rv_fail);
    	}

		return (uint32_t *)z_syscall_table[syscall_id]((z_obj_t *)regs);

	}

	// not a system call; must be an interrupt

	++z_kernel_ticks;

	// handle interrupts
	if ((irqs & (1 << Z_IRQ_UART)) != 0) {
		z_uart_irq();
	}

	//if ((irqs & (1 << Z_IRQ_HID)) != 0) {
	// z_ui_irq();
	//}

	// swap process on KTIMER interrupt
	if ((irqs & (1 << Z_IRQ_KTIMER)) != 0) {

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

		return z_procs[z_pid].regs;
	}

	return regs;

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
	kprint(" kticks: ");
	kprint_hex32(z_kernel_ticks);
	kprint("\n");
	return(&z_rv_ok);
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

z_obj_t *z_sys_hello(z_obj_t *obj);

z_obj_t *z_sys_hello(z_obj_t *obj) {
	printf("TEST.\n");
}

// --

void kprint(const char *s) {
    while (*s) {
        if (*s == '\n') {
            while ((reg_uart0_lsr & 0x20) == 0);
            reg_uart0_data = '\r';
        }
        while ((reg_uart0_lsr & 0x20) == 0);
        reg_uart0_data = *s++;
    }
}

void kprint_hex_digit(uint8_t val) {
    if (val < 10) {
        reg_uart0_data = '0' + val;
    } else {
        reg_uart0_data = 'A' + (val - 10);
    }
    for (volatile uint32_t i = 0; i < 500; i++);
}

void kprint_hex32(uint32_t val) {
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        kprint_hex_digit(nibble);
    }
}

