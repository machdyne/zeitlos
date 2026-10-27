# Numbers and nesting

```lesson
(objective "Write a calculation as nested expressions, and read the numbers repl prints.")
(minutes 15)
(keywords "nested (expression)")
```

An argument can be an expression itself. Then the expression is
**nested**, and Scheme works out the inner one first:

```
> (+ 1 (* 2 3))
7
```

`(* 2 3)` is 6, so the whole is `(+ 1 6)`, which is 7. The brackets
say exactly what belongs with what, so Scheme needs no rule about
doing multiplication before addition.

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Expressions")
```

```check
(number
  (ask "What is the value of `(* 2 (+ 3 4))`?")
  (answer 14) (from "2 * (3 + 4)")
  (wrong-if 10 "That is 2 times 3, plus 4. The brackets put 3 and 4 together: (+ 3 4) is 7 first, then 2 times 7.")
  (hint "Work out the inner expression, (+ 3 4), first.")
  (hint "It is 7. Now 2 times 7."))
```

---

## From a sum to an expression

To write a calculation in Scheme, start from the step done last: it
becomes the outer procedure. In 3 + 4 x 5 the multiplication is done
first and the addition last, so:

```
(+ 3 (* 4 5))
```

```check
(choice
  (ask "Which expression works out (2 + 3) x 4?")
  (right "`(* (+ 2 3) 4)`")
  (wrong "`(+ 2 (* 3 4))`"
    "That is 2 + 3 x 4, which is 14. In (2 + 3) x 4 the addition comes first, so it is the inner expression.")
  (wrong "`(* 2 + 3 4)`"
    "Each procedure needs its own brackets. Here + would be an argument of *, not a sum.")
  (hint "The step done last is the multiplication, so it is the outer procedure."))
```

---

## Division

`/` divides, and its answer need not be a whole number:

| Expression | Value |
|---|---|
| (/ 10 4) | 2.5 |
| (/ 1 3) | 0.333333 |
| (/ 22 7) | 3.14286 |

Every number in this Scheme can have a fraction part. When it has
one, repl prints six digits of the number in all, rounded: one third is not
exactly 0.333333, but that is what you see.

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Numbers")
(source "Zeitlos docs: zapi.md, Numbers")
```

```check
(number
  (ask "What does repl print for `(/ 7 2)`?")
  (answer 3.5) (from "7 / 2")
  (wrong-if 3 "That is division with the fraction thrown away. / keeps the fraction part.")
  (hint "7 divided by 2."))
```

---

## Whole-number division

Sometimes you want how many times one number goes into another, and
what is left over. `quotient` and `remainder` give the two:

```
> (quotient 17 5)
3
> (remainder 17 5)
2
```

5 goes into 17 three times (15), and 2 is left.

```check
(number
  (ask "What is the value of `(remainder 20 6)`?")
  (answer 2) (from "20 - 3 * 6")
  (wrong-if 3 "3 is the quotient: 6 goes into 20 three times. The remainder is what is left: 20 - 18.")
  (wrong-if 3.33333 "That is (/ 20 6). remainder gives what is left over after whole sixes.")
  (hint "How many whole sixes fit in 20, and how much is left?"))
```

---

## Worked example

```example
(example
  (problem "Work out the value of (- (* 4 5) (/ 9 3)).")
  (step "There are two inner expressions. (* 4 5) is 20.")
  (step "(/ 9 3) is 3.")
  (step "The outer one is now (- 20 3).")
  (answer "17"))
```

```example
(example
  (problem "Work out the value of (+ (* 2 5) (quotient 9 2)).")
  (step "(* 2 5) is 10.")
  (step (number (ask "What is (quotient 9 2)?") (answer 4) (from "(9 - 1) / 2")
    (wrong-if 4.5 "quotient keeps only the whole part: 2 goes into 9 four times.")))
  (step (number (ask "So what is the value of the whole expression?") (answer 14) (from "10 + 4")))
  (answer "14"))
```

---

## Practice

```check
(number
  (ask "What is the value of `(+ 1 (* 2 3) (- 10 4))`?")
  (answer 13) (from "1 + 2 * 3 + (10 - 4)")
  (hint "Three arguments: 1, then (* 2 3), then (- 10 4). Add them."))
```

```check
(number
  (ask "What is the value of `(quotient 23 4)`?")
  (answer 5) (from "(23 - 3) / 4")
  (wrong-if 5.75 "That is (/ 23 4). quotient keeps only how many whole fours fit.")
  (wrong-if 3 "3 is the remainder. The quotient is how many whole fours fit in 23.")
  (hint "4 x 5 is 20, and 4 x 6 is 24."))
```

```check
(choice
  (ask "Which expression works out 10 - 2 x 3?")
  (right "`(- 10 (* 2 3))`")
  (wrong "`(* (- 10 2) 3)`"
    "That takes 2 from 10 first, which gives 24. The multiplication is done first, so it goes inside.")
  (wrong "`(- (* 10 2) 3)`"
    "That multiplies 10 by 2. In 10 - 2 x 3, the 2 is multiplied by 3.")
  (hint "The step done last, the taking away, is the outer procedure."))
```

So: inner expressions are worked out first, and `/` keeps the fraction
part while `quotient` and `remainder` do not.
