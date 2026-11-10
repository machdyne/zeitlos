/*
 * z_crash_find() (sw/common/zcrash.h, docs/report.md): the last crash
 * report in a piece of console log.
 *
 *   make -C sw/common/tests -f Makefile.zcrash
 */
#include <stdio.h>
#include <string.h>
#include "../zcrash.h"

static int checks, failures;
static void expect(int ok, const char *what) {
	checks++;
	if (!ok) { failures++; printf("  FAIL: %s\n", what); }
}

static int found(const char *log, const char *want) {
	uint32_t s, l;
	if (!z_crash_find(log, (uint32_t)strlen(log), &s, &l)) return want == NULL;
	return want && l == strlen(want) && !memcmp(log + s, want, l);
}

#define CRASH1 "*** view (pid 7) crashed: store outside its own memory\n" \
	"    address 4001f2c0 (kernel memory)  at pc 800012a4\n" \
	"    ra 80000f10  sp 8003ff70  gp 80004800  a0 4001f2c0  a1 00000000\n" \
	"    ended; the rest of the system is unaffected\n"
#define CRASH2 "*** draw (pid 9) crashed: illegal instruction 00000000\n" \
	"    pc 80000010\n" \
	"    ended; the rest of the system is unaffected\n"

int main(void) {
	expect(found("", NULL), "an empty log: none");
	expect(found("init: done\nwm: hello\n", NULL), "a log with no crash: none");
	expect(found(" *** CPU MISMATCH ***\n*** this is not a crash line\n", NULL),
		"lines with *** but no \"crashed: \": none");
	expect(found("boot\n" CRASH1 "wm: after\n", CRASH1), "one crash, then more log: the crash and its indented lines");
	expect(found("boot\n" CRASH1 "x\n" CRASH2 "y\n", CRASH2), "two crashes: the last");
	expect(found("boot\n" CRASH2, CRASH2), "a crash at the very end of the log");
	{
		const char *cut = "boot\n*** term (pid 4) crashed: misaligned memory access\n    pc 8000";
		expect(found(cut, cut + 5), "a crash cut off by the end of the buffer: as far as it goes");
	}
	expect(found("\n*** a b crashed: x", "*** a b crashed: x"), "no newline at all after it");
	printf("zcrash: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
