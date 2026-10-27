# Fractions

```lesson
(objective "Read a fixed-point number, and say what floating point does well and badly.")
(minutes 20)
(keywords "fixed point" "floating point" "scientific notation" "exponent")
```

Many quantities are not whole numbers. The simplest way to store them
is **fixed point**: whole numbers with an agreed scale. A price kept in
cents is fixed point: 1999 means 19.99. It is exact and fast, and it
is how small machines usually work with fractions.

```sources
(source "Timeless Computing (docs/tc.md), Information: Fractions")
```

```check
(number
  (ask "A program keeps prices in cents. A price is stored as 450. How many dollars is that?")
  (answer 4.5) (from "450 / 100") (unit "dollars")
  (wrong-if 450 "450 is the stored number. The agreed scale is cents, so divide by 100.")
  (wrong-if 45 "That divides by 10. There are 100 cents in a dollar.")
  (hint "The stored number counts cents. How many cents make a dollar?"))
```

---

## Places after the point

In binary, fixed point can give some bits to the fraction. The places
after the binary point are worth 1/2, 1/4, 1/8 and so on, each half
the one before, just as the places before it double.

Here 8 bits have 4 fraction bits, so the stored number is divided
by 16. Move the slider.

```figure
(define n (slider 'n 0 255 40 "Stored number" 1))
(bit-boxes 28 60 8 n 32 #f #t)
(for-range 0 8 (lambda (k)
  (text (+ 44 (* 32 k) (if (> k 3) 8 0)) 50 (list-ref '("8" "4" "2" "1" "1/2" "1/4" "1/8" "1/16") k) 'centre 'small)))
(circle 160 98 3 'ink)
(text 160 140 (string-append "stored: " (fmt n 0)) 'centre 'body)
(text 160 175 (string-append "means: " (fmt n 0) " / 16 = " (fmt (/ n 16) 4)) 'centre 'body 'bold)
(caption "The dot is the binary point.")
```

```check
(number
  (ask "With 4 fraction bits, what does the binary 0001.1000 mean?")
  (answer 1.5) (from "1 + 1/2")
  (wrong-if 24 "24 is the stored number without the scale. The four bits after the point are worth 1/2, 1/4, 1/8 and 1/16.")
  (wrong-if 1.1 "Binary places after the point are halves, not tenths. The first one is worth 1/2.")
  (hint "The 1 before the point is 1. The first place after it is worth 1/2."))
```

---

## Floating point

**Floating point** keeps a number as two parts, like **scientific
notation** (which writes 3,400 as $3.4 * 10^3$), but in binary: its significant digits, and an **exponent**, a
power of two that says where the point goes. One more bit gives the
sign. Nearly every computer uses the formats of a standard, IEEE 754:

| Format | Bits | Digits | Largest |
|---|---|---|---|
| single | 32 | about 7 | about 3.4 x 10^38 |
| double | 64 | about 16 | about 1.8 x 10^308 |

Floating point covers a huge range, from atoms to galaxies, with about
the same number of correct digits everywhere.

```check
(choice
  (ask "What does the exponent of a floating-point number say?")
  (right "Where the binary point goes")
  (wrong "How many digits the number has"
    "The number of digits is fixed by the format: about 7 for a single. The exponent moves the point, making the number larger or smaller.")
  (wrong "Whether the number is negative"
    "A separate bit holds the sign. The exponent is a power of two that places the point.")
  (hint "Think of 3.4 x 10^38: what does the 38 do?"))
```

---

## What floating point gets wrong

- **Most decimal fractions are not exact.** In binary, one tenth goes
  on for ever, as one third does in decimal. So 0.1 + 0.2 is not
  exactly 0.3 in floating point: compare such results by asking
  whether they are very close, not equal.
- **Whole numbers are exact only up to a limit.** A double holds every
  whole number up to $2^53$, about 9 million billion; a single only to
  about 16 million.

```check
(choice
  (ask "A program adds 0.1 and 0.2 in floating point and tests whether the result equals 0.3. What happens?")
  (right "The test fails: the result is very close to 0.3, but not exactly 0.3")
  (wrong "The test passes"
    "0.1 and 0.2 cannot be stored exactly in binary, as 1/3 cannot be written exactly in decimal. The small errors make the sum differ from 0.3 in its last bits.")
  (hint "Can one tenth be written exactly in binary?"))
```

---

## Which to use

The processors of Zeitlos computers have no floating-point hardware.
Each floating-point step is done in software, as many ordinary integer
steps, so it takes tens to hundreds of times as long as an integer
one. Fixed point suits money and small machines; floating point suits
measurements and science.

```check
(choice
  (ask "A shop's till, a small computer, keeps prices. Which is the better choice?")
  (right "Fixed point, in cents")
  (wrong "Floating point"
    "Money must be exact, and most amounts in dollars and cents, such as 0.10, cannot be held exactly in floating point. Whole cents are exact.")
  (hint "Must money be exact?"))
```

---

```check
(number
  (ask "With 4 fraction bits, the stored number is 40. What value does it mean?")
  (answer 2.5) (from "40 / 16")
  (wrong-if 40 "That is the stored number. With 4 fraction bits, the scale is 16.")
  (wrong-if 4 "That divides by 10. Four binary fraction bits mean dividing by 2 x 2 x 2 x 2 = 16.")
  (hint "Divide by 16."))
```

So: fixed point is a whole number with an agreed scale, exact and
fast; floating point covers a vast range but holds most fractions only
nearly.
