# Arithmetic

```lesson
(objective "Work out what BASIC prints for a sum, and use whole-number division and MOD.")
(minutes 20)
(keywords "expression (in BASIC)" "operator (in BASIC)" "integer" "remainder (of a division)")
```

In this lesson you make BASIC calculate.

`PRINT` can show the result of a calculation:

```basic
10 PRINT 2 + 3
20 PRINT 7 * 6
```

`RUN` prints `5`, then `42`. A calculation like `7 * 6` is an
**expression**: numbers joined by **operators**, the signs that say
what to do.

| Operator | Means |
|---|---|
| `+` | add |
| `-` | subtract |
| `*` | multiply |
| `/` | divide |
| `MOD` | what is left over after dividing |

Computers use `*` for times, because x is a letter, and letters have
other jobs in a program.

```check
(number
  (ask "What does this line print? 10 PRINT 12 * 4")
  (answer 48) (from "12 * 4")
  (wrong-if 124 "* does not join the digits: it multiplies. 12 times 4 is 48.")
  (wrong-if 16 "That is 12 + 4. The * operator multiplies.")
  (hint "* means times."))
```

---

## Which comes first

As in ordinary arithmetic, BASIC multiplies and divides before it adds
and subtracts. Brackets go first of all.

```basic
10 PRINT 2 + 3 * 4
20 PRINT (2 + 3) * 4
```

prints `14`, then `20`.

```check
(number
  (ask "What does this line print? 10 PRINT 10 - 2 * 3")
  (answer 4) (from "10 - 2 * 3")
  (wrong-if 24 "That works from left to right: 10 - 2 is 8, times 3 is 24. BASIC multiplies first: 2 * 3 is 6, and 10 - 6 is 4.")
  (hint "Do the multiplication first.")
  (hint "2 * 3 is 6. What is 10 - 6?"))
```

---

## Whole numbers only

Every number in this BASIC is an **integer**: a whole number, with no
fraction. So division gives a whole number too, and drops what is
left over:

```basic
10 PRINT 7 / 2
```

prints `3`, not 3.5.

What is left over is the **remainder**, and `MOD` gives it. 7 is three
2s and 1 more, so `7 MOD 2` is `1`.

```sources
(source "Timeless Computing (docs/tc.md), BASIC: Numbers and Arithmetic")
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), sections 2 and 4")
```

```check
(number
  (ask "What does this line print? 10 PRINT 17 / 5")
  (answer 3) (true "3 * 5 <= 17") (true "4 * 5 > 17")
  (wrong-if 3.4 "BASIC has only whole numbers: the fraction is dropped.")
  (wrong-if 2 "That is the remainder. / gives how many whole 5s fit into 17.")
  (hint "How many whole 5s fit into 17?"))
```

---

```check
(number
  (ask "What does this line print? 10 PRINT 17 MOD 5")
  (answer 2) (from "17 - 3 * 5")
  (wrong-if 3 "That is 17 / 5, how many 5s fit. MOD gives what is left over.")
  (hint "Three 5s make 15. How much more is 17?"))
```

---

## A use for MOD

`MOD` answers questions about sharing out and about counting round:
is a number even (`N MOD 2` is 0)? How many eggs are left when you
fill boxes of 6?

```example
(example
  (problem "There are 50 eggs and boxes of 6. How many boxes are filled, and how many eggs are left?")
  (step "Full boxes: 50 / 6, which BASIC gives as 8 (8 boxes hold 48).")
  (step "Left over: 50 MOD 6 is 50 - 48.")
  (step (number (ask "So how many eggs are left?") (answer 2) (from "50 - 8 * 6")
    (wrong-if 8 "8 is the number of full boxes. The eggs left are 50 - 48.")))
  (answer "8 full boxes and 2 eggs left."))
```

---

```check
(number
  (ask "What does this line print? 10 PRINT 100 / 7 * 7")
  (answer 98) (from "14 * 7")
  (wrong-if 100 "In whole numbers, dividing and then multiplying does not always give back what you started with. 100 / 7 is 14, and 14 * 7 is 98.")
  (hint "Work left to right: first 100 / 7, as a whole number.")
  (hint "100 / 7 is 14 in BASIC. Now times 7."))
```

---

```check
(number
  (ask "What does this line print? 10 PRINT (8 + 4) / (5 - 2)")
  (answer 4) (from "12 / 3")
  (wrong-if 9 "Brackets come first: 8 + 4 is 12 and 5 - 2 is 3. Then 12 / 3.")
  (hint "Work out each bracket first."))
```

So: BASIC calculates with whole numbers, multiplies and divides before
it adds and subtracts, and `MOD` gives the remainder.
