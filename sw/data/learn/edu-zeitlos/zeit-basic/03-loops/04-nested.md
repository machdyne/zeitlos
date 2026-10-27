# Loops inside loops

```lesson
(objective "Put one loop inside another, and join conditions with AND, OR and NOT.")
(minutes 20)
(keywords "nested loop")
```

In this lesson you put loops inside loops, and ask two questions at
once.

A loop can hold another loop. This is a **nested loop**: for each pass
of the outer loop, the inner loop runs all the way through.

```basic
10 FOR R = 1 TO 4
20 FOR C = 1 TO R
30 PRINT "*";
40 NEXT C
50 PRINT
60 NEXT R
```

```text
*
**
***
****
```

The outer loop counts rows, R. The inner loop prints R stars, then line
50 ends the row.

```check
(number
  (ask "How many stars does the program above print in all?")
  (answer 10) (from "1 + 2 + 3 + 4")
  (wrong-if 4 "4 is the number of rows. Each row has as many stars as its number.")
  (wrong-if 16 "That would be 4 rows of 4. The inner loop runs to R, so the rows have 1, 2, 3 and 4 stars.")
  (hint "Row 1 has 1 star, row 2 has 2 ..."))
```

---

## Each NEXT closes its own FOR

The inner loop must end before the outer one: `NEXT C` comes before
`NEXT R`. Up to 6 loops can be running inside one another at once.

```basic
10 FOR A = 1 TO 3
20 FOR B = 1 TO 2
30 PRINT A * 10 + B; " ";
40 NEXT B
50 NEXT A
```

```check
(choice
  (ask "What does this program print?")
  (right "11 12 21 22 31 32")
  (wrong "11 21 31 12 22 32"
    "The inner loop, B, runs all the way through for each value of A: 11, 12, and only then A becomes 2.")
  (wrong "11 22 33"
    "A and B do not move together. For each A, B goes through 1 and 2.")
  (hint "Keep A at 1 while B goes 1, 2. Then A becomes 2."))
```

---

## AND, OR and NOT

Conditions can be joined:

- `A AND B` holds when both hold;
- `A OR B` holds when either holds, or both;
- `NOT A` holds when A does not.

```basic
10 INPUT "HOUR"; H
20 IF H >= 9 AND H < 17 THEN PRINT "OPEN" ELSE PRINT "CLOSED"
```

BASIC compares first, then joins: `H >= 9 AND H < 17` means
`(H >= 9) AND (H < 17)`.

```sources
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 4")
```

```check
(choice
  (ask "The program above runs, and you type 17. What does it print?")
  (right "CLOSED")
  (wrong "OPEN"
    "17 >= 9 holds, but 17 < 17 does not, and AND needs both.")
  (hint "Check each half: is 17 >= 9? Is 17 < 17?"))
```

---

```check
(choice
  (ask "Which condition holds when X is less than 0 or greater than 100?")
  (right "X < 0 OR X > 100")
  (wrong "X < 0 AND X > 100"
    "No number is both below 0 and above 100, so this never holds. Either one is enough: OR.")
  (wrong "X > 0 OR X < 100"
    "Every number is above 0 or below 100, so this always holds. The signs are the wrong way round.")
  (hint "Either of the two is enough."))
```

---

## Finding primes

A prime number can be divided exactly only by 1 and itself. This
program, from *Timeless Computing*, tries each number N from 2 to 50
with divisors D, using a loop built from `IF` and `GOTO` inside a `FOR`
loop:

```basic
10 FOR N = 2 TO 50
20 P = 1: D = 2
30 IF D * D > N THEN GOTO 60
40 IF N MOD D = 0 THEN P = 0
50 D = D + 1: GOTO 30
60 IF P THEN PRINT N; " ";
70 NEXT N
80 PRINT
```

P starts at 1 (prime) and becomes 0 when a divisor is found. `IF P`
holds when P is not 0.

```sources
(source "Timeless Computing (docs/tc.md), BASIC: Decisions and Loops")
```

```check
(number
  (ask "Line 10 is changed to 10 FOR N = 2 TO 10. How many numbers does the program print?")
  (answer 4)
  (explain "2, 3, 5 and 7.")
  (wrong-if 5 "9 is not prime: 3 divides it. And 1 is not tried, since N starts at 2.")
  (hint "Which of 2, 3, 4 ... 10 can be divided only by 1 and themselves?"))
```

So: a loop inside a loop runs all the way through for each pass of the
outer one, and `AND`, `OR` and `NOT` join conditions.
