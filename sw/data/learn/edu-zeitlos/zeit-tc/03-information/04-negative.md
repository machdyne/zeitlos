# Negative numbers

```lesson
(objective "Read and write negative numbers in 8-bit two's complement.")
(minutes 20)
(keywords "two's complement" "signed (number)" "unsigned (number)")
```

A group of bits has no minus sign of its own: it is only a pattern. To
hold negative numbers, nearly every computer agrees on one rule,
called **two's complement**: the most significant bit counts as
negative.

In 8 bits, the leftmost place is worth -128 instead of +128. The
other places are worth what they always were.

Read this way, a number is **signed**; read with every place
positive, as before, it is **unsigned**. Move the slider: the same 8
bits, read both ways.

```figure
(define n (slider 'n 0 255 200 "Bits" 1))
(define s (if (>= n 128) (- n 256) n))
(bit-boxes 32 60 8 n 32 #f #f)
(for-range 0 8 (lambda (k)
  (text (+ 48 (* 32 k)) 50 (list-ref '("-128" "64" "32" "16" "8" "4" "2" "1") k) 'centre 'small)))
(text 160 130 (string-append "as unsigned: " (fmt n 0)) 'centre 'body)
(text 160 165 (string-append "as signed: " (fmt s 0)) 'centre 'body 'bold)
(caption "Signed: the leftmost bit is worth -128.")
```

```sources
(source "Timeless Computing (docs/tc.md), Information: Negative Numbers")
```

```check
(number
  (ask "In 8-bit two's complement, what is 1000 0000?")
  (answer -128)
  (wrong-if 128 "128 is the unsigned reading. In two's complement the leftmost place is worth -128.")
  (wrong-if 0 "The leftmost bit is 1, so its place counts: -128, and nothing else is added.")
  (hint "Only the leftmost bit is 1, and it is worth -128."))
```

---

## Signed and unsigned

Signed or unsigned, the bits are the same; only the reading
differs.

So 8 bits hold 0 to 255 unsigned, or -128 to 127 signed. A signed
number is negative exactly when its leftmost bit is 1.

```check
(number
  (ask "What is the largest signed 8-bit number?")
  (answer 127) (from "64 + 32 + 16 + 8 + 4 + 2 + 1")
  (wrong-if 255 "255 is the largest unsigned number. In signed numbers the leftmost bit makes a number negative, so it must be 0: 0111 1111.")
  (wrong-if 128 "128 would need the leftmost bit, which is worth -128 in a signed number.")
  (hint "Leave the leftmost bit 0 and set all the others."))
```

---

## Reading a negative number

Add the places as usual, with -128 for the leftmost.

```example
(example
  (problem "Read 1111 1111 as a signed 8-bit number.")
  (step "The leftmost bit is worth -128.")
  (step "The other seven bits are 64 + 32 + 16 + 8 + 4 + 2 + 1 = 127.")
  (step "-128 + 127 = -1.")
  (answer "1111 1111 is -1."))
```

```check
(number
  (ask "What is 1111 1110 as a signed 8-bit number?")
  (answer -2) (from "-128 + 126")
  (wrong-if 254 "That is the unsigned reading. Signed, the leftmost bit is worth -128: -128 + 126.")
  (wrong-if -126 "That reads the leftmost bit as a minus sign in front of 126. In two's complement it is worth -128, and the other bits are added to it: -128 + 126 = -2.")
  (hint "The other seven bits make 126. Add that to -128."))
```

---

## Turning a number negative

To negate a two's complement number, turn over every bit, then add 1.

```example
(example
  (problem "Write -5 in 8 bits.")
  (step "5 is 0000 0101.")
  (step "Turn over every bit: 1111 1010.")
  (step "Add 1: 1111 1011.")
  (step "Check: -128 + 64 + 32 + 16 + 8 + 2 + 1 = -5.")
  (answer "-5 is 1111 1011."))
```

```check
(text
  (ask "Write -1 in 8-bit two's complement.")
  (accept "11111111") (accept "1111 1111")
  (wrong-if "10000001" "That is a sign bit in front of 1, but two's complement does not work that way: 1000 0001 is -128 + 1 = -127. Turn over 0000 0001, then add 1.")
  (wrong-if "11111110" "That is 0000 0001 turned over, which is -2. You still need to add 1.")
  (hint "1 is 0000 0001. Turn over every bit, then add 1."))
```

---

## Why this rule

With two's complement, adding needs no special case. The same adding
circuit gives the right answer for positive and negative numbers, and
subtracting is adding the negated number.

For example, 5 + (-3): 0000 0101 + 1111 1101 = 1 0000 0010. The ninth
bit does not fit in 8 bits and is dropped, leaving 0000 0010, which is
2.

```check
(choice
  (ask "Why do computers use two's complement for negative numbers?")
  (right "The same adding circuit works for negative numbers too")
  (wrong "It lets a byte hold larger numbers"
    "A byte still has 256 patterns. Two's complement only moves half of them below zero: -128 to 127 instead of 0 to 255.")
  (wrong "It is the only way to write a minus sign in bits"
    "There are other ways, such as using one bit only as a sign. Two's complement wins because addition and subtraction need no special case.")
  (hint "Think about what happens when you add 5 and -3."))
```

---

```check
(number
  (ask "What is 1000 0011 as a signed 8-bit number?")
  (answer -125) (from "-128 + 2 + 1")
  (wrong-if 131 "That is the unsigned reading. Signed, the leftmost bit is worth -128.")
  (wrong-if -3 "That reads the leftmost bit as a minus sign. In two's complement it is worth -128: -128 + 3.")
  (hint "-128 + 2 + 1."))
```

So: in two's complement the leftmost bit is worth minus its usual
value, and you negate a number by turning over its bits and adding 1.
