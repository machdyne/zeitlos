/*
 * Zeitlos SOC -- shared scaffolding for the zeitlos32 assembly tests.
 *
 * Conventions, matching tb_zeitlos32.v:
 *
 *   TEST_PORT   0xe0000000  store 0 to finish and pass, store an id
 *                           to finish and fail with that id
 *   IRQ_PORT    0xe0000010  store a bitmask to drive the core's irq
 *                           input lines
 *   UART_PORT   0xf0000000  store a byte to print it
 *
 * The check macros clobber t0 and t1 (x5, x6). Do not hold a value
 * being checked in either of those.
 *
 * All labels generated here are of the form .Lz32_<n>_\@ -- \@ is
 * gas's per-macro-invocation counter, so they can never collide with
 * a test's own numeric local labels. Using plain `1:` here instead
 * looks harmless and is not: a test writing `beq a0, a1, 1f` would
 * silently bind to the `1:` inside whichever macro was expanded next,
 * which produces a test that hangs rather than one that fails.
 *
 * Every check needs a UNIQUE id -- that id is the entire failure
 * message, so duplicates make a failure ambiguous. Each test file
 * uses its own hundreds range.
 */

#define TEST_PORT  0xe0000000
#define IRQ_PORT   0xe0000010
#define UART_PORT  0xf0000000

/* Set up the reset and interrupt vectors. PROGADDR_RESET is 0 and
 * PROGADDR_IRQ is 0x10, exactly as rtl/sysctl.v configures the core,
 * so the layout here has to match sw/bios/boot_picorv32.S's. */
.macro TEST_ENTRY
	.section .text
	.globl _start
_start:
	j	_test_main			/* 0x00 */
	nop						/* 0x04 */
	nop						/* 0x08 */
	nop						/* 0x0c */
	.org 0x10
_irq_vec:
	j	_default_irq		/* overridden by tests that want one */
	.org 0x40
_default_irq:
	li	t0, TEST_PORT
	li	t1, 9999			/* an unexpected interrupt is a failure */
	sw	t1, 0(t0)
.Lz32_irqhang:
	j	.Lz32_irqhang
_test_main:
.endm

/* Finish: pass. */
.macro TEST_PASS
	li	t0, TEST_PORT
	sw	x0, 0(t0)
.Lz32_pass_\@:
	j	.Lz32_pass_\@
.endm

/* Finish: fail, reporting \id. */
.macro TEST_FAIL id
	li	t0, TEST_PORT
	li	t1, \id
	sw	t1, 0(t0)
.Lz32_fail_\@:
	j	.Lz32_fail_\@
.endm

/* Fail unless \reg holds \exp. */
.macro CHECK_EQ id, reg, exp
	li	t1, \exp
	beq	\reg, t1, .Lz32_ok_\@
	TEST_FAIL \id
.Lz32_ok_\@:
.endm

/* Fail unless \reg is nonzero. */
.macro CHECK_NZ id, reg
	bne	\reg, x0, .Lz32_ok_\@
	TEST_FAIL \id
.Lz32_ok_\@:
.endm

/* Fail unless \reg is zero. */
.macro CHECK_Z id, reg
	beq	\reg, x0, .Lz32_ok_\@
	TEST_FAIL \id
.Lz32_ok_\@:
.endm

/* Fail unless \a and \b are equal. */
.macro CHECK_REG id, a, b
	beq	\a, \b, .Lz32_ok_\@
	TEST_FAIL \id
.Lz32_ok_\@:
.endm
