# Mistakes and stopping

```lesson
(objective "Correct a line as you type it, read BASIC's error messages, and stop a running program.")
(minutes 20)
(keywords "bug (in a program)" "error message (in BASIC)" "run-time error" "logic error")
```

In this lesson you learn to put mistakes right and to stop a program.

While you type a line, before you press Enter:

| Key | Does |
|---|---|
| Backspace | erases the last character |
| Esc | erases the whole line |
| Up | brings back the last line you entered, to change and enter again |

```sources
(source "docs/basic_app.md, Using it")
```

```check
(choice
  (ask "You entered a long line, and BASIC said it had a mistake in it. What is the quickest way to correct it?")
  (right "Press Up to bring the line back, correct it, and press Enter")
  (wrong "Press Esc, then type the whole line again"
    "That works, but Esc throws the line away. Up brings back the line you entered, so you only change the part that was wrong.")
  (wrong "Type NEW and start the program again"
    "NEW deletes the whole program, not just the line with the mistake.")
  (hint "One key brings back the last line you entered."))
```

---

## Error messages

A mistake in a program is called a **bug**. When BASIC
cannot go on, it prints an **error message**: a short line
in capitals saying what went wrong.

`SYNTAX ERROR`, as you have seen, means BASIC cannot read a line. When
you enter a line with a word or sign BASIC does not know, it says so at
once and does not store the line. Some mistakes it finds only when the
line runs, such as a sum with a number missing: `10 PRINT 2 +`.

Other mistakes show only when the program runs. A mistake found while
the program runs gives a message that ends with `IN` and the number of
the line that went wrong:

```basic
10 PRINT "TEN DIVIDED BY ZERO IS"
20 PRINT 10 / 0
```

`RUN` prints the first line, then `DIV BY 0 IN 20`: nothing can be
divided by zero.

```check
(number
  (ask "A program stops with the message NO LINE IN 50. Which line should you look at first?")
  (answer 50)
  (hint "The number after IN is the line where it went wrong."))
```

---

## Three kinds of mistake

- A syntax error, as you know, is a line that is not written the way the language
  needs. BASIC reports it when you enter the line, or else when the
  line runs.
- A **run-time error** is a line that is written correctly but asks for
  something impossible when it runs, such as dividing by zero. BASIC
  reports it, with the line number.
- A **logic error** is a program that runs without complaint and gives
  the wrong answer. Nothing reports it: you find it by checking what
  the program does.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Mistakes")
```

```check
(choice
  (ask "A program should print the number of eggs in 2 boxes of 6, but prints 8. Its line is 10 PRINT 2 + 6. What kind of mistake is this?")
  (right "A logic error")
  (wrong "A syntax error"
    "The line is written correctly, so BASIC stored it. It just says add where it should say multiply.")
  (wrong "A run-time error"
    "The program ran to the end without an error message. It did what it was told, and was told the wrong thing.")
  (hint "Did BASIC print an error message?"))
```

---

## Stopping a program

Some programs run for a long time, or forever. To stop a running
program, press **Esc**. (Ctrl+C does the same.) BASIC stops at once and
prints `BREAK IN`, then the line it was on.

This program waits 60 seconds on line 20. `SLEEP` waits a number of
seconds:

```basic
10 PRINT "GOING TO SLEEP"
20 SLEEP 60
30 PRINT "AWAKE"
```

```try
(try (app "basic")
  (task "In BASIC, type NEW, then the three lines above, then RUN. Press Esc before the minute is up, and come back.")
  (check (text
    (ask "What did BASIC print when you pressed Esc?")
    (accept "BREAK IN 20")
    (wrong-if "AWAKE" "That is printed only if the program wakes up after 60 seconds. Run it again and press Esc sooner.")
    (hint "It is BREAK, IN, and the line number the program was on."))))
```

```sources
(source "docs/basic_app.md, Using it; docs/keys.md, Game mode")
```

---

```check
(choice
  (ask "What does Esc do in BASIC?")
  (right "It erases the line you are typing, or stops a program that is running")
  (wrong "It closes the BASIC window"
    "Esc belongs to BASIC: it never closes the window. It erases the line being typed, or stops a running program.")
  (wrong "It deletes the program"
    "NEW deletes the program. Esc erases only the line you are typing, or stops a running program.")
  (hint "It does one thing while you type and another while a program runs."))
```

---

```check
(choice
  (ask "You enter 10 PRNT \"HELLO\" (PRINT spelt wrong). What happens?")
  (right "BASIC says SYNTAX ERROR and does not store the line")
  (wrong "BASIC stores it and reports the mistake at RUN"
    "PRNT is not a word BASIC knows, so it cannot read the line at all: it says so as soon as you enter it, and does not store it.")
  (wrong "BASIC guesses you meant PRINT"
    "BASIC never guesses: it cannot read the line, so it says so.")
  (hint "Does BASIC know the word PRNT?"))
```

So: Backspace, Esc and Up correct a line, an error message names the
line that went wrong, and Esc stops a running program.
