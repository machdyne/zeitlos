# Variables

```lesson
(objective "Store numbers in variables, change them, and follow a program's variables line by line.")
(minutes 20)
(keywords "variable (in a program)" "value (of a variable)" "assignment (in a program)")
```

In this lesson you give a program a memory.

A **variable** is a named place that holds a number. In
this BASIC there are 26 of them, named by the letters `A` to `Z`. The
number a variable holds is its **value**.

```basic
10 A = 5
20 PRINT A * 2
```

Line 10 puts 5 into `A`. Line 20 uses it, and prints `10`. A line like
10 is an **assignment**: work out the right-hand side, and store it in
the variable on the left. (You may write `LET A = 5`; `LET` is
optional.)

Every variable is 0 when `RUN` starts.

```basic
10 B = 4
20 PRINT B + B * B
```

```check
(number
  (ask "What does the program just above print?")
  (answer 20) (from "4 + 4 * 4")
  (wrong-if 32 "Multiplication comes first: B * B is 16, and 4 + 16 is 20.")
  (hint "B is 4. Replace each B with 4, then calculate as usual."))
```

---

## X = X + 1

```basic
10 X = 3
20 X = X + 1
30 PRINT X
```

Line 20 is not an equation, which could never hold. It is an
instruction: take the value of `X`, add 1, and store the result back in
`X`. The old value, 3, is replaced. The program prints `4`.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: State")
```

```basic
10 N = 10
20 N = N * 2
30 N = N - 5
40 PRINT N
```

```check
(number
  (ask "What does this program print?")
  (answer 15) (from "10 * 2 - 5")
  (wrong-if 5 "Line 30 takes the value N has after line 20, which is 20, not the 10 it started with.")
  (wrong-if 10 "Each line changes N. After line 20 it is 20; after line 30, 15.")
  (hint "Follow N line by line: 10, then ... ."))
```

---

## Following a program

To find out what a program does, follow its variables line by line, as
the computer does. Write down each variable's value after each line.

```basic
10 A = 3
20 B = A * 2
30 A = A + B
40 PRINT A; " "; B
```

```example
(example
  (problem "What does this program print?")
  (step "After line 10: A is 3, B is 0.")
  (step "After line 20: B is 3 * 2, which is 6. A is still 3.")
  (step "After line 30: A is 3 + 6, which is 9. B is still 6.")
  (step "Line 40 prints A, a space, and B.")
  (answer "9 6"))
```

Now one for you:

```basic
10 P = 7
20 Q = P
30 P = 1
40 PRINT Q
```

```check
(number
  (ask "What does this program print?")
  (answer 7)
  (wrong-if 1 "Line 20 copies the value P has at that moment, 7, into Q. Changing P later does not change Q.")
  (hint "Q gets a copy of P's value at line 20. What was it then?"))
```

---

## One letter each

A variable's name is one letter. `SCORE = 5` is a `SYNTAX ERROR`: BASIC
has no variable called SCORE. Choose letters that remind you of their
job: `S` for a score, `T` for a time, and write down what each is for.

```check
(choice
  (ask "Which line stores 100 in a variable?")
  (right "10 M = 100")
  (wrong "10 MONEY = 100"
    "A variable's name is a single letter. BASIC cannot read MONEY as a name, so it is a SYNTAX ERROR.")
  (wrong "10 100 = M"
    "The variable goes on the left: the value on the right is stored in it.")
  (hint "One letter, on the left of the = sign."))
```

---

## The largest number

A variable holds an integer from -32768 to 32767. A result beyond
that **wraps round** to the other end: 32767 + 1 gives -32768. A
program that counts high must keep its numbers in that range.

```sources
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 2")
```

```check
(choice
  (ask "A program sets X = 32767 and then X = X + 1. What is X now?")
  (right "-32768")
  (wrong "32768"
    "32768 is too big for this BASIC: the largest number is 32767. One more wraps round to the smallest, -32768.")
  (wrong "An error stops the program"
    "BASIC does not stop: the number wraps round to the other end of the range, -32768.")
  (hint "The numbers go round like a counter: past the top comes the bottom."))
```

---

```basic
10 A = 5
20 B = 3
30 A = A + B
40 B = A - B
50 PRINT B
```

```check
(number
  (ask "What does this program print?")
  (answer 5) (from "(5 + 3) - 3")
  (wrong-if 3 "Line 40 uses the new A, 8, from line 30. 8 - 3 is 5.")
  (wrong-if 2 "That is 5 - 3, but by line 40 A is no longer 5: line 30 made it 8.")
  (hint "After line 30, A is 8. Now line 40."))
```

So: a variable is a lettered place for a number, an assignment works
out the right side and stores it on the left, and you follow a
program by following its variables.
