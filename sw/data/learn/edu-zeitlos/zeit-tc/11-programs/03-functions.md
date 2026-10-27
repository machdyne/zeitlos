# Functions and recursion

```lesson
(objective "Say what a function is and why programs are divided into them, and follow a recursive function to its base case.")
(minutes 20)
(keywords "function (in a program)" "argument (of a function)" "recursion" "base case")
```

A **function**, also called a procedure or subroutine, is a named part
of a program that takes some inputs, its **arguments**, does a task,
and usually returns a result.

Once written, a function can be used without thinking about how it
works. It is the main way a large problem is divided into small ones.

```text
function square(x):
    return x * x

area = square(7)
```

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Functions")
```

```check
(number
  (ask "Using the function above, what is square(square(2))?")
  (answer 16) (from "(2 * 2) * (2 * 2)")
  (wrong-if 4 "That is square(2). The outer square then squares that result.")
  (wrong-if 8 "That doubles 4. square multiplies its argument by itself: 4 times 4.")
  (hint "Work from the inside: square(2) first."))
```

---

## A good function

A good function does one clear thing, and its name says what. A program
for a weather station might have functions to:

1. read the sensor;
2. convert the reading to degrees;
3. decide whether it is freezing;
4. write a line to the log.

Each is short and can be checked on its own, and the main program reads
almost like a description of the task.

```check
(choice
  (ask "Which is the better function for that weather station?")
  (right "is_freezing(degrees): true if degrees is below 0")
  (wrong "do_everything(): reads, converts, decides, logs and prints"
    "One function that does everything cannot be checked a piece at a time, and its name says nothing. Small functions with clear names can.")
  (wrong "f2(a): a short function with no clear purpose"
    "Short is good, but the name must say what it does. Someone reading f2(a) has to read its insides to know.")
  (hint "Which one does one clear thing, with a name that says what?"))
```

---

## Recursion

A function can call itself. This is **recursion**, and it suits
problems that contain smaller copies of themselves.

The **factorial** of a number n, written n!, is n times the factorial
of n - 1; and the factorial of 0 is 1. So 3! = 3 * 2 * 1 = 6. A
recursive function says exactly that:

```text
function factorial(n):
    if n = 0:
        return 1
    return n * factorial(n - 1)
```

Recursion needs a **base case**, here n = 0, at which it stops calling
itself.

```check
(number
  (ask "What does factorial(4) return?")
  (answer 24) (from "4 * 3 * 2 * 1")
  (wrong-if 10 "That adds 4 + 3 + 2 + 1. Factorial multiplies.")
  (wrong-if 0 "factorial(0) returns 1, not 0, so the product never becomes 0.")
  (hint "factorial(4) = 4 * factorial(3), and factorial(3) = 3 * factorial(2) ...")
  (hint "4 * 3 * 2 * 1 * 1."))
```

---

```example
(example
  (problem "Follow factorial(3) to its answer.")
  (step "factorial(3) needs 3 * factorial(2), so it calls factorial(2) and waits.")
  (step "factorial(2) needs 2 * factorial(1); factorial(1) needs 1 * factorial(0).")
  (step "factorial(0) is the base case: it returns 1 without calling anything.")
  (step "Now each waiting call finishes: factorial(1) = 1, factorial(2) = 2, factorial(3) = 6.")
  (answer "factorial(3) is 6."))
```

---

## Without a base case

Each call that is waiting keeps its place on the stack. Without a base
case, the function calls itself again and again until the stack is used
up.

```check
(choice
  (ask "Someone deletes the lines \"if n = 0: return 1\". What happens when factorial(3) is called?")
  (right "It calls itself for 2, 1, 0, -1, -2 ... until the stack runs out")
  (wrong "It returns 0, because nothing stops it at 0"
    "Nothing returns at all: with no base case every call makes another call, each waiting on the stack, until the stack is used up.")
  (wrong "It returns 6 as before"
    "The base case is what ends the calls. Without it, factorial(0) calls factorial(-1), and so on with no end.")
  (hint "Which line ever returns without calling factorial again?"))
```

---

```check
(choice
  (ask "A folder holds files and other folders, which hold more. Why does recursion suit counting all the files in it?")
  (right "Each folder inside is a smaller copy of the same problem")
  (wrong "Because recursion is always faster than a loop"
    "It is not always faster. It suits problems that contain smaller versions of themselves, as a folder of folders does.")
  (hint "What is counting the files of a folder inside it?"))
```

So: functions divide a program into small named parts, and a recursive
function solves a problem through smaller copies of itself, down to a
base case.
