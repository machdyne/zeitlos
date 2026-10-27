# State and control flow

```lesson
(objective "Follow a short program line by line, saying what each variable holds after each step.")
(minutes 20)
(keywords "program state" "variable (in a program)" "assignment (to a variable)" "data type" "control flow" "loop (in a program)")
```

A program works by changing its **program state**: the values held in
memory. A **variable** is a named place that holds a value. An
**assignment** replaces the value a variable held before:

```text
count = 0
count = count + 1
```

The second line is not an equation, which would be false. It says:
work out `count + 1`, and store the result back in `count`.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: State")
```

```check
(number
  (ask "x = 5, then x = x * 2, then x = x - 3. What does x hold now?")
  (answer 7) (from "5 * 2 - 3")
  (wrong-if 5 "Each assignment replaces the value. After x = x * 2, x holds 10, not 5.")
  (wrong-if 10 "That is after the second line. The third takes 3 away from that.")
  (hint "Go line by line, writing down what x holds after each."))
```

---

## Data types

Values have **data types**: whole numbers, fractional numbers,
characters, text, true-or-false values and others. The type says what a
value means and what can be done with it.

The same bits can mean different things: the byte 01000001 is the
number 65, or, read as a character, the letter A. A type is how a
language records which meaning applies.

```check
(choice
  (ask "A program adds 1 to a value whose type is text. What is the trouble?")
  (right "Adding a number to text has no clear meaning")
  (wrong "There is none: every value is bits, so it can always add"
    "The bits can always be added, but the result means nothing as text. Types are there to say which operations make sense.")
  (hint "What would \"hello\" + 1 mean?"))
```

---

## Control flow

Programs are built from three kinds of structure, their **control
flow**:

- **sequence**: one step, then the next;
- **choice**: if a condition is true, one step, otherwise another;
- **repetition**: a step repeated a number of times, or until a
  condition changes. A part of a program that repeats is a **loop**.

Every algorithm, however large, can be written with these three.
Euclid's uses all of them: a sequence of steps, repeated while B is not
zero, and that test is a choice.

```check
(choice
  (ask "\"If the tank is full, stop the pump; otherwise keep it running.\" Which structure is this?")
  (right "Choice")
  (wrong "Repetition"
    "Nothing is done again here: one of two steps is picked, depending on a condition. That is a choice.")
  (wrong "Sequence"
    "A sequence does every step in turn. Here only one of the two steps happens.")
  (hint "Do both steps happen, or only one?"))
```

---

## Following a program

Move the slider to run the program one step at a time. The mark shows
the line just done; on the right is the program state after it.

```figure
(define prog '("total = 0" "n = 1" "while n <= 3:" "    total = total + n" "    n = n + 1" "print total"))
(define trace '((1 0 -1) (2 0 1) (3 0 1) (4 1 1) (5 1 2) (3 1 2) (4 3 2) (5 3 3)
                (3 3 3) (4 6 3) (5 6 4) (3 6 4) (6 6 4)))
(define s (slider 's 0 12 0 "Step" 1))
(define now (list-ref trace s))
(for-range 0 6
  (lambda (i)
    (text 20 (+ 40 (* i 22)) (fmt (+ i 1) 0) 'small 'right)
    (text 30 (+ 40 (* i 22)) (list-ref prog i) 'small)))
(arrow 2 (+ 35 (* 22 (- (car now) 1))) 12 (+ 35 (* 22 (- (car now) 1))) 'ink 2)
(frame 210 24 100 90 'ink 1)
(text 220 50 (string-append "total = " (fmt (cadr now) 0)) 'body)
(text 220 80 (string-append "n = " (if (< (caddr now) 0) "?" (fmt (caddr now) 0))) 'body)
(if (= s 12) (text 220 104 "prints 6" 'small))
(caption (string-append "step " (fmt (+ s 1) 0) " of 13: line " (fmt (car now) 0)
  (cond ((and (= (car now) 3) (< (caddr now) 4)) ", n <= 3 is true")
        ((= (car now) 3) ", n <= 3 is false: skip to line 6")
        (else ""))))
```

```check
(number
  (ask "Step through the program. After line 4 runs for the second time, what does total hold?")
  (answer 3) (from "0 + 1 + 2")
  (wrong-if 2 "That is n's value then. total was 1, and line 4 adds n, which is 2.")
  (wrong-if 1 "That is after the first time. The second time adds n = 2 to it.")
  (hint "The first time, total becomes 0 + 1. The second time, n is 2."))
```

---

```check
(number
  (ask "Change line 3 to \"while n <= 5:\". What does the program print?")
  (answer 15) (from "1 + 2 + 3 + 4 + 5")
  (wrong-if 10 "That stops at n = 4. With n <= 5 the loop runs once more, adding 5.")
  (wrong-if 5 "That is the last n added, not the total. total adds up every n from 1 to 5.")
  (hint "The loop adds n to total for every n from 1 while n <= 5."))
```

---

```check
(choice
  (ask "A program prints the wrong total. What is the most useful question to ask first?")
  (right "At which step did a variable first hold a value I did not expect?")
  (wrong "Which line is the longest?"
    "Length says nothing about mistakes. Every step reads some state and writes new state: find where the state first goes wrong.")
  (wrong "Is the computer broken?"
    "Almost never. The computer did exactly what the program said; the program's state went somewhere you did not expect.")
  (hint "Every step reads state and writes new state."))
```

So: a program is a sequence of steps, with choices and loops, that read
and change its state, held in variables.
