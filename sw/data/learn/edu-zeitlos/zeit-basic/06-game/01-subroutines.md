# Subroutines

```lesson
(objective "Put a task into a subroutine, call it with GOSUB, return with RETURN, and keep the main program apart with END.")
(minutes 20)
(keywords "subroutine" "pseudocode")
```

In this lesson you learn to split a program into parts, ready to build
a game.

A **subroutine** is a group of lines that does one task, which the
rest of the program can use as often as it needs. `GOSUB` jumps to it,
and `RETURN` jumps back to just after the `GOSUB`:

```basic
10 FOR N = 1 TO 3
20 GOSUB 100
30 NEXT N
40 END
100 REM PRINT A LINE OF N STARS
110 FOR S = 1 TO N
120 PRINT "*";
130 NEXT S
140 PRINT
150 RETURN
```

```text
*
**
***
```

`REM` starts a remark: a note for the reader, which BASIC skips.

```sources
(source "Timeless Computing (docs/tc.md), BASIC: Subroutines")
```

```check
(number
  (ask "After the subroutine runs for the first time, at which line does the program go on?")
  (answer 30)
  (wrong-if 100 "100 is where the subroutine starts. RETURN goes back to just after the GOSUB on line 20.")
  (wrong-if 10 "RETURN does not start the loop again: it goes on after the GOSUB, at line 30, whose NEXT then repeats the loop.")
  (hint "RETURN goes back to the line after the GOSUB."))
```

---

## Why END matters

Line 40, `END`, stops the program. Without it, the program would run on
from line 40 into the subroutine, and at line 150 `RETURN` would have
no `GOSUB` to go back to: `NO GOSUB IN 150`.

```check
(choice
  (ask "You delete line 40 and RUN. What happens?")
  (right "The three rows are printed, then one more row, then NO GOSUB IN 150")
  (wrong "Only the three rows, as before"
    "Without END the program carries on into line 100 after the loop, prints a row of stars, and reaches RETURN with no GOSUB.")
  (wrong "A SYNTAX ERROR when you delete the line"
    "Deleting a line is always allowed. The trouble shows only when the program runs past the loop.")
  (hint "After line 30's loop is over, which line comes next?"))
```

---

## Sharing variables

All 26 variables belong to the whole program, so the main program and
its subroutines share them: here the loop sets N and the subroutine
reads it. Be careful that a subroutine does not change a variable the
main program is still using. A list of what each variable is for, in
`REM` lines at the top, helps.

```check
(choice
  (ask "A main program counts with N in a FOR loop, and calls a subroutine that uses N to count too. What goes wrong?")
  (right "The subroutine changes N, the variable the main loop counts with")
  (wrong "Nothing: a subroutine has variables of its own"
    "In this BASIC there is only one set of 26 variables. The subroutine's N is the main loop's N.")
  (wrong "BASIC refuses two loops in one program"
    "Many loops are allowed, even inside one another. The trouble is that both use N.")
  (hint "How many variables called N are there?"))
```

---

## Planning in plain language

Before a bigger program, write its plan as **pseudocode**: the steps
in plain language, a step a line. A plan for a game of Catch:

```text
show the title and wait for a key
set the score to 0
start a ball at the top
repeat:
    read a key, and move the bat
    move the ball down
    draw the screen
    when the ball reaches the bat's row:
        caught: add 1 to the score; missed: count a miss
        after 3 misses, the game is over
        otherwise start a new ball
```

Each part that is used in more than one place, such as "start a ball",
becomes a subroutine.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Algorithms; BASIC: Subroutines")
```

```check
(choice
  (ask "Which part of the Catch plan is best made a subroutine?")
  (right "Start a ball at the top: it is needed at the start and after every catch or miss")
  (wrong "Set the score to 0: it is one short line used once"
    "A subroutine is worth it when a task is used in more than one place, or is long. This is a single assignment.")
  (wrong "The whole game"
    "The game is the main program. Subroutines are its parts.")
  (hint "Which step is done more than once?"))
```

---

```basic
10 A = 2
20 GOSUB 100
30 GOSUB 100
40 PRINT A
50 END
100 A = A * A
110 RETURN
```

```check
(number
  (ask "What does this program print?")
  (answer 16) (from "(2 * 2) * (2 * 2)")
  (wrong-if 4 "The subroutine runs twice: 2 * 2 is 4, then 4 * 4 is 16.")
  (wrong-if 8 "The subroutine squares A, it does not double it: 2 becomes 4, then 4 becomes 16.")
  (hint "Line 100 runs twice. A is 2, then ... ."))
```

So: a subroutine is a task called with `GOSUB` and ended with
`RETURN`, `END` keeps the main program out of it, and pseudocode plans
a program before you write it.
