/*
 * zco.c on the build machine: a coroutine run a slice at a time, and a
 * chain -- one coroutine resuming another, whose yields come back to
 * it -- as posix's streaming pipes use (docs/posix.md, "Pipes").
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_zco sw/common/tests/test_zco.c sw/common/zco.c
 */
#include <stdio.h>
#include <string.h>
#include "zco.h"

static int checks, fails;
#define CK(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, m); } } while (0)

static char trace[256];
static void mark(char c) { size_t n = strlen(trace); trace[n] = c; trace[n + 1] = 0; }

static zco_t a, b;
static unsigned char sa[128 * 1024], sb[128 * 1024];

static void inner(void *arg) {
	(void)arg;
	mark('1');
	CK(zco_current() == &b, "inside b, b is current");
	zco_yield();                    /* back to a */
	mark('2');
	zco_yield();
	mark('3');
}

static void outer(void *arg) {
	(void)arg;
	zco_init(&b, inner, NULL, sb, sizeof(sb));
	mark('a');
	CK(zco_resume(&b), "b yields to a");
	CK(zco_current() == &a, "and a is current again");
	mark('b');
	zco_yield();                    /* a to the main loop, b waiting inside */
	mark('c');
	size_t before = strlen(trace);
	CK(zco_resume(&a) && strlen(trace) == before, "a resuming itself: nothing runs");
	CK(zco_resume(&b), "b again, from a");
	CK(!zco_resume(&b) && b.done, "b ends");
	mark('d');
}

int main(void) {
	zco_init(&a, outer, NULL, sa, sizeof(sa));
	CK(zco_current() == NULL, "the main loop has none");
	CK(zco_resume(&a), "a yields to the main loop");
	CK(!strcmp(trace, "a1b"), trace);
	CK(zco_current() == NULL, "back in the main loop");
	CK(!zco_resume(&a) && a.done, "a ends");
	CK(!strcmp(trace, "a1bc23d"), trace);
	CK(!a.overflow && !b.overflow, "no overflow");
	CK(zco_current() == NULL, "none after");
	printf("test_zco: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
