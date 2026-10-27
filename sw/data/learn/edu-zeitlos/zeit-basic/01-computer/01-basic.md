# A computer in a window

```lesson
(objective "Open BASIC, and tell a program line from a command.")
(minutes 15)
(keywords "program (for a computer)" "programming language (BASIC)" "BASIC (the language)" "cursor (in BASIC)" "line number (in BASIC)" "command (in BASIC)" "statement (in BASIC)" "syntax error")
```

In this lesson you open BASIC and learn the two kinds of line you can
type into it.

A **program** is a list of exact instructions that a
computer carries out, one after another. A computer does exactly what
it is told and nothing else, so every instruction must be exact.

A **programming language** is a precise way of writing those
instructions. **BASIC** is one of the oldest. It was
made in 1964 at Dartmouth College by John Kemeny and Thomas Kurtz, so
that students of any subject could program a computer. In the 1980s
almost every home computer started with BASIC.

```sources
(source "Timeless Computing (docs/tc.md), BASIC; Thinking in Programs")
```

```check
(choice
  (ask "Why must the instructions in a program be exact?")
  (right "The computer does exactly what it is told, and cannot guess what you meant")
  (wrong "Computers are slow, so short instructions save time"
    "Speed is not the reason. A computer has no way to guess: an instruction that is nearly right is carried out as it is, and gives a wrong result.")
  (wrong "Exact instructions use less electricity"
    "The reason is not power. A computer follows instructions literally, so they must say exactly what you mean.")
  (hint "Think of a cook who follows a recipe exactly and never guesses."))
```

---

## The BASIC computer

Zeitlos comes with **BASIC**, a small computer of its own that runs in
a window, the way home computers worked. Its screen is 320 dots wide
and 240 high, with room for 40 characters across and 30 rows down.

```try
(try (app "basic")
  (task "Open BASIC from the dock: it is in the Tools group. Look at what it shows, then come back.")
  (check (text
    (ask "BASIC shows its name, then one more line. What does that line say?")
    (accept "READY") (accept "READY.")
    (hint "It is one short line in capitals, ending with a full stop.")
    (hint "It tells you BASIC is waiting for you: R... ."))))
```

```sources
(source "docs/basic_app.md, the start of the page")
(source "sw/data/wm.cfg: basic is in the Tools group")
```

---

## Waiting for you

`READY.` means BASIC is waiting for a line. The **cursor**
marks where the next character you type appears. You type a line, then
press **Enter**, and BASIC deals with the whole line at once.

BASIC writes in capitals, but you can type in small letters too:
`print` and `PRINT` mean the same.

```check
(choice
  (ask "You type a line in BASIC. When does BASIC read it?")
  (right "When you press Enter")
  (wrong "After each key you press"
    "BASIC waits for the whole line. Until you press Enter you can still change it.")
  (wrong "When you type READY."
    "READY. is what BASIC shows when it is waiting. You end a line with Enter.")
  (hint "Which key ends a line?"))
```

---

## Two kinds of line

A line that starts with a number is part of a program. The number is
its **line number**. BASIC does not carry the line out: it
**stores** it, to run later.

```basic
10 PRINT "HELLO"
```

A line without a number is a **command**, and BASIC carries
it out at once. `RUN` is the command that runs the program BASIC has stored, from
its lowest line number up. So `RUN` here shows `HELLO`.

`PRINT "HELLO"` is a **statement**: one instruction in a
program line. `PRINT` shows what follows it on the screen.

```check
(choice
  (ask "You type 10 PRINT \"HI\" and press Enter. What happens?")
  (right "BASIC stores the line; nothing is printed yet")
  (wrong "HI appears on the screen at once"
    "The line starts with a number, so it is part of a program. BASIC keeps it until you type RUN.")
  (wrong "BASIC prints 10 HI"
    "10 is the line number, which BASIC uses to keep the line in order. It is not printed, and the line does not run until RUN.")
  (hint "Does the line start with a number?"))
```

---

## No numbers, no statements

Many BASICs let you type a statement without a line number and run it
at once. Machdyne BASIC 1, the BASIC in Zeitlos, does not: a statement
must go in a numbered line. Typed alone, `PRINT 2 + 2` gets the answer
`SYNTAX ERROR`. A **syntax error** is a line that is not written the
way the language needs, so BASIC cannot read it.

Commands such as `RUN` are the only lines without numbers.

```sources
(source "Timeless Computing (docs/tc.md), BASIC: Lines and Commands")
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 1")
```

```check
(choice
  (ask "You type PRINT 2 + 2, with no line number, and press Enter. What does BASIC show?")
  (right "SYNTAX ERROR")
  (wrong "4"
    "Some BASICs would print 4, but this one runs statements only from numbered lines. A statement typed alone is not something it can read.")
  (wrong "Nothing: it stores the line"
    "BASIC stores only lines that start with a number. This one has none, so BASIC tries to read it as a command, and it is not one.")
  (hint "Without a number, a line must be a command. Is PRINT a command?"))
```

---

```check
(text
  (ask "Which command runs the program that BASIC has stored?")
  (accept "RUN")
  (wrong-if "PRINT" "PRINT is a statement that goes inside a program line. The command that starts the program is another one.")
  (hint "It is three letters. The program ... ."))
```

---

```check
(choice
  (ask "Which of these lines does BASIC store as part of a program?")
  (right "20 PRINT \"BYE\"")
  (wrong "RUN"
    "RUN is a command: BASIC carries it out at once and does not store it.")
  (wrong "PRINT \"BYE\""
    "This line has no line number, so BASIC does not store it. It is not a command either, so it is a SYNTAX ERROR.")
  (hint "Look for the line that starts with a number."))
```

So: a line with a number is stored as part of the program, a line
without one is a command, and `RUN` runs the program.
