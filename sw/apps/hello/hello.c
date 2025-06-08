#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"

void delay() {
   volatile static int x, y;
   for (int i = 0; i < 10000; i++) {
      x += y;
   }
}

void print_hex_digit(uint8_t val) {
    if (val < 10) {
        reg_uart0_data = '0' + val;
    } else {
        reg_uart0_data = 'A' + (val - 10);
    }
    delay();
}

void print_hex32(uint32_t val) {
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        print_hex_digit(nibble);
    }
}

void print_ptr_hex(const void *ptr) {
    print_hex32((uint32_t)(uintptr_t)ptr);
}

int main() {

	uint32_t ctr = 0;

	z_api_map_t *z_api_map;

	reg_uart0_data = 'A';
	z_api_map = z_init();

	delay();
	reg_uart0_data = 'B';
	delay();

	reg_uart0_data = ':';
	delay();

	print_ptr_hex((void *)&z_api_map);
	delay();

	reg_uart0_data = ':';
	delay();

	print_ptr_hex(z_api_map);
	delay();

	while (1) {

		reg_led = 0x00;

//		putchar('Q');	// this doesn't work
//		printf("Hello world! %li\n", ctr);	// this doesn't work

		if (z_api_map && z_api_map->z_hello) {
			z_api_map->z_hello("TEST");
			delay();
		} else {
			reg_uart0_data = 'F';
			delay();
		}

		delay();
		reg_led = 0x01;
		delay();

	}

}
