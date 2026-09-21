/* Zeitlos -- one line of text in, its phonemes out (real text2ph.c). */
#include <stdio.h>
#include <string.h>
#include "../text2ph.h"
int main(void) {
	char line[1024], out[2048];
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		uint32_t pos = 0, len = (uint32_t)strlen(line);
		bool more = true;
		printf("%s\n", line);
		while (more && text2ph_chunk(line, len, &pos, false, out, sizeof(out), &more))
			printf("   -> %s\n", out);
	}
	return 0;
}
