/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * This is the Zeitlos kernel shell / interactive bootloader.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../common/zeitlos.h"
#include "kernel.h"
#include "fs/fs.h"

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

   printf("mounting fs ... ");
   fflush(stdout);

   if (fs_mount() == 0)
      printf("done.\n");
   else
      printf("failed.\n");

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
		else if (!strncmp(buffer, "hd", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr;
			if (sscanf(arg, "%lx", &addr))
				hex_dump(addr);
		}


		// LIST DIRECTORY
		else if (!strncmp(buffer, "ls", cmdlen)) {
			arg = get_arg(buffer, 1);
			if (arg != NULL)
				fs_list_dir(arg);
			else
				fs_list_dir("/");
		}

		// RECEIVE TO ADDR VIA XFER
		else if (!strncmp(buffer, "xa", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t addr, bytes;
			if (!sscanf(arg, "%lx", &addr)) {
				printf("bad address\n");
				continue;
			}
			printf("xfer addr 0x%lx; ready to receive (press D to cancel) ...\n",
				addr);
			bytes = xfer_recv(addr);
			printf("received %li bytes to 0x%lx.\n", bytes, addr);
		}


		// RECEIVE TO FILE VIA XFER
		else if (!strncmp(buffer, "xf", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t bytes_received, bytes_written;
			void *tmp = k_mem_alloc(1024*128); // 128K max file size for now
			uint32_t addr = (uint32_t)(uintptr_t)tmp;
			printf("uploading to file %s.\n", arg);
			printf("xfer addr 0x%lx; ready to receive (press D to cancel) ...\n",
				addr);
			bytes_received = xfer_recv(addr);
			printf("received %li bytes to 0x%lx.\n", bytes_received, addr);
			if (bytes_received) {
				printf("writing to file %s ... ", arg);
				fflush(stdout);
				bytes_written = fs_write_file(arg, tmp, bytes_received);
				k_mem_free(tmp);
				if (bytes_written == bytes_received)
					printf("done.\n");
				else
					printf("failed.\n");
			}
		}

		// CREATE A PROCESS
		else if (!strncmp(buffer, "run", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t size = fs_size(arg);
			printf("creating process (file: %s size: %ld)\n", arg, size);
			fflush(stdout);
			uint32_t pid = k_proc_create(size);
			printf(" - pid: %ld\n", pid);
			if (!pid) {
				printf("unable to create process\n");
				continue;
			}

			uint32_t base = k_proc_base(pid);
			printf(" - base: %lx\n", base);
			printf(" - loading file\n");
			fs_load(base, arg);
			printf(" - starting process\n");
			k_proc_start(pid);

		}

		// KILL A PROCESS
		else if (!strncmp(buffer, "kill", cmdlen)) {
			arg = get_arg(buffer, 1);
			uint32_t pid;
			if (!sscanf(arg, "%ld", &pid)) {
				printf("bad pid\n");
				continue;
			}
			printf("killing process %ld: ", pid);
			fflush(stdout);
			if (k_proc_kill(pid) == Z_OK)
				printf("OK\n");
			else
				printf("FAIL\n");
		}

		// DISPLAY PROCESS SNAPSHOT
		else if (!strncmp(buffer, "ps", cmdlen)) {
			k_proc_dump();
		}

		// DISPLAY KERNEL SNAPSHOT
		else if (!strncmp(buffer, "ks", cmdlen)) {
			k_kernel_dump();
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
	printf(" hd <addr>         hex dump memory\n");
	printf(" xa <addr>         receive to addr via xfer\n");
	printf(" xf <file>         receive to file via xfer\n");
	printf(" run <file>        create a new process\n");
	printf(" kill <pid>        kill a process\n");
	printf(" ps                display a process snapshot\n");
	printf(" ks                display a kernel snapshot\n");
	printf(" ls [path]         display list of files\n");

}
