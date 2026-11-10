#ifndef Z_CRASH_H
#define Z_CRASH_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The last crash report in a piece of console log (docs/report.md).
 *
 * The kernel reports a crashed app on the console (k_fault(),
 * sw/os/kernel.c, docs/mpu.md "Crash reporting"):
 *
 *   *** view (pid 7) crashed: store outside its own memory
 *       address 4001f2c0 (kernel memory)  at pc 800012a4
 *       ra 80000f10  sp 8003ff70  gp 80004800  a0 4001f2c0  a1 00000000
 *       ended; the rest of the system is unaffected
 *
 * z_crash_find() finds the last such report: from the last line that
 * starts "*** " and has " crashed: " in it, through the indented lines
 * after it. Header-only, with no dependencies, so the kernel's `report`
 * command and the checkup app use the same code, and a host test can
 * run it.
 */
#include <stdint.h>
#include <stdbool.h>

static inline bool z_crash_line(const char *s, uint32_t n) {
	if (n < 4 || s[0] != '*' || s[1] != '*' || s[2] != '*' || s[3] != ' ') return false;
	for (uint32_t i = 4; i + 10 <= n; i++)
		if (s[i] == ' ' && s[i + 1] == 'c' && s[i + 2] == 'r' && s[i + 3] == 'a' &&
			s[i + 4] == 's' && s[i + 5] == 'h' && s[i + 6] == 'e' && s[i + 7] == 'd' &&
			s[i + 8] == ':' && s[i + 9] == ' ') return true;
	return false;
}

// True, with the report's offset and length in log[0..n), if there is
// one. A report cut off by the end of the buffer is returned as far as
// it goes.
static inline bool z_crash_find(const char *log, uint32_t n, uint32_t *start, uint32_t *len) {

	uint32_t s = 0, p = 0, e;
	bool found = false;

	while (p < n) {
		e = p;
		while (e < n && log[e] != '\n') e++;
		if (z_crash_line(log + p, e - p)) { s = p; found = true; }
		p = e + 1;
	}
	if (!found) return false;

	e = s;
	while (e < n && log[e] != '\n') e++;
	if (e < n) e++;
	while (e < n && (log[e] == ' ' || log[e] == '\t')) {
		while (e < n && log[e] != '\n') e++;
		if (e < n) e++;
	}

	*start = s;
	*len = e - s;
	return true;

}

#endif
