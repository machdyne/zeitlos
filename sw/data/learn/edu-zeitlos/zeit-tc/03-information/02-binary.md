# Counting in binary

```lesson
(objective "Read a binary number, and write a number in binary.")
(minutes 15)
(keywords "binary" "decimal (everyday word)" "place value")
```

We write numbers in **decimal**, base 10: what a digit's place is
worth, its **place value**, is ten times the place to its right. In 352, the 3 is three hundreds, the 5 five
tens and the 2 two ones.

**Binary**, base 2, works the same way with two digits, 0 and 1. Each
place is worth **twice** the place to its right: 1, 2, 4, 8, 16 and so
on. To read a binary number, add the values of the places that hold a 1.

Move the slider and watch the places add up.

```figure
(define n (slider 'n 0 255 140 "Number" 1))
(bit-boxes 32 60 8 n 32 #t #f)
(text 160 140 (bit-sum n 8) 'centre 'body)
(text 160 170 (string-append "= " (fmt n 0)) 'centre 24)
```

```sources
(source "Timeless Computing (docs/tc.md), Information: Counting in Binary")
```

```check
(number
  (ask "What is the binary number 1011 in decimal?")
  (answer 11) (from "8 + 2 + 1")
  (wrong-if 1011 "That is the binary digits read as a decimal number. In binary the places are worth 8, 4, 2 and 1.")
  (wrong-if 13 "That is 1101: the digits read from the wrong end. The leftmost digit is worth the most.")
  (hint "The places, from the left, are worth 8, 4, 2 and 1. Add the ones that hold a 1."))
```

---

## Writing a number in binary

Take the largest place value that fits, put a 1 there, and go on with
what is left.

```example
(example
  (problem "Write 13 in binary, with 4 digits.")
  (step "The places are worth 8, 4, 2 and 1. 8 fits in 13: a 1 in the 8s place, and 5 is left.")
  (step "4 fits in 5: a 1 in the 4s place, and 1 is left.")
  (step "2 does not fit in 1: a 0 in the 2s place.")
  (step "1 fits: a 1 in the 1s place, and nothing is left.")
  (answer "13 is 1101 in binary."))
```

```check
(text
  (ask "Write 6 in binary, with 4 digits.")
  (accept "0110") (accept "110")
  (wrong-if "011" "That is 3, or 6 written backwards. The 4s place comes before the 2s place.")
  (wrong-if "0011" "0011 is 2 + 1 = 3. 6 is 4 + 2.")
  (hint "6 is 4 + 2. Which places are those?"))
```

---

## Odd and even

The rightmost bit is worth 1. It is the **least significant bit**; the
leftmost is the **most significant**. Every other place is worth an
even number, so a number is odd exactly when its rightmost bit is 1.

```check
(choice
  (ask "Is the binary number 10010110 odd or even?")
  (right "Even")
  (wrong "Odd"
    "Only the rightmost bit can make a number odd, because every other place is worth an even number. Here it is 0.")
  (hint "Look at the rightmost bit only."))
```

---

## Shifting

Move every bit one place to the left, and put a 0 on the right: each 1
is now worth twice as much, so the number **doubles**. Moving every bit
one place right halves it, and the rightmost bit falls off.

```check
(number
  (ask "0011 is 3. Shift every bit one place left: 0110. What number is that?")
  (answer 6) (from "3 * 2")
  (wrong-if 4 "0110 holds a 1 in the 4s place and in the 2s place: 4 + 2.")
  (hint "Add the places that hold a 1: 4 and 2."))
```

---

```check
(number
  (ask "What is the binary number 1000 1100 in decimal?")
  (answer 140) (from "128 + 8 + 4")
  (wrong-if 12 "That is only the right-hand four digits, 1100. The leftmost 1 is in the 128s place.")
  (hint "The places of 8 digits are worth 128, 64, 32, 16, 8, 4, 2 and 1."))
```

So: in binary each place is worth twice the one to its right, and a
number is the sum of the places that hold a 1.
