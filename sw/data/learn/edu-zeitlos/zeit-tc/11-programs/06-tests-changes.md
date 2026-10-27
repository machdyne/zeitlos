# Tests, changes and three languages

```lesson
(objective "Choose good tests, say why every change is recorded, and pick a language to learn next.")
(minutes 15)
(keywords "program test" "regression" "diff" "version control system" "Git")
```

A **program test** runs part of a program with known inputs and checks
that it gives the expected outputs. A test of Euclid's algorithm checks
that the greatest common divisor of 1071 and 462 is 21, and of 7 and 0
is 7.

Good tests go where mistakes are likely: the smallest and largest
values, zero, empty input, and the values on each side of every limit.
A function that works for 10 numbers may fail for none, for one, or for
the largest number its type can hold.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Testing")
```

```check
(choice
  (ask "A function finds the largest number in a list. Which test is most likely to find a mistake?")
  (right "A list with no numbers in it")
  (wrong "A list of 10 ordinary numbers"
    "That is the case the author had in mind, so it probably works. Mistakes hide at the edges: no numbers, one number, the largest.")
  (wrong "A list of 11 ordinary numbers"
    "One more ordinary number tests nothing new. Try the edges, where the author may not have thought.")
  (hint "Where do mistakes hide: in the usual case, or at the edges?"))
```

---

## Keep the tests

Tests are kept and run again after every change. A bug that was fixed
and comes back is a **regression**, and a test that once found a bug
guards against it.

Hardware is tested the same way: a memory test writes known patterns to
every place and reads them back.

```check
(choice
  (ask "You fixed a bug last month. Today's change brings it back. What would have caught it at once?")
  (right "A kept test for that bug, run after every change")
  (wrong "Reading the new change carefully"
    "Reading helps, but the same mistake slipped past once already. A kept test checks every time, without forgetting.")
  (hint "What guards against a bug coming back?"))
```

---

## Writing for people

A program is read far more often than it is written, often by its own
author months later. Clear names, short functions, a consistent layout
and brief comments that explain **why** something is done, not what,
make it much easier to keep working.

```check
(choice
  (ask "Which comment helps a reader most?")
  (right "# the sensor reads 2 degrees high, so take 2 off")
  (wrong "# take 2 off t"
    "The line itself already says that. A comment earns its place by saying why: here, that the sensor reads high.")
  (hint "Which one says something the code itself cannot?"))
```

---

## Keeping track of changes

Without a record of changes, nobody can say what changed between a
version that worked and one that does not, or go back.

The simplest record is a dated copy of the whole project before each
big change, with a note of what changed and why. A **diff** program
compares two versions of a text file and lists only the lines that
differ. Zeitlos has one: the command `diff` ([posix](docs:posix)).

A **version control system** keeps this record for you: each change is
stored with a description, the date and the author, and every earlier
version is kept. **Git** is widely used; it keeps the whole history in
the project's own folder, so a copy of the folder is a copy of the
history, and no server is needed.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Keeping Track of Changes")
```

```check
(choice
  (ask "Yesterday's version of your program worked; today's does not. What does version control let you do?")
  (right "See exactly which lines changed, and go back to yesterday's version")
  (wrong "Nothing more than saving the file does"
    "Saving keeps only the latest version. Version control keeps every earlier one, with what changed, when and why.")
  (hint "What does it keep besides the latest version?"))
```

---

## Three languages

The book's next chapters show these ideas in three languages, each a
different way of thinking about programs. All three run on Zeitlos.

| Language | Born | Style | Best for |
|---|---|---|---|
| BASIC | 1964 | numbered lines | first programs |
| Scheme | 1975 | expressions and functions | ideas, structure, scripting |
| C | 1972 | near the machine | speed, hardware, systems |

Each has a course of its own, to take in any order:
[Programming in BASIC](course:zeit-basic),
[Programming in Scheme](course:zeit-scheme) and
[C on Zeitlos](course:zeit-c).

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Three Languages")
```

```check
(choice
  (ask "You want to write a program that drives hardware as fast as the processor can. Which language suits it best?")
  (right "C")
  (wrong "BASIC"
    "BASIC is made for first programs, and is usually interpreted, so slower. C is near the machine and compiled: made for speed and hardware.")
  (wrong "Scheme"
    "Scheme is best for ideas and structure. For speed and hardware, C, compiled and near the machine, is the usual choice.")
  (hint "Look at the column Best for."))
```

So: test at the edges and keep the tests, record every change, and
choose a language for what you want to do.
