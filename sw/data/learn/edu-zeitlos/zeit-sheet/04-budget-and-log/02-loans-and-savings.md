# Loans and savings

```lesson
(objective "Work out a loan's monthly payment with PMT and what regular saving grows to with FV.")
(minutes 20)
(keywords "interest (on money)" "loan (of money)")
```

In this lesson you learn to work out what a loan costs each month, and
what saving each month comes to.

A **loan** is money borrowed and paid back over time. **Interest** is
what the lender charges for it, a percentage of what is still owed. At
5% a year, charged monthly, each month adds a twelfth of 5% of what is
owed that month.

Loans are paid monthly, so a formula uses the **monthly** rate, the
yearly rate divided by 12, and counts the payments in months.

```check
(number
  (ask "A loan is for 30 years, paid monthly. How many payments is that?")
  (answer 360) (from "30 * 12")
  (wrong-if 30 "Payments are made monthly, not yearly: 12 a year.")
  (hint "12 payments a year."))
```

---

## PMT: the payment

`=PMT(rate,n,amount)` gives the payment for each period that pays a loan
off: `rate` is the interest rate for one period, `n` the number of
periods, `amount` what is borrowed.

```example
(example
  (problem "200,000 is borrowed for 30 years at 5% a year, paid monthly. What is the monthly payment?")
  (step "The monthly rate is 0.05/12.")
  (step "There are 30 times 12, 360, payments.")
  (step "So the formula is =PMT(0.05/12,360,200000).")
  (step "It gives -1073.64 (and more places): about 1,073.64 a month.")
  (answer "1,073.64 a month"))
```

The answer is negative: in these formulas, money you pay out is
negative and money you receive is positive.

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md), Formulas")
```

```check
(choice
  (ask "Why is the PMT answer negative?")
  (right "It is money you pay out, which these functions show as negative")
  (wrong "The loan loses money" "The sign only says which way the money goes: out of your pocket.")
  (wrong "There is a mistake in the formula" "The formula is right. Money paid out is negative in PMT, PV and FV."))
```

---

## What the loan costs

Over the 30 years you make 360 payments of 1,073.64. Everything above
the 200,000 you borrowed is interest.

```check
(number
  (ask "360 payments of 1,073.64 repay a loan of 200,000. How much is paid in interest, to the nearest whole number?")
  (answer 186510) (within 1) (from "360 * 1073.64 - 200000")
  (wrong-if 386510 "That is everything paid back. Take away the 200,000 borrowed to find the interest.")
  (hint "First the total paid: 360 times 1,073.64.")
  (hint "Then take away the 200,000 borrowed."))
```

---

## FV: what saving grows to

`=FV(rate,n,payment)` gives what regular saving grows to after n
periods, with interest added each period. Saving 100 a month is a
payment out, so it is written -100.

`=FV(0.06/12,120,-100)` is 100 a month for 10 years at 6% a year: about
16,387.93.

```check
(number
  (ask "Saving 100 a month for 10 years grows to 16,387.93. How much of that is interest, rather than what you put in?")
  (answer 4387.93) (within 0.01) (from "16387.93 - 100 * 120")
  (wrong-if 6387.93 "You put in 100 a month for 120 months: 12,000, not 10,000.")
  (hint "You put in 100 times 120 months."))
```

---

## Showing the answer

`PMT` gives many places after the point. Round it with `ROUND`, or
give the cell the format `$2`. In a column 9 places wide,
`-$1,073.64` shows as `####`: it needs 10. Widen the column with
Alt+Right.

```check
(text
  (ask "Write a formula for the monthly payment on 10,000 borrowed for 5 years at 6% a year, paid monthly. (Type the formula.)")
  (accept "=PMT(0.06/12,60,10000)") (accept "PMT(0.06/12,60,10000)") (accept "=PMT(0.005,60,10000)") (accept "PMT(0.005,60,10000)")
  (wrong-if "=PMT(0.06,5,10000)" "That is the yearly payment. Paid monthly, the rate is 0.06/12 and there are 60 payments.")
  (wrong-if "=PMT(6,60,10000)" "The rate is written as a number: 6% is 0.06, and monthly it is 0.06/12.")
  (hint "Monthly rate: 0.06/12. Payments: 5 times 12.")
  (hint "=PMT(rate,payments,amount)"))
```

---

```try
(try (app "sheet")
  (task "In sheet, type =PMT(0.05/12,360,200000) into a cell, then =ROUND(PMT(0.05/12,360,200000),2) into the cell below. Come back here.")
  (check (number (ask "What does the second cell show?") (answer -1073.64)
    (wrong-if 1073.64 "Look again: PMT gives money paid out as negative, -1073.64.")
    (hint "It is negative, with 2 places."))))
```

A loan's payment is PMT of the monthly rate, the number of months and
the amount, and FV shows what regular saving grows to; money paid out is
negative.
