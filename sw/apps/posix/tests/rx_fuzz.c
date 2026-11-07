/*
 * rx.c against Python's re (tests/rx_fuzz.py): reads "PATTERN\tSUBJECT"
 * lines, prints the first match's groups as Python would, or "none".
 * Extended syntax, which for this subset means what Python's means.
 */
#include <stdio.h>
#include <string.h>
#include "../rx.h"

int main(void) {
	static char line[4096];
	static rx_t r;
	while (fgets(line, sizeof(line), stdin)) {
		char *tab = strchr(line, '\t'), err[96];
		if (!tab) continue;
		*tab = 0;
		char *s = tab + 1;
		s[strcspn(s, "\n")] = 0;
		if (!rx_compile(&r, line, (int)strlen(line), true, err, sizeof(err))) { printf("error: %s\n", err); continue; }
		int cap[2 * RX_GROUPS];
		bool tl;
		if (!rx_exec(&r, s, (int)strlen(s), 0, cap, &tl)) { printf(tl ? "toolong\n" : "none\n"); continue; }
		for (int g = 0; g <= r.ngroups; g++) printf("%d,%d ", cap[2 * g], cap[2 * g + 1]);
		printf("\n");
	}
	return 0;
}
