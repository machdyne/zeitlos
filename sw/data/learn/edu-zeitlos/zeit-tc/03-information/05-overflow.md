# Overflow

```lesson
(objective "Predict what happens when a result is too big for its bits.")
(minutes 15)
(keywords "overflow" "wrap around" "integer")
```

A group of bits has a fixed range. Add 1 to the largest number it can
hold, and the answer does not get bigger: it goes back to the smallest
number. This is **overflow**, and the number is said to
**wrap around**, as a car's distance meter goes back to 0 after its
highest reading.

In 8 bits unsigned, 255 + 1 needs a ninth bit, 1 0000 0000. There is
no ninth place, so that bit is lost and 0000 0000 is left.

Move the slider through the top of the range.

```figure
(define n (slider 'n 250 260 250 "Count" 1))
(define v (modulo n 256))
(bit-boxes 32 60 8 v 32 #t #f)
(text 160 140 (string-append "the true count: " (fmt n 0)) 'centre 'body)
(text 160 175 (string-append "what 8 bits hold: " (fmt v 0)) 'centre 'body 'bold)
```

```sources
(source "Timeless Computing (docs/tc.md), Information: Overflow")
```

```check
(number
  (ask "An 8-bit unsigned number holds 255. What does it hold after adding 2?")
  (answer 1) (from "(255 + 2) - 256")
  (wrong-if 257 "257 does not fit in 8 bits: the largest is 255. The extra bit is lost, so the number wraps around.")
  (wrong-if 255 "It does not stop at the top. Adding 1 gives 0, and adding 1 more gives 1.")
  (hint "255 + 1 wraps to 0. Then add 1 more."))
```

---

## Signed overflow

Signed numbers wrap too, but from the largest positive to the most
negative. In 8 bits, 127 is 0111 1111; add 1 and you get 1000 0000,
which reads as -128.

```check
(number
  (ask "A signed 8-bit number holds 127. What does it read as after adding 1?")
  (answer -128)
  (wrong-if 128 "128 does not fit in a signed byte. 127 + 1 is 1000 0000, and its leftmost bit is worth -128.")
  (wrong-if 0 "Unsigned 255 wraps to 0. Signed 127 is in the middle of the bit patterns: one more is 1000 0000.")
  (hint "Write 127 + 1 in binary, then read it as signed."))
```

---

## In BASIC

In Machdyne BASIC, every number is a 16-bit signed **integer** (a whole
number), so the
range is -32768 to 32767, and results outside it wrap around
([basic](docs:basic_app)). Larger languages use more bits, which raises
the limit but does not remove it.

```try
(try (app "basic")
  (task "Open BASIC with the button below. BASIC runs only numbered lines, so type NEW, then 10 PRINT 32767 + 1, then RUN, pressing Enter after each. Then come back.")
  (check (number
    (ask "What did BASIC print?")
    (answer -32768)
    (wrong-if 32768 "32768 does not fit in 16 signed bits: the largest is 32767. One more wraps to the most negative number.")
    (hint "The 16-bit range is -32768 to 32767."))))
```

```sources
(source "Timeless Computing (docs/tc.md), BASIC; Zeitlos docs: basic_app.md")
```

---

## Practice

```check
(number
  (ask "In BASIC, 300 * 300 is 90000, which does not fit in 16 bits. 65536 too much is thrown away. What does BASIC give?")
  (answer 24464) (from "90000 - 65536")
  (wrong-if 90000 "90000 is above 32767, the largest 16-bit signed number, so it cannot be held.")
  (wrong-if 32767 "Overflow does not stop at the largest number. The extra bits are thrown away, which takes off 65536.")
  (hint "90000 - 65536."))
```

---

```check
(choice
  (ask "A program counts visitors in an unsigned byte. What goes wrong after the 255th visitor?")
  (right "The next visitor makes the count 0")
  (wrong "The count stops at 255"
    "The bits do not know that a limit was reached. 255 + 1 drops the ninth bit and leaves 0.")
  (wrong "The computer reports an error"
    "Adding in bits gives no error by itself: the extra bit is simply lost. A program must check for overflow itself.")
  (hint "What is 255 + 1 in 8 bits?"))
```

So: a result too big for its bits loses its top bits and wraps around,
so a program must know how large its numbers can get.
