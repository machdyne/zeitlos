/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "../common/zeitlos.h"
#include "uart.h"

void getchars(char *buf, int len);
uint32_t crc32b(char *data, uint32_t len);
void myputch(int c);
int mygetch(void);

void getchars(char *buf, int len) {
	int c;
	for (int i = 0; i < len; i++) {
		while ((c = mygetch()) == EOF) /* wait */;
		buf[i] = (uint8_t)c;
	};
}

void myputch(int c)
{
	while (k_uart_tx_full()) /* wait */;
	k_uart_putc((char)c);
}

int mygetch(void)
{
	if (k_uart_rx_empty())
      return EOF;
   else
		return k_uart_getc();
}

uint32_t xfer_recv(uint32_t addr_ptr)
{

	uint32_t addr = addr_ptr;
	uint32_t bytes = 0;
	uint32_t crc_ours;
	uint32_t crc_theirs;

	char buf_data[252];
	char buf_crc[4];

	int cmd;
	int datasize;

	while (1) {

		while ((cmd = mygetch()) == EOF) /* wait */;
		buf_data[0] = (uint8_t)cmd;

		if ((char)cmd == 'L') {
			while ((datasize = mygetch()) == EOF) /* wait */;
			buf_data[1] = (uint8_t)datasize;
			getchars(&buf_data[2], datasize);
			getchars(buf_crc, 4);

			crc_ours = crc32b(buf_data, 2 + datasize);
			crc_theirs = buf_crc[0] | (buf_crc[1] << 8) |
				(buf_crc[2] << 16) | (buf_crc[3] << 24);

			if (crc_ours == crc_theirs) {
				memcpy((void *)addr, &buf_data[2], datasize);
				addr += datasize;
				bytes += datasize;
				myputch('A');
			} else {
				myputch('N');
			}
		}

		if ((char)cmd == 'D') {
			break;
		}

	}

	return bytes;

}

uint32_t crc32b(char *data, uint32_t len) {

	uint32_t byte, crc, mask;

	crc = 0xffffffff;
	for (int i = 0; i < len; i++) {
		byte = data[i];
		crc = crc ^ byte;
		for (int j = 7; j >= 0; j--) {
			mask = -(crc & 1);
			crc = (crc >> 1) ^ (0xedb88320 & mask);
		}
	}

	return ~crc;

}
