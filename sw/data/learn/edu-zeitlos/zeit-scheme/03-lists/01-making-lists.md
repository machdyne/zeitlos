# Making lists

```lesson
(objective "Make lists, take them apart with car and cdr, and add to them with cons.")
(minutes 20)
(keywords "list (in Scheme)" "empty list" "pair (in Scheme)" "symbol (in Scheme)")
```

A **list** holds several values in order, as one value. `list` makes
one from its arguments:

```
> (define days (list "mon" "tue" "wed"))
days
> days
("mon" "tue" "wed")
> (length days)
3
```

`car` gives the first item of a list, and `cdr` (say "could-er") the
rest of it, as a list:

```
> (car days)
"mon"
> (cdr days)
("tue" "wed")
```

```sources
(source "Zeitlos docs: repl.md, Scheme in ten minutes: Lists")
(source "Timeless Computing (docs/tc.md), Scheme: Lists")
```

```check
(choice
  (ask "With days as above, what is `(car (cdr days))`?")
  (right "\"tue\"")
  (wrong "(\"tue\" \"wed\")"
    "That is (cdr days). car then takes the first item of it.")
  (wrong "\"mon\""
    "\"mon\" is (car days). Here car is taken of the rest, after mon.")
  (hint "Work out (cdr days) first, then take its car."))
```

---

## A chain of pairs

Inside the machine, a list is a chain of **pairs**. Each pair holds one
item and the way to the next pair. The last pair holds `()`, the
**empty list**, which ends the chain.

```figure
(define k (slider 'k 0 3 0 "cdr taken" 1))
(zs-pairs 20 70 70 100 '("mon" "tue" "wed") k)
(text 20 50 "days" 'small)
(arrow 34 54 34 68 'ink 1)
(text 20 140 (cond ((= k 0) "days")
                   ((= k 1) "(cdr days)")
                   ((= k 2) "(cdr (cdr days))")
                   (else "(cdr (cdr (cdr days)))")) 'small 'bold)
(text 20 160 (cond ((= k 0) "(\"mon\" \"tue\" \"wed\")")
                   ((= k 1) "(\"tue\" \"wed\")")
                   ((= k 2) "(\"wed\")")
                   (else "()")) 'small)
(caption "The heavy pairs are the list that is left")
```

`cdr` makes nothing new: it follows the chain one pair along. Take it
three times from days and you reach `()`. `(null? x)` is `#t` when x
is the empty list.

```check
(choice
  (ask "What is `(cdr (list \"x\"))`?")
  (right "()")
  (wrong "\"x\""
    "\"x\" is the car. The cdr is the rest after it, and nothing is left: the empty list.")
  (wrong "#f"
    "The rest of a one-item list is a list too: the empty one, ().")
  (hint "Move the slider to the last pair: what does its right half hold?"))
```

---

## cons

`cons` makes one new pair: an item in front of a list.

```
> (cons "sun" days)
("sun" "mon" "tue" "wed")
> days
("mon" "tue" "wed")
```

days itself is unchanged; the new list shares its pairs. `(cons 1
'())` is `(1)`, a list of one.

```check
(choice
  (ask "What is `(cons 0 (list 1 2))`?")
  (right "(0 1 2)")
  (wrong "(1 2 0)"
    "cons puts the new item in front, never at the end.")
  (wrong "(0 (1 2))"
    "cons does not put the list inside a new one: the item goes in front of the list's own items.")
  (hint "cons adds one item at the front."))
```

---

## Quote

A **quote** mark, `'`, in front of something says: this itself, not
its value. A quoted list is a list as written, not worked out:

```
> '(1 (+ 1 1) 3)
(1 (+ 1 1) 3)
> (list 1 (+ 1 1) 3)
(1 2 3)
```

A quoted word is a **symbol**: a name used as a value, as in
`'(mon tue wed)`. `'mon` is the symbol mon; `mon` without the quote is
a variable, which here has no value, so repl answers `#f`.

```check
(choice
  (ask "What is `(car '(+ 1 2))`?")
  (right "+")
  (wrong "3"
    "The quote stops (+ 1 2) being worked out: it is a list of three items, and the first is the symbol +.")
  (wrong "1"
    "Count from the front: the first item is +.")
  (hint "The quote makes it a list of three items. Which is first?"))
```

---

## Practice

```check
(number
  (ask "What is `(length (cons \"a\" (list \"b\" \"c\")))`?")
  (answer 3)
  (wrong-if 2 "cons adds one item in front of the two: three in all.")
  (hint "cons makes the list one longer."))
```

```check
(choice
  (ask "Which gives the third item of a list l?")
  (right "`(car (cdr (cdr l)))`")
  (wrong "`(cdr (cdr (cdr l)))`"
    "Three cdrs pass the third item and give what is after it. The last step must be car.")
  (wrong "`(car (car (car l)))`"
    "car gives the first item, which is not a list. To move along, use cdr.")
  (hint "Two steps along with cdr, then take the first."))
```

So: a list is a chain of pairs ending in `()`; `car`, `cdr` and `cons`
take it apart and build it.
