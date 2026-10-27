# Tables

```lesson
(objective "Keep a table of names and values as an association list, and look things up in it.")
(minutes 20)
(keywords "dotted pair" "association list")
```

`cons` makes a pair from any two values. When the second is not a
list, repl writes the pair with a dot between its two parts:

```
> (cons 'ben 2501)
(ben . 2501)
> (car (cons 'ben 2501))
ben
> (cdr (cons 'ben 2501))
2501
```

Such a pair is a **dotted pair**. Its car and cdr are its two parts,
as with any pair.

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Association Lists")
```

```check
(choice
  (ask "What is `(cdr (cons 1 2))`?")
  (right "2")
  (wrong "(2)"
    "The cdr of this pair is whatever was put there: the number 2, not a list.")
  (wrong "1"
    "1 is the car, the first part. The cdr is the second.")
  (hint "cons put 1 in the car and 2 in the cdr."))
```

---

## A table

A list of pairs makes a table: each pair a name and its value. It is
called an **association list**. `assq` finds the pair for a symbol,
or gives `#f` if there is none:

```
(define phones
  '((anna . 4711) (ben . 2501) (cleo . 1234)))
```

```
> (assq 'ben phones)
(ben . 2501)
> (cdr (assq 'ben phones))
2501
> (assq 'dora phones)
#f
```

```check
(number
  (ask "With phones as above, what is `(cdr (assq 'cleo phones))`?")
  (answer 1234)
  (hint "Find the pair whose car is cleo, then take its cdr."))
```

---

## Strings as names

For names that are strings, use `assoc`. `assq` compares with `eq?`,
which asks whether two values are one and the same thing in memory;
two strings with the same letters usually are not. `assoc` compares with `equal?`,
which compares the letters:

```
> (define prices (list (cons "tea" 3) (cons "cake" 4)))
prices
> (assoc "cake" prices)
("cake" . 4)
> (assq "cake" prices)
#f
```

```sources
(source "Zeitlos docs: zapi.md, Equality and types; Lists")
```

```check
(choice
  (ask "A table has strings as its names. Why does `assq` not find them?")
  (right "assq compares with eq?, and two strings with the same letters are not the same thing in memory")
  (wrong "assq only works on numbers"
    "assq works on symbols too, as in phones. The trouble is how it compares.")
  (wrong "The strings are in the wrong order"
    "Order does not matter to the search; it looks at every pair until one matches.")
  (hint "Which comparison does each use: eq? or equal??"))
```

---

## A lookup that cannot fail

`(cdr (assoc ...))` stops with an error when the name is missing,
since `(cdr #f)` makes no sense. Test first:

```
(define (price-of item)
  (let ((p (assoc item prices)))
    (if p (cdr p) 0)))
```

`(price-of "tea")` is 3, and `(price-of "jam")` is 0. To add an entry,
put a new pair in front: `(set! prices (cons (cons "jam" 2) prices))`.

```check
(choice
  (ask "Why does price-of test p before taking its cdr?")
  (right "assoc gives #f for a missing name, and the cdr of #f is a Scheme error")
  (wrong "assoc might give the wrong pair"
    "assoc gives the first pair whose name matches, or #f. The danger is the #f.")
  (wrong "To make it faster"
    "The test does not speed anything up. It keeps a missing name from stopping the program.")
  (hint "What does assoc give for a name not in the table?"))
```

---

## Practice

```check
(number
  (ask "After the `set!` that adds jam, what is `(length prices)`?")
  (answer 3)
  (wrong-if 6 "Each entry is one pair, and the list counts pairs: tea, cake and jam.")
  (wrong-if 2 "set! has put the jam pair in front of the other two.")
  (hint "Count the pairs in the table."))
```

```check
(choice
  (ask "What is `(map car phones)`?")
  (right "(anna ben cleo)")
  (wrong "(4711 2501 1234)"
    "Those are the cdrs. car gives the first part of each pair: the names.")
  (wrong "anna"
    "map gives a list, with one answer for every pair.")
  (hint "car of each pair in turn."))
```

So: an association list is a list of pairs, and `assq` or `assoc`
finds a pair by its name.
