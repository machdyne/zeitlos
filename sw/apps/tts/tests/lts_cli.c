/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * One word per line in, "WORD<TAB>phonemes" out -- the real lts.c,
 * for tests/lts_eval.py.
 */
#include <stdio.h>
#include <string.h>
#include "../lts.h"
int main(void) {
	char line[256], out[512];
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!lts_word(line, out, sizeof(out))) out[0] = 0;
		printf("%s\t%s\n", line, out);
	}
	return 0;
}
