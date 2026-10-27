# Loops

```lesson
(objective "Name values for one expression with let, and repeat any number of times with a named let.")
(minutes 20)
(keywords "local variable" "tail call" "named let")
```

`let` names values for one expression only:

```
> (let ((w 10) (h 4)) (* w h))
40
```

Inside the `let`, w is 10 and h is 4; outside it, they do not exist.
A variable that lives only inside one expression is a
**local variable**. Parameters are local variables too: x in `half` means
nothing outside `half`.

```sources
(source "Zeitlos docs: repl.md, Scheme in ten minutes: Local names; zapi.md, Syntax: let")
```

```check
(number
  (ask "What is the value of `(let ((a 3) (b 5)) (* a b))`?")
  (answer 15) (from "3 * 5")
  (wrong-if 8 "The body multiplies: (* a b), with a as 3 and b as 5.")
  (hint "Put 3 for a and 5 for b in the body."))
```

---

## A call with nothing left to do

In factorial, each call waits to multiply after the call it makes. But
when a call is the very last thing a procedure does, nothing waits for
it. That is a **tail call**, and Scheme then needs no room to
remember anything: such calls never pile up.

```
(define (gcd-of a b)
  (if (= b 0)
      a
      (gcd-of b (remainder a b))))
```

`(gcd-of 1071 462)` is 21: the greatest number that divides both.
The call to gcd-of is the last thing done, so it can go on as long as
it needs.

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Tail Calls; Three Languages Side by Side: Greatest Common Divisor")
```

```check
(choice
  (ask "In `(* n (factorial (- n 1)))`, why is the call to factorial not a tail call?")
  (right "The multiplication by n still has to be done after it returns")
  (wrong "Because it calls factorial with a smaller number"
    "gcd-of calls itself with smaller numbers too, and that is a tail call. What matters is whether anything is left to do after the call.")
  (wrong "Because factorial has a base case"
    "Tail calls have nothing to do with the base case. Look at what happens after the call returns.")
  (hint "When the inner call gives its answer, is anything still to be done with it?"))
```

---

## Named let

A **loop** repeats steps. In Scheme you write one with a
**named let**: a `let` with a name, which works as a procedure you can call to
go round again with new values.

```
(define (sum-to n)
  (let loop ((i 1) (total 0))
    (if (> i n)
        total
        (loop (+ i 1) (+ total i)))))
```

loop starts with i as 1 and total as 0. Each `(loop ...)` is a tail
call carrying the next values. `(sum-to 100)` is 5050, and
`(sum-to 1000)` works as well: no calls pile up.

```check
(choice
  (ask "In that sum-to, what are i and total on the third time round the loop?")
  (right "i is 3 and total is 3")
  (wrong "i is 3 and total is 6"
    "total is 6 only after 3 has been added. On the third time round it holds 1 + 2.")
  (wrong "i is 2 and total is 3"
    "i starts at 1 and goes up one each time: 1, 2, 3.")
  (hint "Write them down: (1, 0), then (2, 1), then ..."))
```

---

## A loop that does things

The body of a loop can show output before it goes round again:

```
(define (count-to n)
  (let loop ((i 1))
    (if (<= i n)
        (begin (display i) (display " ")
               (loop (+ i 1))))))
```

```
> (count-to 5)
1 2 3 4 5 #f
```

A loop runs until its test lets it stop, and repl answers nothing until
then. Try a new loop with a small number first.

```check
(choice
  (ask "What would `(count-to 0)` show before repl's #f?")
  (right "Nothing")
  (wrong "0"
    "loop starts with i as 1, and (<= 1 0) is false, so nothing is displayed.")
  (wrong "1"
    "The test comes before display: (<= 1 0) is false.")
  (hint "Work out the test with i as 1 and n as 0."))
```

---

## Worked example

```example
(example
  (problem "Write (fact n), the factorial, as a loop.")
  (step "Carry a counter i from 1 to n, and the product so far, acc, starting at 1.")
  (step "Stop when i passes n: the answer is acc.")
  (step "Otherwise go round with i + 1 and acc times i.")
  (answer "(define (fact n) (let loop ((i 1) (acc 1)) (if (> i n) acc (loop (+ i 1) (* acc i)))))"))
```

```check
(number
  (ask "With that loop, what is `(fact 5)`?")
  (answer 120) (from "5 * 4 * 3 * 2 * 1")
  (hint "1 x 2 x 3 x 4 x 5."))
```

---

## Practice

```check
(number
  (ask "After `(define (sum-sq n) (let loop ((i 1) (t 0)) (if (> i n) t (loop (+ i 1) (+ t (* i i))))))`, what is `(sum-sq 3)`?")
  (answer 14) (from "1 + 4 + 9")
  (wrong-if 6 "That adds 1 + 2 + 3. Each time round, t gets i times i added: 1 + 4 + 9.")
  (hint "Add the squares of 1, 2 and 3."))
```

```check
(number
  (ask "What is `(gcd-of 12 18)`?")
  (answer 6)
  (true "12 / 6 = 2") (true "18 / 6 = 3")
  (wrong-if 2 "2 divides both, but so does a larger number.")
  (wrong-if 3 "3 divides both, but so does a larger number.")
  (hint "The greatest number that divides both 12 and 18."))
```

So: `let` makes local variables, and a named let repeats with tail
calls, which never pile up.
