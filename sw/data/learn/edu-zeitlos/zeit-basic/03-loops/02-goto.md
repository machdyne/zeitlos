# Loops with GOTO

```lesson
(objective "Make a program repeat with GOTO, and stop the repeating with IF.")
(minutes 20)
(keywords "loop (in a program)" "jump (in a program)" "algorithm")
```

In this lesson your programs learn to repeat.

`GOTO` followed by a line number is a **jump**: the
program goes on at that line instead of the next one.

```basic
10 PRINT "HELLO ";
20 GOTO 10
```

Line 20 jumps back to line 10, which prints again, and so on forever.
A part of a program that repeats is a **loop**. This one
never ends by itself: press Esc to stop it.

```try
(try (app "basic")
  (task "In BASIC, type NEW, then these two lines, then RUN. Watch the screen fill, press Esc, and come back.")
  (check (choice
    (ask "What stopped the program?")
    (right "Esc")
    (wrong "It stopped by itself when the screen was full"
      "When the screen is full, everything moves up and the program carries on. Only Esc stops it.")
    (hint "This loop has no end of its own."))))
```

---

## A loop that ends

Add a variable that counts, and an `IF` that jumps back only while
there is more to do:

```basic
10 N = 1
20 PRINT N
30 N = N + 1
40 IF N <= 3 THEN GOTO 20
```

Watch it run, a line at a time. The arrow is the line that has just
run.

```figure
(zb-trace '("10 N = 1" "20 PRINT N" "30 N = N + 1" "40 IF N <= 3 THEN GOTO 20")
  '((0 "N = 1" () "N starts at 1")
    (1 "N = 1" ("1") "print N")
    (2 "N = 2" ("1") "N goes up by 1")
    (3 "N = 2" ("1") "2 <= 3 holds: go to line 20")
    (1 "N = 2" ("1" "2") "print N")
    (2 "N = 3" ("1" "2") "N goes up by 1")
    (3 "N = 3" ("1" "2") "3 <= 3 holds: go to line 20")
    (1 "N = 3" ("1" "2" "3") "print N")
    (2 "N = 4" ("1" "2" "3") "N goes up by 1")
    (3 "N = 4" ("1" "2" "3") "4 <= 3 does not hold: on to the next line")
    (-1 "N = 4" ("1" "2" "3") "there is no next line: the program ends"))
  1.5)
```

```check
(number
  (ask "Line 40 is changed to 40 IF N <= 10 THEN GOTO 20. How many numbers does the program print?")
  (answer 10)
  (wrong-if 9 "N <= 10 holds for 10 too, so 10 is printed before the loop ends.")
  (wrong-if 11 "After 10 is printed, N becomes 11, and 11 <= 10 does not hold: the loop ends without printing 11.")
  (hint "It prints 1, 2, 3 ... up to which number?"))
```

---

## THEN with a number

`THEN 20` is short for `THEN GOTO 20`. So line 40 can be written
`40 IF N <= 3 THEN 20`.

`GOTO` needs a line number written as a number: `GOTO N` is not
allowed. A jump to a line that does not exist stops the program with
`NO LINE`.

```check
(choice
  (ask "Which line jumps to line 100 when X is 0?")
  (right "50 IF X = 0 THEN 100")
  (wrong "50 IF X = 0 THEN X = 100"
    "That stores 100 in X. To jump, the number must come straight after THEN, or after GOTO.")
  (wrong "50 GOTO X = 0"
    "GOTO takes only a line number. The condition needs an IF.")
  (hint "THEN followed by a number means GOTO that line."))
```

---

## An algorithm

An **algorithm** is a method for solving a problem in a finite number
of exact steps. One of the oldest, recorded by Euclid around 300 BC, finds the largest number that divides two numbers exactly:
while B is not 0, replace A with B, and B with the remainder of A
divided by B. The answer is A.

```basic
10 A = 1071: B = 462
20 IF B = 0 THEN GOTO 60
30 T = A MOD B
40 A = B: B = T
50 GOTO 20
60 PRINT A
```

The remainders are 147, then 21, then 0, so it prints `21`.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Algorithms; Three Languages Side by Side: Greatest Common Divisor")
```

```check
(number
  (ask "Line 10 is changed to 10 A = 12: B = 8. What does the program print?")
  (answer 4) (true "12 / 4 = 3") (true "8 / 4 = 2")
  (explain "12 MOD 8 is 4: A = 8, B = 4. 8 MOD 4 is 0: A = 4, B = 0. It prints 4.")
  (wrong-if 8 "Follow the loop once more: after the first pass A is 8 and B is 4, and B is not yet 0.")
  (wrong-if 2 "2 divides both, but 4 does too, and the program finds the largest.")
  (hint "First pass: T = 12 MOD 8 = 4, then A = 8 and B = 4.")
  (hint "Second pass: T = 8 MOD 4 = 0, then A = 4 and B = 0. Now B is 0."))
```

---

```basic
10 N = 10
20 PRINT N;
30 N = N - 3
40 IF N > 0 THEN 20
```

```check
(choice
  (ask "What does this program print?")
  (right "10741")
  (wrong "1074"
    "After 4 is printed N becomes 1, and 1 > 0 holds, so 1 is printed too.")
  (wrong "107410"
    "After 1 is printed N becomes -2, and -2 > 0 does not hold, so the loop ends.")
  (hint "Follow N: 10, 7, 4, 1, -2. Which of these are printed?"))
```

So: `GOTO` jumps to a line, a jump backwards makes a loop, and an `IF`
decides when the loop ends.
