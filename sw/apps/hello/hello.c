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

	for (int i = 0; i < 10; i++) {

		reg_led = 0x00;
		delay();

		printf("HELLO %i.\n", i);

		reg_led = 0x01;
		delay();

	}

//	while (1) /* wait */;

}
