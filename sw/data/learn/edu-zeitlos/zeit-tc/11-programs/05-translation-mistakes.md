# Compilers, interpreters and bugs

```lesson
(objective "Say how compilers and interpreters differ, name the three kinds of bug, and follow the steps of debugging.")
(minutes 20)
(keywords "interpreter" "bytecode" "bug (in a program)" "syntax error" "run-time error" "logic error" "debugging")
```

A processor runs only machine code, so a program in any other language
must be translated. There are two ways.

A compiler translates the whole program into machine code before it
runs. The result runs at full speed, and many mistakes are reported
before the program runs at all. The cost is a separate step between
writing and running. C is normally compiled; on Zeitlos, by `zcc`
([zcc](docs:zcc)).

An **interpreter** reads the program and carries out each part
directly, as it goes. It is slower, often ten times or more. In
exchange, you can type a line and see what it does at once, change the
program while it runs, and work on a machine with no compiler. BASIC
and Scheme are usually interpreted.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Compilers and Interpreters")
```

```check
(choice
  (ask "You want to try out ideas one line at a time and see each result at once. Which suits you?")
  (right "An interpreter")
  (wrong "A compiler"
    "A compiler translates the whole program first, then it runs. Trying one line at a time is what an interpreter is good at.")
  (hint "Which one carries out each part as it reads it?"))
```

---

## In between

Many systems lie between the two. Some interpreters first translate the
program into a compact internal form, called **bytecode** or tokens, and
interpret that, which is faster than working from the text. Machdyne
BASIC checks each line and turns it into tokens when you type it in.

```check
(choice
  (ask "A program must run as fast as the processor allows, on a machine that will not change it. Which is the usual choice?")
  (right "Compile it")
  (wrong "Interpret it"
    "An interpreter is often ten times slower or more. For full speed, the program is compiled into machine code first.")
  (hint "Which runs at full speed?"))
```

---

## Bugs

Almost every program has mistakes, **bugs**, when first written. They
are of three kinds:

- **Syntax errors**: the program is not valid in its language, as a
  sentence with its words in the wrong order is not. The compiler or
  interpreter reports these.
- **Run-time errors**: the program is valid but tries something
  impossible as it runs, such as dividing by zero or reading past the
  end of an array.
- **Logic errors**: the program runs without complaint and gives the
  wrong answer. These are the hardest to find, because nothing reports
  them.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Mistakes")
```

```check
(choice
  (ask "A program meant to average 3 numbers adds them and divides by 2. It runs and prints a number. Which kind of bug is this?")
  (right "A logic error")
  (wrong "A syntax error"
    "The program is valid, or it would not have run. It does what it says, but what it says is wrong: a logic error.")
  (wrong "A run-time error"
    "Nothing impossible happened: dividing by 2 is fine. The answer is simply wrong, and nothing reports it.")
  (hint "Did anything complain?"))
```

---

```check
(choice
  (ask "A program reads item number 10 of an array of 10 items, numbered from 0, and stops with an error. Which kind of bug?")
  (right "A run-time error")
  (wrong "A logic error"
    "A logic error gives a wrong answer silently. Here the program tried something impossible, reading past the end, and was stopped: a run-time error.")
  (wrong "A syntax error"
    "The program was valid enough to start running. The mistake showed only when it ran.")
  (hint "Items are numbered 0 to 9. When did the trouble show?"))
```

---

## Debugging

Finding bugs is **debugging**. The method matters more than the tools:

1. **Reproduce** the problem reliably. A bug you cannot make happen on
   demand cannot be shown to be fixed.
2. **Observe** instead of guessing. Print the values of variables at
   key points, or step through the program, and compare them with what
   you expected.
3. **Narrow the search.** Find the earliest point at which the state is
   wrong. Halving the search each time, as in binary search, finds it
   quickly.
4. **Fix the cause**, not the symptom. Then check that the problem is
   gone and nothing else has broken.

```check
(choice
  (ask "A program's total is wrong at the end. It is right at line 100 and wrong at line 400. Where do you look next?")
  (right "Print the total near line 250, to halve the search")
  (wrong "Change line 400 until the total comes out right"
    "That fixes the symptom, not the cause. The total went wrong somewhere between 100 and 400; find where first.")
  (wrong "Read all 300 lines carefully from line 100"
    "That can work, but halving finds the spot in a few looks: right at 250 means the bug is after it, wrong means before.")
  (hint "The bug is between 100 and 400. What does one look in the middle tell you?"))
```

---

```check
(choice
  (ask "A bug happens only sometimes. What is the first thing to do?")
  (right "Find a way to make it happen every time")
  (wrong "Change something and see if it goes away"
    "If it only happens sometimes, it going away proves nothing. First make it happen on demand, so you can tell when it is fixed.")
  (hint "How would you know your fix worked?"))
```

So: a compiler translates first and an interpreter as it goes; bugs are
syntax, run-time or logic errors, found by reproducing, observing and
narrowing.
