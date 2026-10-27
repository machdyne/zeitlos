# C and its compiler

```lesson
(objective "Say what a compiler does, and how running a C program differs from running a BASIC one.")
(minutes 15)
(keywords "C (the language)" "source file" "machine code" "compiler" "compile (a program)" "floating point")
```

This course teaches **C**, a programming language made by Dennis
Ritchie at Bell Labs around 1972, to write the Unix operating system.

C was designed to be close to the machine: its programs run nearly as
fast as the processor's own instructions allow, and it lets a program
reach any part of memory. It is still a leading language of operating
systems and of small computers.

Zeitlos itself is written in C: the system at its core, its window
manager, and the BASIC you may have used.

```sources
(source "Timeless Computing (docs/tc.md), C")
```

```check
(choice
  (ask "Why is C still used for operating systems?")
  (right "Its programs are close to the machine, so they are fast and can reach the hardware")
  (wrong "It is the newest language"
    "C is from around 1972, one of the oldest languages in use. It is used because it is close to the machine, not because it is new.")
  (wrong "It checks every mistake before it can do harm"
    "The opposite: C checks little. It does what the program says, even when that is a mistake. Its speed and its reach are why it is used.")
  (hint "Think of what an operating system has to do: run fast, and drive the hardware directly."))
```

---

## Source and machine code

The processor runs **machine code**: instructions stored as numbers,
which people seldom write by hand.

You write a C program as text, in a **source file** whose name ends in
`.c`. A **compiler** is a program that reads a source file and writes
out the same program in machine code. Doing that is to **compile** the
program. The result is a new file: a program that the computer can run
directly.

BASIC works differently. When you type RUN, BASIC reads your program a
line at a time and carries out each line as it goes. Nothing is made
in advance, so a change is ready to run at once.

```check
(choice
  (ask "You compiled first.c into a program called first, then changed a line of first.c. What does first do when you run it now?")
  (right "What it did before: the change is not in it until you compile again")
  (wrong "The new version: the computer reads first.c as the program runs"
    "That is how BASIC works. A compiled program is machine code made from the source when you compiled it. It never looks at first.c again.")
  (wrong "Nothing: changing the source file breaks the program"
    "The program is a separate file, made at compile time. Changing the source does not touch it; it simply goes on doing what it did.")
  (hint "Which file does the computer run: first.c, or first?")
  (hint "first was made from the old first.c. Nothing has made it again since."))
```

---

## zcc, the compiler on Zeitlos

Most C compilers are large programs that run on large computers, and
programs for a small computer are usually compiled on a bigger one.
Zeitlos has a compiler that runs on the board itself: **zcc**. You
write, compile and run without any other computer.

zcc writes machine code straight from your source, in one pass, with
no other tool after it. Into every program it makes, zcc copies
**libz**, a small collection of ready-made program parts (such as
`printf`, which prints), about 70 KB. So even the smallest program is
a file of over 70 KB.

```sources
(source "Timeless Computing (docs/tc.md), C: Compiling")
(source "docs/zcc.md, Why there is no assembler and no linker; The runtime")
(source "docs/libz.md, What is in it, and what is not")
```

```check
(choice
  (ask "Where does zcc run?")
  (right "On the Zeitlos board itself")
  (wrong "Only on a larger computer, which sends the program to the board"
    "That is cross-compiling, the usual way for small computers. zcc runs on the board itself, so no other computer is needed.")
  (wrong "Inside BASIC"
    "zcc is a program of its own, written in C. BASIC is a different language with its own way of running programs.")
  (hint "The point of zcc is that you need no other computer."))
```

---

## The loop

Every C program you write here goes round the same loop:

1. **Write** the source file in `text`, and save it.
2. **Compile** it with `zcc` in `term`.
3. **Run** the program it made.
4. **Read** what happened, change the source, and go round again.

```figure
(zc-box 10 40 80 34 "text")
(zc-box 120 40 80 34 "zcc")
(zc-box 230 40 80 34 "run")
(arrow 90 57 118 57 'ink 1.5)
(arrow 200 57 228 57 'ink 1.5)
(text 104 30 "first.c" 'centre 'small)
(text 214 30 "first" 'centre 'small)
(path '(270 74 270 130 50 130 50 76) 'ink 1.5)
(arrow 50 90 50 76 'ink 1.5)
(text 160 150 "read what it did; change the source" 'centre 'small)
(caption "write, compile, run, change")
```

```check
(choice
  (ask "zcc reports a mistake on line 7 of your source file. What do you change?")
  (right "Line 7 of the source file, in text; then compile again")
  (wrong "The program file zcc made"
    "When zcc reports a mistake it makes no program at all. And a program file is machine code: you always change the source and compile again.")
  (wrong "Nothing: run the program anyway"
    "With a mistake in the source there is no new program to run. Fix the source first.")
  (hint "Which file in the loop do you ever edit by hand?"))
```

---

## What zcc does not do

zcc takes most of C, but not all of it. Two gaps matter from the
start:

- **No fractions.** C keeps numbers with a fractional part in
  **floating point**, as `float` and `double`. This processor has no
  instructions for them, so zcc refuses floating-point arithmetic.
  Whole numbers only.
- **No extra-large whole numbers.** `long long`, C's type for them, is
  refused too. zcc's whole numbers go up to about 2 billion, or about
  4 billion without a minus sign, as the next unit shows.

When zcc meets something it does not take, it says so, with the line
of your file, rather than making a wrong program.

```sources
(source "docs/zcc.md, The supported subset")
```

```check
(choice
  (ask "You want a program that works out an average to the hundredth, such as 12.50. What does zcc let you do?")
  (right "Work in whole numbers, for example in hundredths, and print the point yourself")
  (wrong "Use float, which zcc turns into whole numbers for you"
    "zcc does not convert float. It refuses floating-point arithmetic with an error, so you choose whole-number units yourself.")
  (wrong "Use long long for the fractions"
    "long long is for extra-large whole numbers, and zcc refuses it as well. Neither gives you fractions.")
  (hint "zcc has whole numbers only. How do you write 12.50 as a whole number?")
  (hint "1250 hundredths is 12.50."))
```

So: a compiler turns your source file into machine code once, and zcc
does that on the board itself.
