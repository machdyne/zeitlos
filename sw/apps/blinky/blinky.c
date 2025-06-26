#include <stdio.h>
#include "../../common/zeitlos.h"

void delay() {
   volatile static int x, y;
   for (int i = 0; i < 50000; i++) {
      x += y;
   }
}

int main() {

	while (1) {

		reg_led = 0x01;
		delay();
		reg_led = 0x00;
		delay();

	}

}
