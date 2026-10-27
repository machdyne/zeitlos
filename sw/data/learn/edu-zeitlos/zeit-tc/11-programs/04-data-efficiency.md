# Data structures and efficiency

```lesson
(objective "Name the common data structures, and say how the work of linear and binary search grows with the data.")
(minutes 20)
(keywords "data structure" "string (of characters)" "linked list" "hash table" "binary search")
```

How data is arranged matters as much as the steps that work on it. A
few **data structures** cover most needs:

- An **array** is a row of values of the same type, numbered from 0 in
  C and many other languages, each reached directly by its number. Fast
  and simple, but fixed in size.
- A **string** is a sequence of characters, usually stored as an array.
- A **record** groups a few named values that belong together, such as
  a name, a date and an amount.

```sources
(source "Timeless Computing (docs/tc.md), Thinking in Programs: Data Structures")
```

```check
(number
  (ask "An array holds 10 values, numbered from 0. What is the number of the last one?")
  (answer 9) (from "10 - 1")
  (wrong-if 10 "Counting starts at 0, so the numbers are 0 to 9: ten numbers, the last of them 9.")
  (hint "The first is number 0. Count up ten numbers from there."))
```

---

## Chains and trees

- A **linked list** is a chain of records, each holding a value and the
  address of the next. It grows and shrinks easily, but reaching the
  hundredth item means passing through the first 99.
- A **tree** is like a list, but each record can point to several
  others. Folders holding files and more folders make a tree.
- A **hash table** stores values under keys, such as words and their
  meanings, and finds a key quickly without searching every entry. (It
  uses a hash of the key, not usually a cryptographic one, to pick a
  place.)

```check
(choice
  (ask "A program must read the 5,000th item, again and again. Which structure suits it?")
  (right "An array: each item is reached directly by its number")
  (wrong "A linked list: it grows easily"
    "Growing is not the need here. To reach item 5,000 a linked list passes through the 4,999 before it, every time.")
  (hint "Which structure reaches an item by its number, without passing others?"))
```

---

## How the work grows

On a slow computer, efficiency matters. The useful measure is how the
work grows as the data grows.

To find a name in an unsorted list of n names, a program may have to
check all n. Double the list, and the work doubles.

In a sorted list it can do far better: look at the middle entry, decide
which half holds the name, and repeat with that half. This is
**binary search**. Pick a number to look for, and step through the search.

```figure
(define a '(3 7 9 12 15 18 21 24 28 31 35 40 44 47 52 58))
(define t (list-ref a (slider 't 0 15 9 "Which number" 1)))
(define k (slider 'k 0 5 0 "Comparisons" 1))
(define r (sp-search a t k))
(define lo (car r))
(define hi (cadr r))
(define m (caddr r))
(text 160 30 (string-append "looking for " (fmt t 0)) 'centre 'body 'bold)
(sp-boxes a 8 60 19 lo hi m)
(if (<= lo hi)
    (begin (line (+ 8 (* lo 19)) 96 (+ 7 (* (+ hi 1) 19)) 96 'ink 2)
           (text (+ 8 (* 9.5 (+ lo hi 1))) 112 "still possible" 'centre 'small)))
(caption
  (cond ((cadddr r) (string-append "found " (fmt t 0) " after " (fmt k 0) (if (= k 1) " comparison" " comparisons")))
        ((< m 0) "nothing compared yet: all 16 possible")
        ((< (list-ref a m) t) (string-append (fmt (list-ref a m) 0) " is too small: keep the right half"))
        (else (string-append (fmt (list-ref a m) 0) " is too big: keep the left half"))))
```

```check
(number
  (ask "Look for 31 in the figure. How many comparisons does binary search make to find it?")
  (answer 3)
  (wrong-if 10 "That is how many a search from the start would make. Binary search looks at the middle, 24, and throws away the half that cannot hold 31.")
  (wrong-if 1 "The first entry looked at is the middle, 24, not 31. Step on: 40 is next, then 31.")
  (hint "Move Comparisons up one at a time until the figure says found."))
```

---

## Halving

Each comparison halves what is left, so a million names need only about
20 comparisons: $2^20$ is just over a million.

```check
(number
  (ask "At most how many comparisons does binary search need for 1,000 sorted names?")
  (answer 10)
  (true "2^9 < 1000") (true "2^10 >= 1000")
  (wrong-if 500 "That is a search from the start, which on average checks half. Binary search halves what is left each time.")
  (wrong-if 1000 "That checks every name. Each comparison of binary search throws away half of those left.")
  (hint "How many times can you halve 1,000 before 1 is left?")
  (hint "Double from 1 until you pass 1,000: 2, 4, 8, ... 512, 1024."))
```

---

## Work that grows fast

Some methods grow much faster. Comparing every item with every other
takes about n times n steps, so 1,000 items take a million comparisons.
A loop inside a loop over a lot of data is often a sign that a better
method exists.

```check
(number
  (ask "A method takes n times n steps. With 1,000 items it takes 1,000,000. How many steps with 2,000 items?")
  (answer 4000000) (from "2000 * 2000")
  (wrong-if 2000000 "That would be work that doubles with the data. Here both n's double, so the work goes up four times.")
  (hint "2,000 times 2,000."))
```

---

```check
(choice
  (ask "A program checks a list of 1,000,000 sorted names one by one from the start. Which change helps most?")
  (right "Use binary search: about 20 comparisons instead of up to a million")
  (wrong "Run it on a computer twice as fast"
    "That halves the time. Binary search cuts a million comparisons to about 20: a far bigger change than any faster computer.")
  (hint "Compare: half the time, or 20 steps instead of a million?"))
```

So: choose the data structure for how the data is used, and judge a
method by how its work grows with the data.
