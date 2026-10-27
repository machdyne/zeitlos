# Bytes and hexadecimal

```lesson
(objective "Say what a byte holds, and write a byte in hexadecimal.")
(minutes 15)
(keywords "byte" "nibble" "word (of bits)" "hexadecimal")
```

Bits are handled in groups. A group of 8 bits is a **byte**. Its 8 bits
make 256 patterns, so a byte holds a number from 0 to 255. Half a byte,
4 bits, is a **nibble**.

```sources
(source "Timeless Computing (docs/tc.md), Information: Bytes and Words; Hexadecimal")
```

```check
(number
  (ask "What is the largest number a byte can hold?")
  (answer 255) (from "2^8 - 1")
  (wrong-if 256 "A byte has 256 patterns, but the first is 0. So they are the numbers 0 to 255.")
  (wrong-if 8 "8 is how many bits a byte has. The largest number is when all 8 bits are 1.")
  (hint "All 8 bits are 1: 128 + 64 + 32 + 16 + 8 + 4 + 2 + 1."))
```

---

## Words

A processor works best with one size of group, its **word**. The
processors in Zeitlos computers have 32-bit words; small, simple
processors use 8 or 16 bits. The size sets the range:

| Bits | Patterns | Numbers |
|---|---|---|
| 8 | 256 | 0 to 255 |
| 16 | 65,536 | 0 to 65,535 |
| 32 | 4,294,967,296 | 0 to about 4.29 billion |

```check
(choice
  (ask "How many different numbers can 16 bits hold?")
  (right "65,536" (true "2^16 = 65536"))
  (wrong "65,535" (false "2^16 = 65535")
    "65,535 is the largest of them. Counting 0 too, there are 65,536.")
  (wrong "32" (false "2^16 = 32")
    "That would be 16 bits counted twice. Each bit doubles the count: 2 to the power 16.")
  (hint "16 bits make 2 to the power 16 patterns."))
```

---

## Hexadecimal

Long rows of 0s and 1s are hard to read and easy to copy wrongly.
**Hexadecimal**, or hex, is base 16. One hex digit stands for exactly
four bits. Its digits are 0 to 9, then A to F for ten to fifteen.

Move the slider: each nibble of the byte is one hex digit.

```figure
(define n (slider 'n 0 255 140 "Byte" 1))
(bit-boxes 28 50 8 n 32 #f #t)
(define hi (floor (/ n 16)))
(define lo (modulo n 16))
(text 92 116 (hex-digit hi) 'centre 24)
(text 228 116 (hex-digit lo) 'centre 24)
(text 92 136 (string-append "= " (fmt hi 0)) 'centre 'small)
(text 228 136 (string-append "= " (fmt lo 0)) 'centre 'small)
(text 160 176 (string-append "0x" (hex-digit hi) (hex-digit lo) " = " (fmt n 0)) 'centre 'body 'bold)
(caption "Each group of four bits is one hex digit.")
```

```check
(text
  (ask "The nibble 1010 is ten. Which hex digit is it?")
  (accept "A") (accept "0xA")
  (wrong-if "10" "10 is ten in decimal, two digits. Hex has one digit for each value up to fifteen: ten is A.")
  (hint "After 9 come A for ten, B for eleven, and so on."))
```

---

## A byte in hex

Split the byte into two nibbles from the right, and write each as its
hex digit. A byte is always exactly two hex digits. `0x` in front marks
a number as hex.

```example
(example
  (problem "Write the byte 1000 1100 in hex.")
  (step "The left nibble, 1000, is 8: hex digit 8.")
  (step "The right nibble, 1100, is 8 + 4 = 12: hex digit C.")
  (answer "1000 1100 is 0x8C."))
```

```check
(text
  (ask "Write the byte 1111 0000 in hex.")
  (accept "F0") (accept "0xF0")
  (wrong-if "0F" "The nibbles are in the wrong order. The left nibble, 1111, is the first hex digit.")
  (wrong-if "150" "That is 15 and 0 written as decimal. Fifteen is the hex digit F.")
  (hint "1111 is fifteen, and 0000 is zero. Fifteen is which hex digit?"))
```

---

```check
(number
  (ask "A 32-bit word, written in hex: how many hex digits is that?")
  (answer 8) (from "32 / 4")
  (wrong-if 4 "4 is how many bits each hex digit stands for. 32 bits are 32 / 4 digits.")
  (wrong-if 32 "That is one digit for each bit. One hex digit stands for four bits.")
  (hint "Each hex digit is 4 bits."))
```

---

```check
(number
  (ask "What is 0xFF in decimal?")
  (answer 255) (from "15 * 16 + 15")
  (wrong-if 30 "That is F + F, fifteen and fifteen added. The left digit is worth sixteen times as much.")
  (hint "The left digit counts sixteens. F is fifteen: 15 x 16 + 15."))
```

So: a byte is 8 bits, 0 to 255, and it is written as two hex digits,
one for each nibble.
