# Counting with FOR

```lesson
(objective "Repeat part of a program a set number of times with FOR and NEXT, counting in any step.")
(minutes 20)
(keywords "loop variable (of a FOR loop)")
```

In this lesson you meet the easiest way to repeat something a set
number of times.

Counting loops are so common that BASIC has a pair of statements for
them. `FOR` starts the loop and `NEXT` ends it; the lines between them
run once for each value of the **loop variable**:

```basic
10 FOR I = 1 TO 3
20 PRINT I
30 NEXT I
40 PRINT "DONE"
```

```figure
(zb-trace '("10 FOR I = 1 TO 3" "20 PRINT I" "30 NEXT I" "40 PRINT \"DONE\"")
  '((0 "I = 1" () "I starts at 1")
    (1 "I = 1" ("1") "print I")
    (2 "I = 2" ("1") "I + 1 is 2, not past 3: back")
    (1 "I = 2" ("1" "2") "print I")
    (2 "I = 3" ("1" "2") "I + 1 is 3, not past 3: back")
    (1 "I = 3" ("1" "2" "3") "print I")
    (2 "I = 3" ("1" "2" "3") "I + 1 would be 4, past 3: on")
    (3 "I = 3" ("1" "2" "3" "DONE") "the loop is over")
    (-1 "I = 3" ("1" "2" "3" "DONE") "the program ends"))
  1.5)
```

```sources
(source "Timeless Computing (docs/tc.md), BASIC: A First Program")
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 5, FOR")
```

```check
(number
  (ask "Line 10 is changed to 10 FOR I = 1 TO 8. How many numbers does the program print before DONE?")
  (answer 8)
  (hint "I takes every value from 1 to 8."))
```

---

## STEP

`STEP` sets how much the loop variable changes each time. Without it,
the step is 1.

```basic
10 FOR I = 0 TO 20 STEP 5
20 PRINT I; " ";
30 NEXT I
```

prints `0 5 10 15 20`. A negative step counts down:
`FOR I = 3 TO 1 STEP -1` gives 3, 2, 1.

```check
(number
  (ask "How many times does the body of FOR I = 10 TO 30 STEP 10 run?")
  (answer 3)
  (explain "I is 10, 20 and 30.")
  (wrong-if 2 "30 is included: the loop runs for I = 30 too, because 30 is not past the end.")
  (wrong-if 20 "That is 30 - 10. Count the values instead: 10, 20, 30.")
  (hint "List the values of I: start at 10, add 10 each time, stop after 30."))
```

---

## At least once

In this BASIC, the body of a `FOR` loop **always runs at least once**,
even when the start is already past the end. `FOR I = 5 TO 1` (with no
`STEP`, so counting up) runs once, with I = 5, then stops.

After the loop, the variable keeps the last value it was used with:
after `FOR I = 1 TO 3`, I is 3.

```check
(choice
  (ask "What does this print? 10 FOR I = 5 TO 1: PRINT I: NEXT I")
  (right "5")
  (wrong "5, 4, 3, 2, 1"
    "Without STEP -1 the loop counts up, and 5 is already past 1. To count down, write STEP -1.")
  (wrong "Nothing"
    "In this BASIC the body always runs at least once, so it prints 5 before NEXT finds the loop is over.")
  (hint "Which way does a loop count without STEP? And how often does the body run, at least?"))
```

---

## Building up a result

A loop often builds up a result. This program multiplies F by 1, 2, 3
and so on up to 7:

```basic
10 F = 1
20 FOR I = 1 TO 7
30 F = F * I
40 NEXT I
50 PRINT F
```

It prints `5040`, which is 1 * 2 * 3 * 4 * 5 * 6 * 7.

```sources
(source "Timeless Computing (docs/tc.md), Three Languages Side by Side: Factorial")
```

```example
(example
  (problem "What does the program print if line 20 is FOR I = 1 TO 4?")
  (step "F starts at 1.")
  (step "I = 1: F = 1. I = 2: F = 2. I = 3: F = 6.")
  (step (number (ask "I = 4: what is F now?") (answer 24) (from "6 * 4")
    (wrong-if 10 "F is multiplied by I, not added to: 6 * 4.")))
  (answer "24"))
```

---

```basic
10 S = 0
20 FOR I = 1 TO 5
30 S = S + I
40 NEXT I
50 PRINT S
```

```check
(number
  (ask "What does this program print?")
  (answer 15) (from "1 + 2 + 3 + 4 + 5")
  (wrong-if 5 "S does not count the passes: it adds I each time, 1 + 2 + 3 + 4 + 5.")
  (wrong-if 10 "That is 1 + 2 + 3 + 4. The loop includes I = 5.")
  (hint "S adds up the values of I."))
```

---

```check
(text
  (ask "Finish the line that prints 10, 8, 6, 4, 2: 10 FOR I = 10 TO 2 STEP ...")
  (accept "-2")
  (wrong-if "2" "A positive step counts up, and 10 is already past 2, so the loop would run once. Counting down needs a negative step.")
  (hint "Each value is 2 less than the one before."))
```

So: `FOR` and `NEXT` repeat the lines between them for each value of
the loop variable, `STEP` sets how it counts, and the body always runs
at least once.
