/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Test suite for calc_core.c. Runs on the HOST, not the target:
 *
 *   cd sw/apps/calc && make test
 *
 * A calculator that is subtly wrong is worse than no calculator --
 * nobody checks its answers, which is the entire point of having one.
 * calc_core.c is deliberately free of drawing, windows and messages so
 * that this can exist.
 *
 * Tests are written as keystroke sequences, the way the thing is
 * actually used, rather than by calling the arithmetic directly:
 * almost every classic calculator bug is a STATE bug (a digit after
 * `=` extending the answer, an operator applied twice, a decimal point
 * changing where the next digit lands) and calling fix_mul() in
 * isolation would never find one.
 */

#include <stdio.h>
#include <string.h>

#include "calc_core.h"

static int checks, fails;

// Feeds a key sequence and compares the display against `want`.
//
//   0-9 . + - * /   as themselves
//   =               equals
//   c               clear
//   n               negate
//   <               backspace
static void seq(const char *keys, const char *want) {

	calc_t c;
	calc_reset(&c);

	for (const char *k = keys; *k; k++) {
		if (*k >= '0' && *k <= '9') calc_digit(&c, *k - '0');
		else if (*k == '.') calc_point(&c);
		else if (*k == '=') calc_equals(&c);
		else if (*k == 'c') calc_reset(&c);
		else if (*k == 'n') calc_sign(&c);
		else if (*k == '<') calc_backspace(&c);
		else if (*k == '+' || *k == '-' || *k == '*' || *k == '/')
			calc_op(&c, *k);
	}

	char got[32];
	calc_format(&c, got, sizeof(got));

	checks++;

	if (strcmp(got, want)) {
		printf("  FAIL  %-22s got \"%s\"  want \"%s\"\n", keys, got, want);
		fails++;
	}

}

int main(void) {

	printf("entry:\n");
	seq("0", "0");
	seq("7", "7");
	seq("123", "123");
	seq("000", "0");
	seq("102", "102");

	printf("decimal entry:\n");
	seq("1.5", "1.5");
	seq(".5", "0.5");
	seq("1.", "1.");                 // point shown before any digit
	seq("1.50", "1.50");             // as typed, not tidied
	seq("1..5", "1.5");              // second point ignored
	seq("0.000001", "0.000001");     // full precision
	seq("0.0000009", "0.000000");    // beyond precision: digit refused

	printf("sign:\n");
	seq("5n", "-5");
	seq("5nn", "5");
	seq("5n+3=", "-2");
	seq("0n", "0");

	printf("backspace:\n");
	seq("123<", "12");
	seq("123<<", "1");
	seq("123<<<", "0");
	seq("1.5<", "1.");
	seq("1.5<<", "1");
	seq("5=<", "5");                 // a result cannot be backspaced

	printf("the four operations:\n");
	seq("2+3=", "5");
	seq("9-4=", "5");
	seq("6*7=", "42");
	seq("8/2=", "4");
	seq("1/2=", "0.5");
	seq("1/3=", "0.333333");
	seq("2.5*4=", "10");
	seq("1.5*1.5=", "2.25");         // rounding, not truncation
	seq("0.1+0.2=", "0.3");          // the one binary floats get wrong

	printf("chaining:\n");
	seq("1+2+3=", "6");
	seq("2*3+4=", "10");             // left to right, no precedence
	seq("100/4/5=", "5");
	seq("1+2=", "3");
	seq("2+3+", "5");                // operator shows the running total

	printf("state:\n");
    seq("2+3=4", "4");               // digit after = starts fresh
	seq("2+3=+1=", "6");             // result feeds the next operation
	seq("2+*3=", "6");               // operator replaces a pending one
	seq("2++3=", "5");
	seq("5=", "5");                  // = with nothing pending
	seq("2+3==", "8");               // = repeats the last operation
	seq("2+3===", "11");
	seq("2*3==", "18");
	seq("7c3", "3");                 // clear really clears
	seq("2+3c4+1=", "5");

	printf("division by zero:\n");
	seq("1/0=", "error");
	seq("0/0=", "error");
	seq("1/0=5", "error");           // sticky: cannot be typed away
	seq("1/0=+1=", "error");
	seq("1/0=c5", "5");              // clear recovers

	printf("overflow:\n");
	seq("9999999999*9=", "error");
	seq("9999999999+9999999999=", "error");
	seq("9999999999*9999999999=", "error");
	seq("9999999999", "9999999999"); // the largest displayable entry
	seq("99999999999", "9999999999");// further digits refused, not wrapped
	seq("9999999999n-9999999999=", "error");

	printf("negative results:\n");
	seq("3-5=", "-2");
	seq("0-0.5=", "-0.5");
	seq("2*3n=", "-6");
	seq("6n/3=", "-2");
	seq("1n/3=", "-0.333333");       // rounds away from zero, like +

	printf("\n%d checks", checks);
	printf(fails ? ", %d FAILED\n" : ", all passed\n", fails);

	return fails != 0;

}
