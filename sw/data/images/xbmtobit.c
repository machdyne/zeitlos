#include <stdio.h>
#include "zeitlos.xbm"

int main(int argc, char *argv) {

	FILE *f = fopen("zeitlos.bin", "wb");
	fwrite(zeitlos_bits, sizeof(zeitlos_bits), 1, f);
	fclose(f);

}
