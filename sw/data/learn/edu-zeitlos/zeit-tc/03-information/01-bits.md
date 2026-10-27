# Bits

```lesson
(objective "Say how many different things a group of bits can stand for.")
(minutes 10)
(keywords "bit" "binary digit")
```

As the Electricity unit showed, a wire in a computer is either high or
low: its voltage is near the supply, or near 0 volts. Everything a
computer holds -- numbers, letters, pictures, programs -- is made of
patterns of those two states.

One high-or-low value is a **bit**, short for **binary digit**. It is
written 1 or 0. One bit can answer one yes-or-no question: is the light
on? is the key pressed?

```sources
(source "Timeless Computing (docs/tc.md), Information: Bits")
```

```check
(choice
  (ask "Why do computers use two states in a wire, not ten?")
  (right "Two states are the easiest to tell apart reliably")
  (wrong "A wire can only carry two voltages"
    "A wire can carry any voltage. Two are used because high and low are far apart, so a little noise cannot turn one into the other.")
  (wrong "Ten states would need ten wires"
    "Ten levels could share one wire. The trouble is telling ten levels apart reliably when every voltage wobbles a little.")
  (hint "Think about a voltage that wobbles a little. With two levels, how far apart are they?"))
```

---

## Patterns

Two bits make four patterns: 00, 01, 10 and 11. Three bits make eight.
Each extra bit **doubles** the number of patterns, so n bits make
$2^n$ patterns.

Move the slider to see every pattern of 1 to 5 bits.

```figure
(define n (slider 'bits 1 5 2 "Bits" 1))
(define count (expt 2 n))
(define cols (if (> count 16) 4 (if (> count 8) 4 (if (> count 2) 2 1))))
(define rows (/ count cols))
(define cw 70)
(define rh (if (> rows 4) 18 24))
(define x0 (- 160 (/ (* cols cw) 2)))
(for-range 0 count
  (lambda (k)
    (text (+ x0 (* (floor (/ k rows)) cw) (/ cw 2)) (+ 40 (* (modulo k rows) rh))
          (bin k n) 'centre 'body)))
(caption (string-append (fmt n 0) (if (= n 1) " bit: " " bits: ") (fmt count 0) " patterns"))
```

```check
(number
  (ask "How many different patterns can 4 bits make?")
  (answer 16) (from "2^4")
  (wrong-if 8 "8 is what 3 bits make. One more bit doubles it.")
  (wrong-if 4 "That is 4 patterns, the number for 2 bits. Each extra bit doubles the count.")
  (hint "Start from 1 bit, 2 patterns, and double once for each bit you add."))
```

---

## Enough bits

To give each of a set of things its own pattern, you need enough bits
for at least that many patterns.

```check
(number
  (ask "There are 26 letters. How many bits do you need to give each one a pattern of its own?")
  (answer 5)
  (true "2^4 < 26") (true "2^5 >= 26")
  (wrong-if 4 "4 bits make only 16 patterns, so 10 letters would have none.")
  (wrong-if 26 "26 bits would be one bit for each letter. Patterns count for more: each extra bit doubles the number.")
  (hint "Double from 2: 2, 4, 8, 16, 32. Which is the first at least 26?"))
```

---

```check
(number
  (ask "A group of 8 bits. How many different patterns can it make?")
  (answer 256) (from "2^8")
  (wrong-if 16 "16 is what 4 bits make. 8 bits double that four more times.")
  (wrong-if 64 "64 is 6 bits. Two more bits double it twice.")
  (hint "2 to the power 8: double 2 seven times."))
```

So: one bit is a 1 or a 0, and n bits make $2^n$ patterns.
