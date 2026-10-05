/*
 * Host tests for sw/common/zexec.h: the stack size code in the flags
 * word, and the loader's answer to it.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_zexec sw/common/tests/test_zexec.c
 *   /tmp/test_zexec
 *
 * See docs/executables.md, "The stack size".
 */

#include <stdio.h>
#include <string.h>

#include "zexec.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

#define KB 1024u
#define MB (1024u * 1024u)
#define DEF (16 * KB)
#define CAP (8 * MB)

static void test_table(void) {
	static const uint32_t want[16] = {
		0, 8 * KB, 16 * KB, 32 * KB, 64 * KB, 128 * KB, 256 * KB,
		512 * KB, 1 * MB, 2 * MB, 4 * MB, 8 * MB, 16 * MB, 32 * MB,
		64 * MB, 0,
	};
	for (uint32_t c = 0; c < 16; c++)
		CHECK(z_exec_stack_bytes(c) == want[c], "code to bytes");
}

static void test_decide(void) {
	uint32_t n;

	CHECK(z_exec_stack(0, DEF, CAP, &n) == Z_EXEC_STACK_OK && n == DEF,
		"0 is the default");
	CHECK(z_exec_stack(1, DEF, CAP, &n) == Z_EXEC_STACK_OK && n == 8 * KB,
		"1 is 8KB");
	CHECK(z_exec_stack(10, DEF, CAP, &n) == Z_EXEC_STACK_OK && n == 4 * MB,
		"10 is 4MB");
	CHECK(z_exec_stack(11, DEF, CAP, &n) == Z_EXEC_STACK_OK && n == 8 * MB,
		"11 is the cap, granted");
	CHECK(z_exec_stack(12, DEF, CAP, &n) == Z_EXEC_STACK_OVER_CAP &&
		n == 16 * MB, "12 is over the cap, and says how much");
	CHECK(z_exec_stack(14, DEF, CAP, &n) == Z_EXEC_STACK_OVER_CAP,
		"14 is over the cap");
	CHECK(z_exec_stack(15, DEF, CAP, &n) == Z_EXEC_STACK_IS_RESERVED &&
		n == 0, "15 is reserved");
	CHECK(z_exec_stack(0x0010, DEF, CAP, &n) == Z_EXEC_STACK_BAD_FLAGS &&
		n == 0, "bit 4 is reserved");
	CHECK(z_exec_stack(0x8003, DEF, CAP, &n) == Z_EXEC_STACK_BAD_FLAGS,
		"bit 15 is reserved, whatever the code");
	CHECK(z_exec_stack(14, DEF, 64 * MB, &n) == Z_EXEC_STACK_OK &&
		n == 64 * MB, "a raised cap grants 64MB");
}

static void test_parse(void) {
	uint8_t h[16] = { 'Z', 'E', 'X', 'E', 1, 0, 0x0a, 0, 0x10, 0, 0, 0 };
	z_exec_info_t xi;

	CHECK(z_exec_parse(h, sizeof(h), 1024, &xi) == 0 && xi.is_zexe &&
		xi.flags == 10, "the header carries the code through");
	h[6] = 0x0f;
	CHECK(z_exec_parse(h, sizeof(h), 1024, &xi) == 0 && xi.flags == 15,
		"a reserved code is not a parse error; the loader refuses it");
	CHECK(z_exec_parse("raw", 3, 3, &xi) == 0 && !xi.is_zexe &&
		xi.flags == 0, "a raw binary does not ask");
}

int main(void) {
	test_table();
	test_decide();
	test_parse();
	printf("test_zexec: %d checks, %d failed\n", run, failed);
	return failed != 0;
}
