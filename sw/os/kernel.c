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
#include "zeitlos.h"
#include "mem.h"
#include "uart.h"
#include "ui.h"

#define Z_PROCS_MAX 16
#define Z_PROC_STACK_SIZE  8*1024

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
uint32_t k_proc_active_count(void);
void *k_proc_exit_stub(void);

void kprint(const char *s);
void kprint_hex32(uint32_t);

// --

int main(void) {

	kprint("\nZEITLOS\n");

	// init uart
	z_uart_init();
	printf(" - uart initialized.\n");

	// init memory management
	k_mem_init();
	printf(" - memory initialized.\n");

	// set all processes as available
	for (int p = 0; p < Z_PROCS_MAX; p++) {
		z_procs[p].base = 0x00000000;
		z_procs[p].flags = 0x00000000;
	}

	// create process zero (this process):
	uint32_t k_size = ((uint32_t)&_end - (uint32_t)&_start) + Z_PROC_STACK_SIZE;
	printf(" - kernel process size %ld\n", k_size);
	k_proc_create(k_size);
	k_proc_start(0);

	// set the kernel register so the irq handler knows who to call
	reg_kernel = (uint32_t)(uintptr_t)z_kernel_entry;
	printf(" - kernel active.\n");

//	while (1) {
//		if ((z_kernel_ticks % 100) == 0) z_kernel_dump();
//	};

	printf(" - starting shell.\n");

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
		if (k_proc_active_count() < 2) return regs;

		// save current process registers
  		for (int i = 0; i < 32; i++) {
			z_procs[z_pid].regs[i] = *(regs + i);
		}

		// find next active process (round-robin scheduling)
		next_process:
		z_pid++;
		if (z_pid >= Z_PROCS_MAX) z_pid = 0;

		if ((z_procs[z_pid].flags & Z_PROC_FLAG_DIE) == Z_PROC_FLAG_DIE) {
			// free the memory
			k_mem_free(z_procs[z_pid].base);
			// kill the process
			z_procs[z_pid].base = 0x00000000;
			z_procs[z_pid].flags = 0x00000000;
			z_pid++;
			if (z_pid >= Z_PROCS_MAX) z_pid = 0;
		}

		if ((z_procs[z_pid].flags & Z_PROC_FLAG_ACTIVE) != Z_PROC_FLAG_ACTIVE)
			goto next_process;

		return z_procs[z_pid].regs;
	}

	return regs;

}

uint32_t k_proc_active_count(void) {

	uint32_t count = 0;

	for (int i = 0; i < Z_PROCS_MAX; i++)
		if ((z_procs[i].flags & Z_PROC_FLAG_ACTIVE) == Z_PROC_FLAG_ACTIVE)
			count++;

	return(count);

}

// return process id or 0 on fail
uint32_t k_proc_create(uint32_t size) {

	uint32_t mem_size = k_mem_align_up(size + Z_PROC_STACK_SIZE,
		Z_MEM_ALIGNMENT);

	// find first available process slot
	for (int p = 0; p < Z_PROCS_MAX; p++) {

		if (z_procs[p].base == 0x00000000) {

			void *mem = k_mem_alloc(mem_size);
			if (!mem) return(Z_FAIL);
			uint32_t base = (int32_t)(uintptr_t)mem;
			z_procs[p].base = base;
			z_procs[p].size = mem_size;
			for (int i = 0; i < 32; i++) {
				z_procs[p].regs[i] = 0x00000000;
			}
			z_procs[p].regs[0] = base;	// pc
			z_procs[p].regs[1] = (uint32_t)&k_proc_exit_stub; // ra

			// sp:
			//  sp for kernel is set in boot code
			//  sp for apps is set in crt0
			//  so this init value shouldn't be used is probably unnecessary
			z_procs[p].regs[2] = 0x40100000;
		
			return(p);

		}

	}

	return(0);
}

z_rv k_proc_start(uint32_t pid) {
	z_procs[pid].flags |= Z_PROC_FLAG_ACTIVE;
}

z_rv k_proc_stop(uint32_t pid) {
	z_procs[pid].flags &= ~Z_PROC_FLAG_ACTIVE;
}

z_rv k_proc_kill(uint32_t pid) {
	z_procs[pid].flags |= Z_PROC_FLAG_DIE;
	return Z_OK;
}

uint32_t k_proc_base(uint32_t pid) {
	return z_procs[pid].base;
}

z_rv k_kernel_dump(void) {
	kprint(" kticks: ");
	kprint_hex32(z_kernel_ticks);
	kprint("\n");
	return Z_OK;
}

z_rv k_proc_dump(void) {
	for (int i = 0; i < Z_PROCS_MAX; i++) {
		if (!z_procs[i].base) continue;
		printf(" pid: %2i base: %.8lx size: %.8lx pc %.8lx sp: %.8lx flags: %.8lx\n",
			i, z_procs[i].base, z_procs[i].size,
			z_procs[i].regs[0], z_procs[i].regs[2], z_procs[i].flags);
	}
	return Z_OK;
}

void *k_proc_exit_stub(void) {
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

