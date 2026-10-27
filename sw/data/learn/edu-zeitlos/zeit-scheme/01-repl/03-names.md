# Names for values

```lesson
(objective "Give values names with define, change them with set!, and read what repl says about a name it does not know.")
(minutes 15)
(keywords "variable (in Scheme)" "serial console")
```

`define` gives a value a name, so you can use it again:

```
> (define price 12)
price
> price
12
> (* price 3)
36
```

A name that stands for a value is a **variable**. repl answers a
`define` with the name it has just defined. After that, the variable
can go anywhere its value could: `(* price 3)` is `(* 12 3)`.

```sources
(source "Zeitlos docs: repl.md, Scheme in ten minutes: Names")
```

```check
(number
  (ask "You type `(define width 8)`, then `(* width width)`. What does repl answer to the second?")
  (answer 64) (from "8 * 8")
  (wrong-if 16 "That is width plus width. The procedure is *, so it is 8 times 8.")
  (hint "Put 8 in place of each width."))
```

---

## A value, worked out once

The value given to `define` can be any expression. It is worked out
when the `define` is, and the variable keeps the answer:

```
> (define area (* 4 5))
area
> area
20
```

Choose names that say what they hold, in lower case, with `-`
between words: `apples-per-box`, `area`. Capitals count: `Area`
and `area` are two different variables.

```check
(number
  (ask "You type `(define a 3)` and then `(define b (* a a))`. What is the value of b?")
  (answer 9) (from "3 * 3")
  (wrong-if 6 "That is a plus a. b is a times a, with a as 3.")
  (hint "Work out (* a a) with a as 3."))
```

---

## Changing a variable

`set!` gives an existing variable a new value. repl answers with that
value:

```
> (define score 10)
score
> (set! score (+ score 5))
15
```

`(+ score 5)` is worked out with the old value, 10, and the answer
becomes the new value of score. The `!` in its name is a warning sign:
it changes something.

```sources
(source "Timeless Computing (docs/tc.md), Three Languages Side by Side: Common Constructs")
```

```check
(number
  (ask "After `(define n 4)` and then `(set! n (* n 3))`, what is the value of n?")
  (answer 12) (from "4 * 3")
  (wrong-if 4 "set! changes n: its new value is (* n 3), worked out with n as 4.")
  (wrong-if 3 "The new value is the whole expression (* n 3), not the 3 alone.")
  (hint "Work out (* n 3) with the old value of n."))
```

---

## A name repl does not know

If you use a name that has no value -- a typing mistake, or a
variable not yet defined -- repl answers `#f`:

```
> prise
#f
```

The computer writes its own messages to its **serial console**. When
a mistake stops a calculation, repl says
`repl: Scheme error (see the serial console for details)`, and puts
the reason there. To read it, open another `term` window and press
CONSOLE: for a misspelled name, its line there ends with the word
unbound and the name: Scheme's way of saying the name has no value.

```sources
(source "Zeitlos docs: console.md; scheme.md, What's reachable right now")
(source "Zeitlos source: sw/ext/ms/ms.c, ms_eval(), unbound symbol")
```

```check
(choice
  (ask "You type `totl` at the prompt, meaning total, which holds 50. What does repl answer?")
  (right "#f")
  (wrong "50"
    "repl does not guess what you meant. totl is a different name, and it has no value.")
  (wrong "0"
    "A name with no value has no number at all. repl answers #f, and the serial console says why.")
  (hint "Is totl the same name as total?"))
```

---

## Try it

```try
(try (app "repl")
  (task "In repl, type (define side 7), then (* side side side), then come back here.")
  (check (number (ask "What did repl answer to the second?") (answer 343) (from "7 * 7 * 7")
    (hint "7 times 7 times 7."))))
```

```check
(choice
  (ask "What does repl answer to `(define side 7)`?")
  (right "side")
  (wrong "7"
    "define answers with the name it has defined. Type side on its own to see its value, 7.")
  (wrong "Nothing at all"
    "repl prints the value of every expression you give it. For a define, that is the name.")
  (hint "Look back at what repl said after (define price 12)."))
```

---

## Practice

```example
(example
  (problem "After (define x 2), you type (set! x (+ x x)) twice. What is x?")
  (step "The first set! works out (+ x x) with x as 2: 4.")
  (step (number (ask "The second works it out with x as 4. What is x now?") (answer 8) (from "4 + 4")
    (wrong-if 6 "Each set! uses the value x has at that moment, and the first has made it 4.")))
  (answer "8"))
```

```check
(choice
  (ask "You have defined `total` as 50. Which of these changes it to 60?")
  (right "`(set! total (+ total 10))`")
  (wrong "`(+ total 10)`"
    "That works out 60 and repl prints it, but total stays 50. Only set! (or another define) changes it.")
  (wrong "`(set! (+ total 10))`"
    "set! needs the variable's name first, then its new value.")
  (hint "Which one names the variable that changes?"))
```

So: `define` names a value, `set!` changes it, and a name with no
value gives `#f`.
