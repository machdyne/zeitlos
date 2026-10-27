# Asking for a number

```lesson
(objective "Write a program that asks for numbers with INPUT and works with the answers.")
(minutes 20)
(keywords "input (to a program)" "prompt (in a program)" "output (of a program)")
```

In this lesson your programs start asking questions.

What a program is given while it runs is its **input**; what it shows
is its **output**. `INPUT` stops the program, waits for a number to be
typed, and stores it in a variable:

```basic
10 INPUT "HOW OLD ARE YOU"; A
20 PRINT "NEXT YEAR YOU WILL BE "; A + 1
```

Line 10 prints its **prompt**, the text in quotes, then a question mark
and a space: `HOW OLD ARE YOU? `. The person types a number and presses
Enter, and the number goes into `A`. Then the program goes on.

`INPUT A`, without a prompt, prints only `? `.

```sources
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 5, INPUT")
```

```check
(number
  (ask "The program above runs, and you type 41 and press Enter. What number does it print?")
  (answer 42) (from "41 + 1")
  (hint "A is 41. Line 20 prints A + 1."))
```

---

## A program that calculates

```basic
10 INPUT "WIDTH"; W
20 INPUT "LENGTH"; L
30 PRINT "AREA: "; W * L
```

```try
(try (app "basic")
  (task "In BASIC, type NEW, then this program, then RUN. Answer 6 for the width and 4 for the length. Come back.")
  (check (number
    (ask "What area did it print?")
    (answer 24) (from "6 * 4")
    (wrong-if 10 "That is 6 + 4. Line 30 multiplies: W * L.")
    (hint "W is 6 and L is 4. What is W * L?"))))
```

---

## Numbers only

`INPUT` reads a number: an optional minus sign and the digits after it.
Anything after the digits is ignored, so `12 APPLES` gives 12. An
answer with no digits at all, such as `TWELVE`, gives 0.

```check
(number
  (ask "A program says INPUT N. You type TEN and press Enter. What is N?")
  (answer 0)
  (wrong-if 10 "BASIC does not read numbers written in letters. With no digits at all, the value is 0.")
  (hint "INPUT reads only digits, and an answer with none gives 0."))
```

---

## Esc while it waits

While the program waits at `INPUT`, Esc only erases what you have typed:
it does not stop the program. Answer the question, and the program goes
on. If a program asks again and again, give it a way out, such as "type
0 to stop" (you learn how a program decides that in the next unit).

```sources
(source "sw/apps/basic/basic_app.c: Esc stops only a running program; a program waiting at INPUT is not running")
```

```check
(choice
  (ask "A program is waiting at INPUT, and you want it to go on. What do you do?")
  (right "Type a number and press Enter")
  (wrong "Press Esc"
    "While a program waits at INPUT, Esc only erases what you typed. The program goes on when you answer.")
  (wrong "Type RUN"
    "Whatever you type now is the answer to INPUT. RUN has no digits, so it would give 0.")
  (hint "The program is waiting for an answer."))
```

---

```example
(example
  (problem "Write a program that asks for a number of minutes and prints it in hours and minutes.")
  (step "Ask: 10 INPUT \"MINUTES\"; M")
  (step "Whole hours: M / 60. Minutes left over: M MOD 60.")
  (step "Print: 20 PRINT M / 60; \" H \"; M MOD 60; \" MIN\"")
  (step (number (ask "What does it print, in minutes after the H, for 135?") (answer 15) (from "135 - 2 * 60")
    (wrong-if 2 "2 is the hours, 135 / 60. The minutes left over are 135 MOD 60.")))
  (answer "For 135 it prints 2 H 15 MIN."))
```

---

```basic
10 INPUT "PRICE"; P
20 INPUT "HOW MANY"; N
30 T = P * N
40 PRINT "TOTAL "; T
```

```check
(number
  (ask "You answer 15 for the price and 3 for how many. What total does it print?")
  (answer 45) (from "15 * 3")
  (wrong-if 18 "That adds. Line 30 multiplies: T = P * N.")
  (hint "T is P times N."))
```

So: `INPUT` prints a prompt and a question mark, waits for a number,
and stores it, so one program can work with many different numbers.
