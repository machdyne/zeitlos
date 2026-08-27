# calc

A four-function calculator. `sw/apps/calc`.

```
> run wm
> run calc
```

149x138, fixed size — a keypad has nothing to reveal by growing, and
screen space is scarce. The window dimensions are *derived* from the
button metrics rather than picked, so the window is exactly as wide as
the keypad needs.

## The arithmetic is in its own file

`calc_core.c` holds all of it and contains no drawing, windows or
messages. That's so `calc_test.c` can build it for the host:

```
cd sw/apps/calc && make test
```

A calculator that is subtly wrong is worse than no calculator —
nobody checks its answers, which is the entire point of having one.
This is the one app in the tree where correctness can be demonstrated
rather than asserted, so it is.

62 checks covering entry, decimals, sign, backspace, all four
operations, chaining, state transitions, division by zero, overflow
and negative results.

**Tests are written as keystroke sequences**, the way the thing is
actually used, not by calling the arithmetic directly. Almost every
classic calculator bug is a *state* bug — a digit after `=` extending
the answer, an operator applied twice, a decimal point changing where
the next digit lands — and calling `fix_mul()` in isolation would
never find one. The buttons and the tests speak the same alphabet, so
a button is exactly a tested keystroke.

It found two real bugs before the app ever ran:

- **Digit entry overflowed at seven digits.** It shifted left with
  `fix_mul(entry, 10 * CALC_SCALE)`, which forms the doubly-scaled
  product and only then brings it down — and that intermediate
  overflows an `int64` far short of the ten digits the display holds.
  Typing `9999999999` stopped at `999999`. A plain integer multiply
  needs no intermediate.
- **Backspace over a fractional digit removed nothing.** The
  truncation divisor was computed with one division too many, landing
  on the place still being kept, so `1.5` backspaced twice stayed
  `1.5`.

## Fixed point, not floating

There is no FPU, so floating point means soft-float: a large library
for something done once per keypress, and binary fractions that can't
represent `0.1` exactly — the wrong failure to accept on a machine
whose whole job is agreeing with you about decimal numbers. `0.1 + 0.2
= 0.3` is in the test suite for that reason.

A value is an `int64` of millionths (`CALC_DP` = 6). Six places chosen
against the display: the app is deliberately narrow, and spending more
characters on fraction than integer is the wrong trade for a pocket
calculator. Results are shown to `CALC_DIGITS` (10) significant digits.

64-bit arithmetic does pull in libgcc's `__muldi3`/`__divdi3` on this
target — a few hundred bytes for exactness, and the only cost in this
app that isn't obvious from the source, so it's called out in
`calc_core.h`.

**Overflow is refused, never wrapped.** Wrapping yields a number that
looks entirely reasonable and is completely wrong. Every operation
checks before it acts — signed overflow is undefined behaviour, so it
can't be detected by inspecting the result — and a result too large to
display is an error rather than something shown truncated.

Errors are **sticky**: every operation is a no-op until `C`, so an
error can't be arithmetic'd away into a plausible-looking number.

Multiply and divide round to nearest rather than truncating, or
`1.5 * 1.5` would come out as `2.249999` from operands that are
themselves exact.

## Keyboard

Everything works without a pointer, and not via Tab: digits, `.`,
`+ - * /`, Enter (`=`), Escape (clear) and Backspace all do the
obvious thing directly.

Tab focus over the keypad exists for consistency with the rest of the
system, and Space presses the focused button. **Enter is deliberately
not that** — it's `=` on a calculator, which is what a hand reaching
for it expects, and having it mean two different things depending on
where focus sat would be worse than having no focus at all.

## Behaviour worth knowing

- No operator precedence: `2 * 3 + 4` is 10, evaluated left to right,
  like every pocket calculator.
- `=` repeats the last operation: `2 + 3 =` then `=` gives 8.
- An operator pressed straight after another replaces it: `2 + *` is
  `2 *`.
- A digit after `=` starts a fresh number rather than extending the
  result.
- While typing, decimal places are shown **as typed** — `1.50` stays
  `1.50` rather than being tidied to `1.5` under your fingers. Results
  have trailing zeros stripped.
