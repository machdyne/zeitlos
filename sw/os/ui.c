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

uint16_t z_cursor_y = 0;
uint16_t z_cursor_x = 0;

void z_ui_int(void) {

}

void delay() {
   volatile static int x, y;
   for (int i = 0; i < 10000; i++) {
      x += y;
   }
}

void z_hello_impl (char *str) {

   reg_uart0_data = ':'; // this works
   delay();

	printf("HELLO\r\n"); // this doesn't
	//printf("HELLO %s!\r\n", str);

}
