# Procedures as values

```lesson
(objective "Hand procedures to map, filter and reduce, and make procedures without names with lambda.")
(minutes 20)
(keywords "higher-order procedure" "lambda")
```

In Scheme a procedure is a value like a number: it can be handed to
another procedure. `map` calls a procedure on every item of a list
and gives a list of the answers:

```
> (define (double x) (* 2 x))
double
> (map double '(1 2 3))
(2 4 6)
> (map string-length '("a" "bb" "ccc"))
(1 2 3)
```

A procedure that takes or gives procedures is a **higher-order
procedure**. In repl, `map` manages lists of about a dozen items.

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Functions as Values")
(source "Zeitlos docs: zapi.md, Lists")
```

```check
(choice
  (ask "What is `(map double '(5 0 -1))`, with double as above?")
  (right "(10 0 -2)")
  (wrong "10"
    "map gives a list: one answer for every item, in order.")
  (wrong "(5 0 -1 5 0 -1)"
    "double is called on each item, giving 2 times it. Nothing is repeated.")
  (hint "Call double on each item in turn."))
```

---

## filter and reduce

`filter` keeps the items for which a predicate is true. `reduce`
combines the items one at a time, from a starting value:

```
> (filter even? '(1 2 3 4 5 6))
(2 4 6)
> (reduce + 0 '(1 2 3 4))
10
```

`(reduce + 0 l)` works out `(+ (+ (+ (+ 0 1) 2) 3) 4)`. With `*` and
1 it multiplies them all instead.

```check
(number
  (ask "What is `(reduce * 1 '(1 2 3 4))`?")
  (answer 24) (from "1 * 2 * 3 * 4")
  (wrong-if 0 "Starting from 0 would make every product 0, but the starting value here is 1.")
  (wrong-if 10 "That adds them. The procedure handed to reduce is *, so they are multiplied.")
  (hint "1 x 1, then x 2, x 3, x 4."))
```

---

## lambda

Often a procedure is needed only once, to hand to `map` or `filter`.
**lambda** makes a procedure without giving it a name:

```
> (filter (lambda (x) (> x 10)) '(5 50 8 80))
(50 80)
```

`(lambda (x) (> x 10))` reads: a procedure of x that works out `(> x
10)`. In fact `(define (sq x) (* x x))` is short for `(define sq
(lambda (x) (* x x)))`.

```check
(number
  (ask "What is `((lambda (x) (* x x)) 5)`?")
  (answer 25) (from "5 * 5")
  (wrong-if 10 "The body is (* x x), x times x, not x + x.")
  (hint "The first item is a procedure; 5 is its argument."))
```

---

## for-each and apply

`for-each` calls a procedure on every item for what it does, not for
an answer, and its value is `#f`. `apply` calls a procedure with a
list's items as its arguments:

```
> (for-each print '("a" "b"))
a
b
#f
> (apply + '(1 2 3))
6
```

```check
(choice
  (ask "You want to show each name in a list on its own line. Which do you use?")
  (right "`(for-each print names)`")
  (wrong "`(apply print names)`"
    "apply calls print once, with all the names as arguments. for-each calls it once for each name.")
  (wrong "`(filter print names)`"
    "filter keeps items by a test. It would print them, but it is meant to choose, and it gives a list back.")
  (hint "Which one calls the procedure for each item, for its output?"))
```

---

## Practice

```example
(example
  (problem "Give the list of words in a list, each with an ! after it.")
  (step "A procedure of one word s: (lambda (s) (string-append s \"!\")).")
  (step "Hand it to map with the list.")
  (answer "(map (lambda (s) (string-append s \"!\")) '(\"hi\" \"yo\")) is (\"hi!\" \"yo!\")."))
```

```check
(choice
  (ask "What is `(filter odd? '(1 2 3 4 5))`?")
  (right "(1 3 5)")
  (wrong "(2 4)"
    "filter keeps the items for which the predicate is true: the odd ones.")
  (wrong "3"
    "filter gives a list of the items it keeps, not how many there are.")
  (hint "Keep each item for which (odd? item) is #t."))
```

```check
(number
  (ask "What is `(reduce + 0 (map double '(1 2 3)))`?")
  (answer 12) (from "2 + 4 + 6")
  (wrong-if 6 "That adds 1, 2 and 3. map doubles them first: 2, 4 and 6.")
  (hint "Work out the map first."))
```

So: procedures are values, and `map`, `filter`, `reduce` and `lambda`
do in one expression what a loop would.
