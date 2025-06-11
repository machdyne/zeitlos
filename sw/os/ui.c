/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * User interface.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "../common/zeitlos.h"
#include "kernel.h"
#include "uart.h"

uint16_t z_cursor_y = 0;
uint16_t z_cursor_x = 0;

void z_ui_int(void) {

}

// --

z_obj_t *z_ui_print (z_obj_t *obj) {

	char *s = obj->val.str;

    while (*s) {
        if (*s == '\n') {
            while (k_uart_tx_full());
            reg_uart0_data = '\r';
        }
        while (k_uart_tx_full());
        reg_uart0_data = *s++;
    }

	return (&z_rv_ok);

}
