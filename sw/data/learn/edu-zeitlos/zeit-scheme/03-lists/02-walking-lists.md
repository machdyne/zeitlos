# Working through a list

```lesson
(objective "Write procedures that work through a list, by recursion and by a loop.")
(minutes 20)
```

Most procedures on lists follow one pattern: answer for the empty
list, and otherwise do something with the car and go on with the cdr.

```
(define (sum l)
  (if (null? l)
      0
      (+ (car l) (sum (cdr l)))))
```

```
> (sum '(3 1 4 1 5))
14
```

The empty list is the base case: its sum is 0. Each call takes one
item off the front, so the calls always get there.

```sources
(source "Timeless Computing (docs/tc.md), Scheme: Lists")
```

```check
(number
  (ask "With that sum, what is `(sum '())`?")
  (answer 0)
  (wrong-if 1 "The empty list is the base case, and the procedure gives 0 for it: the sum of no numbers.")
  (hint "Which branch of the if is taken for the empty list?"))
```

---

## Counting some of them

The same pattern with `cond` chooses what to do with each item. This
counts the items over 10:

```
(define (count-big l)
  (cond ((null? l) 0)
        ((> (car l) 10) (+ 1 (count-big (cdr l))))
        (else (count-big (cdr l)))))
```

An item over 10 adds 1 to the count of the rest; any other item adds
nothing.

```check
(number
  (ask "What is `(count-big '(5 50 8 80 11))`?")
  (answer 3)
  (wrong-if 2 "11 is over 10 too: 50, 80 and 11.")
  (wrong-if 5 "Only the items over 10 are counted, not all five.")
  (hint "Which items are greater than 10?"))
```

---

## Long lists

sum makes a call that waits for every item, so in repl it stops with a
Scheme error past about 30 items. The procedures repl comes with are
written the same way: `map`, which comes next, manages lists of about
a dozen items, `for-each` about twenty, and `(range 0 n)`, which makes
the list 0 to n - 1, about 30.

A loop with a named let has no such limit. It carries what it has
found so far, and its call is a tail call:

```
(define (total l)
  (let loop ((l l) (t 0))
    (if (null? l)
        t
        (loop (cdr l) (+ t (car l))))))
```

`length`, `reverse` and `list-ref` work on lists of any length that
repl's memory holds: a few hundred numbers.

```sources
(source "Zeitlos source: sw/ext/ms/ms_stdlib.l (map, for-each, range, reverse); sw/apps/repl/Makefile, MS_PROTECT_STACK_SIZE 192")
```

```check
(choice
  (ask "You need the total of a list of 200 numbers in repl. Which works?")
  (right "total, the loop with a named let")
  (wrong "sum, the recursion"
    "sum waits for every item, and repl has room for only about 30 waiting calls.")
  (wrong "Neither: repl cannot hold 200 numbers"
    "A list of 200 numbers fits easily. The limit is on calls waiting at once, not on the list.")
  (hint "Which one makes tail calls?"))
```

---

## Building a list in a loop

A loop can build a list too, with `cons`. Since `cons` adds to the
front, the loop goes from the last item back to the first:

```
(define (upto n)
  (let loop ((i n) (acc '()))
    (if (= i 0)
        acc
        (loop (- i 1) (cons i acc)))))
```

`(upto 5)` is `(1 2 3 4 5)`, and `(total (upto 100))` is 5050.

```check
(choice
  (ask "What would `(upto 3)` be if the loop counted i up from 1 instead, still with cons?")
  (right "(3 2 1)")
  (wrong "(1 2 3)"
    "cons puts each new item in front. Adding 1, then 2, then 3 to the front leaves 3 first.")
  (wrong "(1)"
    "Every item is added, one each time round. Only the order changes.")
  (hint "Add 1, then 2, then 3, each to the front."))
```

---

## Worked example

```example
(example
  (problem "Write (biggest l), the largest number in a list that is not empty.")
  (step "Start with the first item as the biggest so far, and the rest to look at.")
  (step "Each time round, keep (max b (car l)), the larger of the two, and go on with the cdr.")
  (step "When nothing is left, the biggest so far is the answer.")
  (answer "(define (biggest l) (let loop ((l (cdr l)) (b (car l))) (if (null? l) b (loop (cdr l) (max b (car l))))))"))
```

```check
(number
  (ask "What is `(biggest '(3 9 2 7))`?")
  (answer 9)
  (wrong-if 7 "7 is the last item. The loop keeps the largest it has seen, and 9 came before.")
  (hint "Which number is largest?"))
```

So: a list procedure handles the empty list and then the car and the
cdr; a loop does the same for lists of any length.
