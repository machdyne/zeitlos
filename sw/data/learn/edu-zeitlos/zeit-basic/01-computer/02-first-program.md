# Your first program

```lesson
(objective "Type a program, run it, list it, and change, add or delete its lines.")
(minutes 20)
```

In this lesson you write a program of several lines, look at it, and
change it.

Type these three lines into BASIC, pressing Enter after each. After
each one BASIC stores the line and shows `READY.` again.

```basic
10 PRINT "MY FIRST PROGRAM"
20 PRINT "BY A NEW PROGRAMMER"
30 PRINT "THE END"
```

Then type `RUN`. BASIC runs the lines in order of their line numbers
and prints three lines.

```try
(try (app "basic")
  (task "Open BASIC, type the three lines above and then RUN. Come back when you have seen what it prints.")
  (check (number
    (ask "How many lines did the program print?")
    (answer 3)
    (hint "Each PRINT statement prints one line. How many PRINT statements are there?"))))
```

---

## Looking at the program

`LIST` is the command that shows the program BASIC has
stored, in order of line number.

The order you type lines in does not matter. If you type line 30 first,
then 10, then 20, `LIST` still shows 10, 20, 30, and `RUN` runs them in
that order.

`LIST` also writes every line in one tidy way: capitals for BASIC's own
names, single spaces. Text inside quotes is kept exactly as you typed
it.

```check
(choice
  (ask "You type 30 PRINT \"C\", then 10 PRINT \"A\", then 20 PRINT \"B\", then RUN. What is printed, from top to bottom?")
  (right "A, B, C")
  (wrong "C, A, B"
    "That is the order you typed them in. BASIC keeps the lines in order of their numbers, and runs them that way.")
  (wrong "Only C"
    "Every numbered line is stored, not only the first. RUN runs all of them.")
  (hint "Which order does BASIC keep its lines in?"))
```

---

## Changing a line

To change a line, type it again with the same number. The new line
replaces the old one.

To delete a line, type its number alone and press Enter.

To add a line between two others, give it a number between theirs.
That is why programs are numbered in tens: 10, 20, 30 leaves room for
15 or 25 later.

```check
(choice
  (ask "Your program has lines 10, 20 and 30. You want a new line to run between 10 and 20. Which number can it have?")
  (right "15")
  (wrong "11 would not work, so there is no way"
    "11 would work too: any number between 10 and 20 puts the line between them. Numbering in tens just leaves plenty of choice.")
  (wrong "25"
    "25 comes after 20, so the line would run between 20 and 30.")
  (wrong "You must renumber every line"
    "There is no need: any number between 10 and 20 goes in between. That is why lines are numbered in tens.")
  (hint "Lines run in order of their numbers. Which number is between 10 and 20?"))
```

---

```check
(choice
  (ask "Your program has a line 20. You type 20 alone and press Enter. What happens?")
  (right "Line 20 is deleted")
  (wrong "Line 20 runs"
    "Lines run only with RUN. A number alone, with nothing after it, deletes the line with that number.")
  (wrong "SYNTAX ERROR"
    "A line number alone is allowed: it is how you delete a line.")
  (hint "There is nothing after the number. What could that replace the line with?"))
```

---

## Starting again

`NEW` is the command that deletes the whole program, so you
can start a new one. Be careful: a program that is not saved is gone
after `NEW`. You learn to save programs later in this unit.

```example
(example
  (problem "Your program is 10 PRINT \"RED\", 20 PRINT \"GREEN\", 30 PRINT \"BLUE\". You type 20 PRINT \"YELLOW\", then 30, then RUN. What does it print?")
  (step "20 PRINT \"YELLOW\" replaces line 20.")
  (step "30 alone deletes line 30.")
  (step "The program is now 10 PRINT \"RED\" and 20 PRINT \"YELLOW\".")
  (answer "RED, then YELLOW."))
```

```check
(number
  (ask "Your program has lines 10, 20, 30 and 40, each a PRINT. You type 20, then 25 PRINT \"X\", then 30 PRINT \"Y\". How many lines does the program have now?")
  (answer 4)
  (explain "20 deletes a line, 25 adds one, and 30 replaces one: 10, 25, 30 and 40.")
  (wrong-if 5 "Typing 30 again does not add a line: it replaces the line 30 that is there.")
  (wrong-if 3 "25 is a new number, so it adds a line, while 20 alone deleted one.")
  (hint "Go through them one at a time: one deletes, one adds, one replaces."))
```

---

```check
(choice
  (ask "You typed lines 10 to 50 an hour ago and do not remember them all. Which command shows them?")
  (right "LIST")
  (wrong "RUN"
    "RUN carries the program out, which shows what it does, not the lines themselves.")
  (wrong "NEW"
    "NEW deletes the whole program. LIST shows it.")
  (hint "Which command shows the program's lines?"))
```

So: `RUN` runs the program, `LIST` shows it, a line typed again
replaces the old one, a number alone deletes it, and `NEW` starts
afresh.
