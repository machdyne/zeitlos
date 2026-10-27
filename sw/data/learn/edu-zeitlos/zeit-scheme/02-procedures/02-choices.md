# Choices

```lesson
(objective "Compare values and choose between expressions with if and cond.")
(minutes 20)
(keywords "truth value" "predicate (in Scheme)")
```

Scheme has two **truth values**: `#t`, true, and `#f`, false. A
comparison works one out:

| Expression | Value |
|---|---|
| (> 5 3) | #t |
| (= 2 3) | #f |
| (<= 4 4) | #t |

A procedure whose value is a truth value is a **predicate**. Many have
names ending in `?`: `(even? 10)` is `#t`, `(zero? 7)` is `#f`, and
`(string=? "cat" "cat")` compares two strings.

Give `<`, `>`, `<=` and `>=` two numbers at a time. In this Scheme
they look only at their first two arguments, so `(< 1 3 2)` comes out
`#t`.

```sources
(source "Zeitlos docs: zapi.md, Numbers; Equality and types")
(source "Zeitlos source: sw/ext/ms/ms.c, bi_lt() compares two arguments")
```

```check
(choice
  (ask "What is the value of `(< 10 4)`?")
  (right "#f")
  (wrong "#t"
    "(< 10 4) asks: is 10 less than 4? It is not, so the answer is false, #f.")
  (wrong "6"
    "< compares; it does not take away. Its value is a truth value.")
  (hint "Read it as: is 10 less than 4?"))
```

---

## if

`if` takes a test and two expressions. When the test is true, the
first is worked out; otherwise the second:

```
> (if (> 5 3) "bigger" "smaller")
"bigger"
```

Only `#f` counts as false. Every other value, even 0 or the empty
string `""`, counts as true:

```
> (if 0 "yes" "no")
"yes"
```

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Choices")
```

```check
(choice
  (ask "What is the value of `(if (= 1 2) \"same\" \"different\")`?")
  (right "\"different\"")
  (wrong "\"same\""
    "(= 1 2) is #f, so if takes its second expression.")
  (wrong "#f"
    "#f is the test's value. if then gives one of its two expressions: here the second.")
  (hint "Work out the test first: is 1 equal to 2?"))
```

---

## if in a procedure

A procedure can choose what to work out:

```
(define (bigger a b)
  (if (> a b) a b))
```

`(bigger 4 9)` tests `(> 4 9)`, which is `#f`, so its value is b, 9.

```check
(number
  (ask "After `(define (fee age) (if (< age 12) 2 5))`, what is `(fee 30)`?")
  (answer 5)
  (wrong-if 2 "(< 30 12) is #f: 30 is not less than 12. So if gives its second expression, 5.")
  (hint "Is 30 less than 12?"))
```

---

## cond: more than two ways

`cond` tries its tests in order and works out the expression of the
first that is true. `else` catches everything left:

```
(define (size n)
  (cond ((< n 10) "small")
        ((< n 100) "medium")
        (else "large")))
```

`(size 42)`: 42 is not less than 10, but it is less than 100, so the
value is `"medium"`. The later tests are never tried.

```check
(choice
  (ask "With that `size`, what is `(size 100)`?")
  (right "\"large\"")
  (wrong "\"medium\""
    "(< 100 100) is #f: 100 is not less than itself. So cond goes on to else.")
  (wrong "\"small\""
    "(< 100 10) is #f, so the first clause is passed over.")
  (hint "Try each test in turn with n as 100."))
```

---

## and, or, not

`(and a b)` is true when both are; `(or a b)` when either is; `(not
a)` turns true into false and false into true:

```
> (and (> 3 1) (< 3 5))
#t
> (not (= 1 2))
#t
```

So a test for "between 1 and 10" is `(and (>= n 1) (<= n 10))`.

```check
(choice
  (ask "n is 15. What is `(or (< n 0) (> n 10))`?")
  (right "#t")
  (wrong "#f"
    "(> 15 10) is true, and or needs only one true test.")
  (wrong "15"
    "or gives a truth value here: one of its tests is #t, so its value is #t.")
  (hint "Is either test true?"))
```

---

## Practice

```example
(example
  (problem "Write a procedure (sign n) that gives \"negative\", \"zero\" or \"positive\".")
  (step "Three answers, so cond.")
  (step "First test: (< n 0) gives \"negative\". Second: (= n 0) gives \"zero\".")
  (step "Anything left is positive: (else \"positive\").")
  (answer "(define (sign n) (cond ((< n 0) \"negative\") ((= n 0) \"zero\") (else \"positive\")))"))
```

```check
(choice
  (ask "After `(define (grade p) (cond ((>= p 90) \"A\") ((>= p 70) \"B\") (else \"C\")))`, what is `(grade 70)`?")
  (right "\"B\"")
  (wrong "\"C\""
    "(>= 70 70) is #t: 70 is greater than or equal to 70. So the second clause is taken.")
  (wrong "\"A\""
    "(>= 70 90) is #f, so the first clause is passed over.")
  (hint "Try the tests in order with p as 70."))
```

```check
(choice
  (ask "What is the value of `(if 0 \"yes\" \"no\")`?")
  (right "\"yes\"")
  (wrong "\"no\""
    "In Scheme only #f is false. 0 is a number, and it counts as true.")
  (hint "Which values count as false in Scheme?"))
```

So: comparisons and predicates give `#t` or `#f`, and `if` and `cond`
choose by them; only `#f` is false.
