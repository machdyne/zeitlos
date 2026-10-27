# Recursion

```lesson
(objective "Write a procedure that calls itself, with a base case that ends it.")
(minutes 20)
(keywords "recursion" "base case")
```

A procedure's body can call the procedure itself. This is
**recursion**, and it is how Scheme repeats things. This one counts
down:

```
(define (countdown n)
  (if (> n 0)
      (begin (print n) (countdown (- n 1)))))
```

`begin` works out several expressions in order. While n is above 0,
countdown prints n, then calls itself with one less. At 0 the test is
false and nothing more is called: the `if` has no second expression,
so its value is `#f`.

```
> (countdown 3)
3
2
1
#f
```

```sources
(source "Zeitlos docs: repl.md, Scheme in ten minutes: Repeating")
```

```check
(choice
  (ask "What would `(countdown 0)` print before repl's #f?")
  (right "Nothing: (> 0 0) is false, so it neither prints nor calls itself")
  (wrong "0"
    "The test (> n 0) comes first. With n as 0 it is false, so print is never reached.")
  (wrong "It counts down for ever"
    "Each call has a smaller n, and at 0 the test stops it. Starting at 0, it stops at once.")
  (hint "Work out the test with n as 0."))
```

---

## The base case

The factorial of n is 1 x 2 x ... x n. It can be said in two parts:
the factorial of 0 is 1, and the factorial of any larger n is n times
the factorial of n - 1. In Scheme it reads just the same:

```
(define (factorial n)
  (if (= n 0)
      1
      (* n (factorial (- n 1)))))
```

The part that needs no further call, here n = 0, is the
**base case**. Every call makes n smaller, so the calls always reach it.

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Recursion")
```

```check
(number
  (ask "What is `(factorial 4)`?")
  (answer 24) (from "4 * 3 * 2 * 1")
  (wrong-if 10 "That is 4 + 3 + 2 + 1. The factorial multiplies: 4 x 3 x 2 x 1.")
  (hint "4 times the factorial of 3, which is 6."))
```

---

## Calls that wait

Each call to factorial must wait for the one it makes before it can
multiply. Move the slider to see the calls pile up and then finish.

```figure
(define n (slider 'n 1 4 3 "n" 1))
(define lines (zs-fact-lines n))
(text 12 20 "Working out (factorial n) by hand:" 'small)
(zs-lines 12 44 17 lines)
(caption (string-append (fmt n 0) " multiplications wait for the base case"))
```

The deepest line, with `(factorial 0)`, has every multiplication
still waiting.

```check
(number
  (ask "Working out `(factorial 3)`, how many multiplications are waiting when `(factorial 0)` is reached?")
  (answer 3)
  (wrong-if 4 "Count the (* in the line with (factorial 0): one for 3, one for 2, one for 1.")
  (wrong-if 0 "None has been done yet: each waits for the call inside it to finish.")
  (hint "Set the slider to 3 and count the (* in the fourth line."))
```

---

## How deep repl can go

Every waiting call takes room, and repl has room for about 30 of them
at once. `(factorial 30)` works; `(factorial 40)` stops with a Scheme
error. The next lesson shows a way to repeat that does not pile up
calls, for when you need to go further.

A recursion with no base case, or one it never reaches, piles up
calls until it hits that limit too. But when the call to itself is the
very last thing it does, as in `down` below, nothing piles up: it runs
for ever, and repl cannot stop it. Do not try one in repl.

```sources
(source "Zeitlos source: sw/apps/repl/Makefile, MS_PROTECT_STACK_SIZE 192; sw/ext/ms/ms.c, protect stack overflow")
```

```check
(choice
  (ask "After `(define (down n) (if (= n 0) 0 (down (- n 2))))`, what happens with `(down 5)`?")
  (right "n goes 5, 3, 1, -1 ... and never equals 0, so it never stops by itself")
  (wrong "Its value is 0"
    "It would be 0 if n reached 0. Taking 2 at a time from 5 jumps past it: 1, then -1.")
  (wrong "Its value is 5"
    "The call never gets back to its start with an answer: n keeps going down past 0.")
  (hint "Write down n for each call, starting at 5."))
```

---

## Worked example

```example
(example
  (problem "Write (power b e): b multiplied by itself e times.")
  (step "Base case: anything to the power 0 is 1, so (= e 0) gives 1.")
  (step "Otherwise b to the power e is b times b to the power e - 1.")
  (step "(define (power b e) (if (= e 0) 1 (* b (power b (- e 1)))))")
  (answer "(power 2 5) is 32."))
```

```example
(example
  (problem "Write (sum-to n): 1 + 2 + ... + n.")
  (step "Base case: (sum-to 0) is 0.")
  (step "Otherwise n plus the sum up to n - 1.")
  (step (choice (ask "So the body is:")
    (right "(if (= n 0) 0 (+ n (sum-to (- n 1))))")
    (wrong "(if (= n 0) 0 (+ n (sum-to n)))"
      "(sum-to n) calls itself with the same n, so it never gets nearer the base case.")
    (wrong "(if (= n 0) 1 (+ n (sum-to (- n 1))))"
      "The sum of no numbers is 0, not 1. With 1, every answer would be 1 too big.")))
  (answer "(define (sum-to n) (if (= n 0) 0 (+ n (sum-to (- n 1)))))"))
```

---

## Practice

```check
(number
  (ask "With that sum-to, what is `(sum-to 4)`?")
  (answer 10) (from "4 + 3 + 2 + 1")
  (wrong-if 24 "That is the factorial, which multiplies. sum-to adds: 4 + 3 + 2 + 1.")
  (hint "4 + 3 + 2 + 1 + 0."))
```

```check
(number
  (ask "With power as above, what is `(power 3 3)`?")
  (answer 27) (from "3^3")
  (wrong-if 9 "That is 3 to the power 2. e is 3, so b is multiplied three times: 3 x 3 x 3.")
  (hint "3 x 3 x 3."))
```

So: a recursive procedure calls itself on a smaller problem, and its
base case ends the calls.
