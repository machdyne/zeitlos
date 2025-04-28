#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"

void delay() {
   volatile static int x, y;
   for (int i = 0; i < 500000; i++) {
      x += y;
   }
}

int main() {

	uint32_t ctr = 0;

	while (1) {
		reg_led = 0x00;
		reg_uart0_data = 'A'; // this works
//		putchar('Q');	// this doesn't work
//		printf("Hello world! %li\n", ctr);	// this doesn't work
		delay();
		reg_led = 0x01;
		delay();
	}

}
