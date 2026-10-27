# Procedures of your own

```lesson
(objective "Define a procedure with parameters and call it with arguments.")
(minutes 20)
(keywords "parameter (of a procedure)" "body (of a procedure)" "call (a procedure)")
```

You can make procedures of your own with `define`. Put the new
procedure's name and a name for its argument in brackets, then what
it works out:

```
> (define (half x) (/ x 2))
half
> (half 10)
5
```

x is a **parameter**: a variable that holds whatever argument the
procedure is given. `(/ x 2)` is its **body**, the expression it
works out. Writing `(half 10)` is a **call**: x becomes 10 and the
body's value, 5, is the answer.

```sources
(source "Zeitlos docs: repl.md, Scheme in ten minutes: Functions")
(source "Timeless Computing (docs/tc.md), Scheme: Definitions")
```

```check
(number
  (ask "After `(define (half x) (/ x 2))`, what is the value of `(half 7)`?")
  (answer 3.5) (from "7 / 2")
  (wrong-if 3 "/ keeps the fraction part, so half of 7 is 3.5.")
  (hint "x becomes 7. Work out the body, (/ x 2)."))
```

---

## Several parameters

A procedure can have several parameters. The arguments of a call go
to them in order:

```
> (define (area w h) (* w h))
area
> (area 3 4)
12
```

In `(area 3 4)`, w is 3 and h is 4. Give a procedure as many
arguments as it has parameters: in `(area 3)`, h has no value, and the
call stops with a Scheme error.

```check
(number
  (ask "After `(define (average a b) (/ (+ a b) 2))`, what is `(average 3 8)`?")
  (answer 5.5) (from "(3 + 8) / 2")
  (wrong-if 7 "That is 3 + 8 / 2, the division done first. The body adds a and b first: (+ a b) is inside.")
  (hint "a is 3 and b is 8. Add them, then halve."))
```

---

## A procedure is used like the others

A procedure you define is called just like `+` or `string-append`,
and its arguments can be expressions:

```
> (area (half 8) 3)
12
```

`(half 8)` is worked out first, 4, then `(area 4 3)`. A body can call
other procedures too:

```
(define (greet name)
  (string-append "Hello, " name "!"))
```

`(greet "Ada")` is `"Hello, Ada!"`.

```check
(number
  (ask "After `(define (twice x) (* 2 x))`, what is `(twice (twice 3))`?")
  (answer 12) (from "2 * (2 * 3)")
  (wrong-if 6 "The inner call gives 6, and then twice is called again on that: 2 times 6.")
  (hint "Work out the inner call first."))
```

---

## Long lines

A definition can be typed over several lines, as greet is above: repl
waits until the brackets close. But all that you type at one prompt
can be at most 128 characters, counting new lines and spaces. Past
that, repl takes no more characters.

Keep definitions typed at the prompt short. The lessons set them out
over several lines, indented, to be read; at the prompt, leave out the
spaces at the start of each line. Longer programs go in a file, which
a later lesson shows.

```sources
(source "Zeitlos source: sw/common/zline.h, Z_LINE_MAX 128; sw/apps/repl/repl.c, multi-line entry")
```

```check
(choice
  (ask "You are typing a definition at repl's prompt and the characters stop appearing. Why?")
  (right "It has reached the 128 characters repl takes at one prompt")
  (wrong "A definition can only be one line long"
    "A definition can take several lines; repl waits until its brackets close. The limit is on all the characters together.")
  (wrong "The procedure has too many parameters"
    "Parameters have no such limit. It is the length of what is typed at one prompt.")
  (hint "There is a limit on what one prompt takes."))
```

---

## Worked example

```example
(example
  (problem "Write a procedure that turns degrees Celsius into Fahrenheit: F = C x 1.8 + 32.")
  (step "Its name and parameter: (c-to-f c).")
  (step "Its body: the last step, + 32, is the outer procedure: (+ (* c 1.8) 32).")
  (step "So: (define (c-to-f c) (+ (* c 1.8) 32)), and (c-to-f 100) is 212.")
  (answer "(define (c-to-f c) (+ (* c 1.8) 32))"))
```

```check
(number
  (ask "With that definition, what is `(c-to-f 20)`?")
  (answer 68) (from "20 * 1.8 + 32")
  (wrong-if 36 "That is 20 x 1.8 alone. The body then adds 32.")
  (hint "20 times 1.8, then add 32."))
```

---

## Practice

```check
(number
  (ask "After `(define (cube x) (* x x x))`, what is `(cube 3)`?")
  (answer 27) (from "3 * 3 * 3")
  (wrong-if 9 "That is 3 times 3. The body multiplies three xs.")
  (wrong-if 6 "That is 3 + 3. The body multiplies.")
  (hint "3 times 3 times 3."))
```

```check
(choice
  (ask "Which defines a procedure `add-tax` that adds 20 to its argument?")
  (right "`(define (add-tax p) (+ p 20))`")
  (wrong "`(define add-tax (+ p 20))`"
    "Without brackets round its name, define makes a variable, and p has no value yet. A procedure needs (add-tax p).")
  (wrong "`(define (add-tax) (+ p 20))`"
    "This procedure has no parameter, so p is never given a value. Put p inside the brackets with the name.")
  (hint "The name and its parameter go in brackets together."))
```

So: `(define (name parameters) body)` makes a procedure, and a call
gives its parameters values and works out the body.
