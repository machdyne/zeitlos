/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * This is the Zeitlos kernel shell / interactive bootloader.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../common/zeitlos.h"
#include "kernel.h"

// --

char *get_arg(char *str, int n);
void sh_help(void);
void hex_dump(uint32_t addr);
uint32_t xfer_recv(uint32_t addr_ptr);

// --

void sh(void) {

   char buffer[256];
   int cmdlen;
   char *cmdend;
	char *arg;

	printf("type help for help.\n\n");

	while (1) {

		printf("> ");
		fflush(stdout);

		readline(buffer, 255);
		cmdend = strchr(buffer, ' ');

		if (cmdend == NULL)
			cmdlen = 255;
		else
			cmdlen = cmdend - buffer;

		printf("\n");

		// HELP
		if (!strncmp(buffer, "help", cmdlen)) sh_help();

		// HEX DUMP
		if (!strncmp(buffer, "hd", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr;
			if (sscanf(arg, "%lx", &addr))
				hex_dump(addr);
		}

		// RECEIVE TO ADDR VIA XFER
		if (!strncmp(buffer, "xa", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr, bytes;
			if (!sscanf(arg, "%lx", &addr))
				addr = 0x40040000; 
			printf("xfer addr 0x%lx; ready to receive (press D to cancel) ...\n",
				addr);
			bytes = xfer_recv(addr);
			printf("received %li bytes to 0x%lx.\n", bytes, addr);
		}

		// CREATE A PROCESS
		if (!strncmp(buffer, "pc", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr;
			if (!sscanf(arg, "%lx", &addr)) {
				printf("bad address\n");
				continue;
			}
			printf("creating process at %lx: ", addr);
			fflush(stdout);
			if (z_proc_create(addr, 128*1024) == 0)
				printf("OK\n");
			else
				printf("FAIL\n");
		}

		// UPLOAD A PROCESS
		if (!strncmp(buffer, "pu", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr, bytes;
			if (!sscanf(arg, "%lx", &addr))
				addr = 0x40040000; 
			printf("xfer addr 0x%lx; ready to receive (press D to cancel) ...\n",
				addr);
			bytes = xfer_recv(addr);
			printf("received %li bytes to 0x%lx.\n", bytes, addr);
			printf("creating process at %lx: ", addr);
			fflush(stdout);
			if (z_proc_create(addr, bytes + (32*1024)) == 0)
				printf("OK\n");
			else
				printf("FAIL\n");
		}

		if (!strncmp(buffer, "ps", cmdlen)) {
			z_proc_dump();
		}

		if (!strncmp(buffer, "ks", cmdlen)) {
			z_kernel_dump();
		}

      if (!strncmp(buffer, "boot", cmdlen)) {
         asm volatile ("li a0, 0x40040000");
         asm volatile ("jr a0");
         __builtin_unreachable();
      }

	}

}

char *get_arg(char *str, int n) {

	char *token;
	int tn = 0;

	token = strtok(str, " ");

	while (token != NULL && tn != n) {
		token = strtok(NULL, " ");
		tn++;
	}

	return token;

}

void hex_dump(uint32_t addr) {

	uint8_t tmp;

	for (int i = 0; i < 16; i++) {
		printf("%.8lx ", addr);
		printf(" ");
		for (int x = 0; x < 16; x++) {
			tmp = (*(volatile uint8_t *)addr);
			printf("%.2x ", tmp);
			addr += 1;
		}
		printf("\n");
	}

}

void sh_help(void) {

	printf("commands:\n");
	printf(" hd [addr]         hex dump memory\n");
	printf(" xa [addr]         receive to addr via xfer\n");
	printf(" pc [addr]         create & start a new process\n");
	printf(" pu [addr]         upload & create & start a new process\n");
	printf(" ps                display a process snapshot\n");
	printf(" ks                display a kernel snapshot\n");

}
