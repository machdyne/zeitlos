#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"

void delay() {
   volatile static int x, y;
   for (int i = 0; i < 10000; i++) {
      x += y;
   }
}

int main() {

	uint32_t ctr = 0;

	uint32_t *(*z_kernel_ptr)(uint32_t, uint32_t *, uint32_t);

	z_kernel_ptr =
		(uint32_t *(*)(uint32_t, uint32_t *, uint32_t))(uintptr_t)reg_kernel;


	while (1) {

		reg_led = 0x00;
		delay();

//		printf("HELLO.\n"); // not working yet

		z_obj_t obj;
		obj.type = Z_STR;
		obj.val.str = "THIS IS A TEST.\n";
		z_kernel_ptr(Z_SYS_UI_PRINT, (uint32_t *)&obj, 0);

		reg_led = 0x01;
		delay();

	}

}
