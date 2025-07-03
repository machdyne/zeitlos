/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * UART interface.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "../common/zeitlos.h"
#include "kernel.h"
#include "uart.h"

#define UART_FIFO_SIZE 512

volatile uint8_t __attribute__((section(".bss"))) uart_rx_fifo[UART_FIFO_SIZE];
volatile uint8_t __attribute__((section(".bss"))) uart_tx_fifo[UART_FIFO_SIZE];
volatile uint8_t __attribute__((section(".bss"))) rx_head = 0, rx_tail = 0;
volatile uint8_t __attribute__((section(".bss"))) tx_head = 0, tx_tail = 0;

uint16_t leds = 0x00;

void z_uart_init(void) {

   reg_uart0_fcr = (uint8_t)0b00000111; // flush FIFOs
	reg_uart0_ier = (uint8_t)0b00000001; // enable RX interrupt

	reg_leds = 0;

}

void uart_irq_enable(void) {
    uint32_t mask = maskirq(0);               // read current IRQ mask
    mask &= ~(1 << 4);                        // clear bit 4 to unmask UART
    maskirq(mask);                            // write new mask
}

void uart_irq_disable(void) {
    uint32_t mask = maskirq(0);               // read current IRQ mask
    mask |= (1 << 4);                         // set bit 4 to mask UART
    maskirq(mask);                            // write new mask
}

uint32_t ints = 0;

void z_uart_irq(void) {

	uint8_t iir = reg_uart0_iir;

	if (!(iir & 0x01)) {

		uint8_t lsr = reg_uart0_lsr;

		// error
		if (lsr & 0x80) {
			char c = reg_uart0_data;
			return;
		}

		uint8_t int_id = (iir >> 1) & 0x07;

		//reg_leds = int_id;

		switch (int_id) {

			case 0x01: // Transmit Holding Register Empty (THRE)
				if (tx_head == tx_tail) {
					// nothing to send; disable THRE interrupt
					reg_uart0_ier = 0x01;
				} else {
					// drain TX FIFO
					while ((reg_uart0_lsr & 0x20) && (tx_head != tx_tail)) {
						reg_uart0_data = uart_tx_fifo[tx_tail];
						tx_tail = (tx_tail + 1) % UART_FIFO_SIZE;
					}

					// check again in case we just finished
					if (tx_head == tx_tail) {
						reg_uart0_ier = 0x01;
					}
				}
				break;

			case 0x02: // Received Data Available (RDA)
			case 0x06: // Character Timeout Indication (treated same as RDA)

				while (reg_uart0_lsr & 0x01) {  // data Ready

					uint8_t c = reg_uart0_data; // reading data clears error state
					uint8_t next = (rx_head + 1) % UART_FIFO_SIZE;
					if (next != rx_tail) {  // RX FIFO not full
						uart_rx_fifo[rx_head] = c;
						rx_head = next;
					} else {
						// TODO: handle RX overflow; currently drops character
					}
	
				}

				break;

			default:
				break;

		}
	}
}

static inline bool uart_tx_fifo_empty() {
    return tx_head == tx_tail;
}

static inline bool uart_rx_fifo_empty() {
    return rx_head == rx_tail;
}

static inline bool uart_tx_fifo_full() {
    return ((tx_head + 1) % UART_FIFO_SIZE) == tx_tail;
}

static inline bool uart_rx_fifo_full() {
    return ((rx_head + 1) % UART_FIFO_SIZE) == rx_tail;
}

// --

bool k_uart_rx_empty(void) {
	return uart_rx_fifo_empty();
}

bool k_uart_tx_full(void) {
	return uart_tx_fifo_full();
}


int16_t k_uart_getc(void) {

	if (rx_head == rx_tail) {
		// RX FIFO is empty
		return -1;
	}

	char c = uart_rx_fifo[rx_tail];
	rx_tail = (rx_tail + 1) % UART_FIFO_SIZE;

	return c;

}

void k_uart_putc(char c) {

	uart_irq_disable();

	uint8_t next = (tx_head + 1) % UART_FIFO_SIZE;

	if (next == tx_tail) {
		uart_irq_enable();
		// TX buffer full
		return;  // Or return error code
	}

	bool fifo_was_empty = (tx_head == tx_tail);

	uart_tx_fifo[tx_head] = c;
	tx_head = next;

	if (fifo_was_empty && (reg_uart0_lsr & 0x20)) {
		// FIFO was empty and UART is ready — send directly
		reg_uart0_data = uart_tx_fifo[tx_tail];
		tx_tail = (tx_tail + 1) % UART_FIFO_SIZE;
	}

	if (tx_head != tx_tail) {
		reg_uart0_ier = 0b00000011; // Enable TX and RX
	}

	uart_irq_enable();

}

// --

z_obj_t *z_uart_rx_empty(z_obj_t *obj) {
	obj->val.int32 = uart_rx_fifo_empty();
	return (&z_ok);
}

z_obj_t *z_uart_tx_full(z_obj_t *obj) {
	obj->val.int32 = uart_tx_fifo_full();
	return (&z_ok);
}

z_obj_t *z_uart_getc(z_obj_t *obj) {
	obj->val.int32 = k_uart_getc();
	return (&z_ok);
}

z_obj_t *z_uart_putc(z_obj_t *obj) {
	char c = (char)obj->val.int32;
	k_uart_putc(c);
	return (&z_ok);
}
