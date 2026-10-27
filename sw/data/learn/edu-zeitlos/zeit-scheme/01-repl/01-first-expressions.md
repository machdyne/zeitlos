# First expressions

```lesson
(objective "Open repl and work out sums by typing expressions.")
(minutes 15)
(keywords "Scheme (the language)" "REPL (read-eval-print loop)" "prompt (in repl)" "expression (in Scheme)" "value (in Scheme)" "procedure (in Scheme)" "argument (of a procedure)")
```

You are going to type your first lines of a programming language.

Zeitlos comes with **Scheme**, a small programming language, in the app
`repl`. repl is a **REPL**, short for read-eval-print loop: it reads
what you type, works it out, prints the answer, and waits for the next.

To reach it, open `term` (the terminal) from the dock. Its window shows
a panel of choices with REPL already chosen: press Enter. repl then
shows its **prompt**, `>`, which means it is waiting for you.

```sources
(source "Zeitlos docs: repl.md, Starting; terminal.md, Starting out: the panel")
```

```check
(choice
  (ask "What does repl do after it prints an answer?")
  (right "It waits at the prompt for the next thing you type")
  (wrong "It closes, and you open it again for the next one"
    "repl is a loop: read, work out, print, then read again. It stays until you leave it.")
  (wrong "It works out the same thing again"
    "It works out each thing you type once. Then it waits for the next.")
  (hint "The L in REPL stands for loop."))
```

---

## An expression

Type this at the prompt and press Enter:

```
> (+ 1 2)
3
```

`(+ 1 2)` is an **expression**: something Scheme can work out. What it
works out to, 3, is its **value**.

In brackets, the first item says what to do, and the rest are what to
do it to. `+` is a **procedure**: it makes a value from the values you
give it. Those values, 1 and 2, are its **arguments**. Spaces separate
the items.

```check
(number
  (ask "What does repl answer to `(* 6 7)`? The procedure `*` multiplies.")
  (answer 42) (from "6 * 7")
  (wrong-if 13 "That is 6 + 7. The procedure in front is `*`, which multiplies.")
  (hint "The first item is the procedure; the others are its arguments.")
  (hint "6 times 7."))
```

---

## The procedure comes first

Scheme always writes the procedure first, even for the sums you would
write as 10 - 4:

| Expression | Value |
|---|---|
| (- 10 4) | 6 |
| (* 3 5) | 15 |
| (+ 1 2 3 4) | 10 |

The arguments keep their order: `(- 10 4)` is 10 take away 4. And a
procedure like `+` takes any number of arguments, not just two.

```check
(number
  (ask "What is the value of `(- 20 5 3)`?")
  (answer 12) (from "20 - 5 - 3")
  (wrong-if 18 "That is 20 - (5 - 3). Each argument after the first is taken away from it, in turn: 20 - 5 - 3.")
  (wrong-if -12 "The first argument is the one taken away from: 20 take away 5, take away 3.")
  (hint "Start from 20, then take away each of the other arguments."))
```

---

## When the brackets do not match

repl works an expression out only when its brackets are closed. If you
press Enter while one is still open, it shows `.` at the start of a new
line and waits for the rest:

```
> (+ 1 2
. 3)
6
```

Type the missing `)` and press Enter. To give up on what you are
typing instead, press **Ctrl+C**: repl drops it and shows a fresh
prompt.

```sources
(source "Zeitlos source: sw/apps/repl/repl.c, form_complete(); sw/common/zline.c, Ctrl+C")
```

```check
(choice
  (ask "You typed `(* 2 (+ 3 4)` and pressed Enter. What happens?")
  (right "repl shows . and waits, because one bracket is still open")
  (wrong "repl answers 14"
    "Count the brackets: two open, one closed. repl works nothing out until every bracket is closed.")
  (wrong "repl answers with an error"
    "An open bracket is not a mistake yet: you may be about to type the rest. repl waits for it.")
  (hint "Count the ( and the ) in what was typed."))
```

---

## Try it

```try
(try (app "repl")
  (task "Open term from the dock and press Enter for REPL. Type `(* 12 12)` and press Enter, then come back here.")
  (check (number (ask "What did repl answer?") (answer 144) (from "12 * 12")
    (hint "12 times 12."))))
```

```check
(number
  (ask "What is the value of `(+ 100 20 3)`?")
  (answer 123) (from "100 + 20 + 3")
  (hint "Add all three arguments."))
```

---

## Practice

```check
(number
  (ask "What is the value of `(* 2 3 4)`?")
  (answer 24) (from "2 * 3 * 4")
  (wrong-if 9 "That adds them. The procedure is `*`, so they are multiplied: 2 times 3 times 4.")
  (hint "Multiply all three arguments."))
```

```check
(choice
  (ask "Which expression works out 7 take away 2?")
  (right "`(- 7 2)`")
  (wrong "`(7 - 2)`"
    "In Scheme the procedure comes first inside the brackets. (7 - 2) asks Scheme to use 7 as a procedure.")
  (wrong "`(- 2 7)`"
    "The arguments keep their order: (- 2 7) is 2 take away 7, which is -5.")
  (hint "Procedure first, then the arguments in order."))
```

So: an expression is a procedure and its arguments in brackets, and
repl prints its value.
