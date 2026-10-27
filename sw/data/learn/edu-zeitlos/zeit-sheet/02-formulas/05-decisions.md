# Decisions and rounding

```lesson
(objective "Compare numbers, choose between two results with IF, and round with ROUND and INT.")
(minutes 20)
(keywords "comparison (in a formula)" "condition (in a formula)")
```

In this lesson you learn to make a formula choose its answer, and to
round numbers.

A **comparison** asks a question about two numbers and gives 1 for yes
(true) or 0 for no (false):

| Written | Asks |
|---|---|
| = | equal? |
| <> | not equal? |
| <, > | less than? greater than? |
| <=, >= | less than or equal? greater than or equal? |

So `=B2>100` shows 1 when B2 is more than 100, and 0 when it is not.

```check
(number
  (ask "B2 holds 80. What does =B2>=80 show?")
  (answer 1)
  (wrong-if 0 ">= means greater than or equal. 80 is equal to 80, so the answer is yes: 1.")
  (wrong-if 80 "A comparison gives 1 or 0, not the number itself.")
  (hint "Is 80 greater than or equal to 80?"))
```

---

## IF

`=IF(condition,a,b)` gives **a** when the **condition** is true and
**b** when it is false. The condition is usually a comparison.

`=IF(B2>100,B2*0.1,0)` gives a discount of a tenth on orders over 100,
and 0 on smaller ones.

```figure
(define v (slider 'v 0 200 120 "B2 (order)" 10))
(zs-bar 8 4 304 "C2" "=IF(B2>100,B2*0.1,0)")
(zs-place 50 56 80 20 0 1)
(zs-sheet (list '("Item" "Order" "Discount") (list "chairs" (fmt v 0) (zs-g (if (> v 100) (* v 0.1) 0)))))
(zs-box 2 2 2 2)
(text 160 130 (if (> v 100) "B2>100 is true: B2*0.1" "B2>100 is false: 0") 'body 'centre)
```

```check
(number
  (ask "B2 holds 100. What does =IF(B2>100,B2*0.1,0) show?")
  (answer 0)
  (wrong-if 10 "100 is not more than 100, so the condition is false and IF gives the last argument, 0. For 100 to count, use >=.")
  (hint "Is 100 greater than 100?"))
```

---

## Numbers only

In `sheet`, a formula always gives a number. IF chooses between two
numbers (or two formulas that give numbers); it cannot give text.

`AND`, `OR` and `NOT` join conditions: `=IF(AND(B2>0,B2<10),1,0)` is 1
only when B2 is between 0 and 10.

```check
(choice
  (ask "Which of these formulas can sheet work out?")
  (right "=IF(OR(B2<0,B2>9),1,0)")
  (wrong "=IF(B2>100,\"big\",\"small\")" "In sheet a formula gives numbers only, so IF cannot choose between two pieces of text.")
  (wrong "=IF(B2>100)" "IF needs at least a condition and the answer to give when it is true.")
  (hint "Formulas in sheet give numbers."))
```

---

## Rounding

`=ROUND(x,n)` rounds x to n places after the point: `=ROUND(2.346,2)`
is 2.35, and `=ROUND(2.5,0)` is 3 (a half rounds away from zero).

`=INT(x)` rounds **down** to a whole number: `=INT(7.9)` is 7, and
`=INT(-1.5)` is -2, the whole number below it.

```check
(number
  (ask "What does =ROUND(12.3456,2) show?")
  (answer 12.35) (from "12.35")
  (wrong-if 12.34 "That cuts off the digits. ROUND looks at the next digit, 5, and rounds up.")
  (wrong-if 12 "That is 0 places. The 2 asks for 2 places after the point.")
  (hint "Keep 2 places: 12.34, then look at the next digit."))
```

---

## Remainders and sizes

`=MOD(x,y)` is the remainder when x is divided by y: `=MOD(17,5)` is 2.
`=ABS(x)` is x without its minus sign: `=ABS(-4)` is 4.

```check
(number
  (ask "200 eggs are packed in boxes of 12. What does =MOD(200,12) show, the eggs left over?")
  (answer 8) (from "200 - 16*12")
  (wrong-if 16 "That is how many full boxes: INT(200/12). MOD gives what is left over.")
  (hint "16 boxes take 192 eggs.")
  (hint "200 - 192."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type 150 in B2 and =IF(B2>100,B2*0.1,0) in C2. Then change B2 to 90. Come back here.")
  (check (number (ask "What does C2 show now?") (answer 0)
    (wrong-if 9 "90 is not more than 100, so IF gives its last argument, 0.")
    (hint "Is 90 more than 100?"))))
```

---

```check
(number
  (ask "What does =INT(19/4) show?")
  (answer 4) (from "4")
  (wrong-if 4.75 "That is 19/4 itself. INT rounds it down to a whole number.")
  (wrong-if 5 "INT always rounds down, not to the nearest: 4.75 becomes 4.")
  (hint "19/4 is 4.75."))
```

Comparisons give 1 or 0, IF picks one of two numbers by a condition,
and ROUND and INT tidy a result.
