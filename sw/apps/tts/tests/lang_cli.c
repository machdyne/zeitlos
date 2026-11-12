/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Text to phonemes in another language, from the shell, for writing
 * and checking a language's rules (docs/tts.md, "Other languages"):
 *
 *     make -C sw/apps/tts lang_cli
 *     echo "Datei löschen?" | /tmp/lang_cli de      (from the tree's root)
 */
#include <stdio.h>
#include <string.h>
#include "../lang.h"

int main(int argc, char **argv) {
	char line[1024], out[2048];
	if (argc < 2 || !lang_load(argv[1])) {
		fprintf(stderr, "lang_cli: no rules for %s (sw/data/tts/lang/%s.ttl)\n",
			argc > 1 ? argv[1] : "?", argc > 1 ? argv[1] : "?");
		return 1;
	}
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		uint32_t pos = 0, len = (uint32_t)strlen(line);
		bool more = true;
		printf("%s\n", line);
		while (more && lang_chunk(line, len, &pos, false, out, sizeof(out), &more))
			printf("   -> %s\n", out);
	}
	return 0;
}
