# Adding with gates

```lesson
(objective "Add binary numbers with half adders and full adders, and explain how the same adder subtracts.")
(minutes 25)
(keywords "half adder" "full adder" "ripple-carry adder" "arithmetic logic unit" "ALU" "propagation delay" "nanosecond")
```

Adding two single bits has four cases: 0 + 0 = 0, 0 + 1 = 1,
1 + 0 = 1, and 1 + 1 = 2, which is 10 in binary. So the answer needs
two bits: a sum bit, and a **carry** into the next place, as when 5 + 7
in decimal writes 2 and carries 1.

- The sum bit is 1 when exactly one input is 1: that is A XOR B.
- The carry is 1 only when both are 1: that is A AND B.

These two gates together are a **half adder**.

| A | B | Sum (A ^ B) | Carry (A & B) |
|---|---|---|---|
| 0 | 0 | 0 | 0 |
| 0 | 1 | 1 | 0 |
| 1 | 0 | 1 | 0 |
| 1 | 1 | 0 | 1 |

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: Combinational Circuits")
```

```check
(choice
  (ask "A half adder adds A = 1 and B = 1. What are its outputs?")
  (right "Sum 0, carry 1")
  (wrong "Sum 1, carry 0"
    "1 + 1 is 2, which is 10 in binary: a 0 in this place and a 1 carried to the next. Sum 1 with no carry would mean the answer is 1.")
  (wrong "Sum 1, carry 1"
    "That is 11 in binary, which is 3. 1 + 1 is only 2, which is 10.")
  (hint "1 + 1 = 2. Write 2 in binary with two digits."))
```

---

## The full adder

To add numbers of many bits, each place must also add the carry from
the place to its right. A **full adder** has three inputs: A, B and a
carry in. It gives a sum bit and a carry out:

```text
sum       = A ^ B ^ carry_in
carry_out = (A & B) | (carry_in & (A ^ B))
```

```check
(choice
  (ask "A full adder has A = 1, B = 1 and carry in = 1. What are its outputs?")
  (right "Sum 1, carry out 1")
  (wrong "Sum 0, carry out 1"
    "That is 1 + 1 = 2 without the carry in. With it, 1 + 1 + 1 = 3, which is 11 in binary.")
  (wrong "Sum 1, carry out 0"
    "1 + 1 + 1 is 3, which is 11 in binary: both bits are 1.")
  (hint "Add the three bits as numbers, then write the total in binary with two digits."))
```

---

## Adding whole numbers

Full adders in a row, each one's carry out going into the next one's
carry in, add numbers of any width. The carry passes along the row
like a ripple, so this is a **ripple-carry adder**.

Move the sliders. The small numbers on top are the carries into each
place; the box on the far left of the sum is the last carry out.

```figure
(define a (slider 'a 0 15 5 "A" 1))
(define b (slider 'b 0 15 3 "B" 1))
(define (c i) (if (= i 0) 0 (if (>= (+ (modulo a (expt 2 i)) (modulo b (expt 2 i))) (expt 2 i)) 1 0)))
(define s (+ a b))
(text 60 32 "carry" 'right 'small)
(for-range 0 4 (lambda (k) (text (+ 116 (* (- 3 k) 32)) 32 (fmt (c k) 0) 'centre 'small)))
(text 60 72 "A" 'right 'body 'bold)
(bit-boxes 100 50 4 a 32 #f #f)
(text 60 112 "B" 'right 'body 'bold)
(bit-boxes 100 90 4 b 32 #f #f)
(line 60 132 236 132 'ink 1.5)
(text 60 162 "sum" 'right 'body 'bold)
(bit-boxes 68 140 5 s 32 #f #f)
(text 250 72 (fmt a 0) 'left 'body)
(text 250 112 (fmt b 0) 'left 'body)
(text 250 162 (fmt s 0) 'left 'body 'bold)
(caption (string-append (fmt a 0) " + " (fmt b 0) " = " (fmt s 0)))
```

```check
(text
  (ask "Add the binary numbers 0101 and 0011. Give the 4-bit sum.")
  (accept "1000")
  (wrong-if "0110" "That adds each place without carrying. 1 + 1 in the rightmost place is 10: write 0 and carry 1 to the next place, and so on.")
  (wrong-if "0111" "That is 0101 OR 0011: every place with a 1 in either. Adding must carry: 1 + 1 makes 0 and a carry.")
  (hint "0101 is 5 and 0011 is 3. What is 8 in binary?"))
```

---

## Subtracting with the same adder

In two's complement, a number is negated by turning over every bit
and adding one. So A - B is A + (NOT B) + 1.

An XOR gate on each bit of B turns it over when its other input is 1.
Set the adder's first carry in to 1, and it adds the 1. One control
bit turns the adder into a subtractor. Tick Subtract to see it; the
carry out is then thrown away.

```figure
(define (il-signed n digits)
  (if (= 1 (bit n (- digits 1))) (- n (expt 2 digits)) n))
(define a (slider 'a 0 7 7 "A" 1))
(define b (slider 'b 0 7 3 "B" 1))
(define sub (if (toggle 'sub "Subtract") 1 0))
(define bb (if (= sub 1) (- 15 b) b))
(define (c i) (if (>= (+ (modulo a (expt 2 i)) (modulo bb (expt 2 i)) sub) (expt 2 i)) 1 0))
(define s (modulo (+ a bb sub) 16))
(text 60 32 "carry" 'right 'small)
(for-range 0 4 (lambda (k) (text (+ 116 (* (- 3 k) 32)) 32 (fmt (if (= k 0) sub (c k)) 0) 'centre 'small)))
(text 60 72 "A" 'right 'body 'bold)
(bit-boxes 100 50 4 a 32 #f #f)
(text 60 112 (if (= sub 1) "NOT B" "B") 'right 'body 'bold)
(bit-boxes 100 90 4 bb 32 #f #f)
(line 60 132 236 132 'ink 1.5)
(text 60 162 "result" 'right 'body 'bold)
(bit-boxes 100 140 4 s 32 #f #f)
(define r (if (= sub 1) (il-signed s 4) s))
(text 250 162 (fmt r 0) 'left 'body 'bold)
(caption (string-append (fmt a 0) (if (= sub 1) " - " " + ") (fmt b 0) " = " (fmt r 0)))
```

```check
(text
  (ask "With 4 bits: 0110 - 0010. Turn over 0010 and add with a carry in of 1. What 4-bit result do you get?")
  (accept "0100") (accept "100")
  (wrong-if "1000" "That adds 0110 and 0010. To subtract, turn over every bit of 0010 first, to 1101.")
  (hint "0110 + 1101 + 1. Keep only the 4 lowest bits.")
  (hint "6 - 2 is 4. What is 4 in binary?"))
```

---

## The ALU

An adder that can also subtract, AND, OR and compare, with a
multiplexer to choose which result comes out, is an **arithmetic logic
unit**, or **ALU**. It is the part of a processor that calculates.

```check
(choice
  (ask "In an ALU, what chooses which of its results comes out?")
  (right "A multiplexer")
  (wrong "A decoder"
    "A decoder turns a number into one active line. Passing one of several results on is what a multiplexer does.")
  (wrong "The adder"
    "The adder makes one of the results. Something else must choose between it and the AND, OR and other results.")
  (hint "Which circuit chooses one of its inputs?"))
```

---

## Propagation delay

A gate's output does not change at the instant its input does: it
changes a short time after, typically a fraction of a **nanosecond** (ns),
a billionth of a second, in a modern chip. This time is the gate's
**propagation delay**.

Delays add up along a chain of gates. In a ripple-carry adder, a carry may have
to pass through every full adder before the top bit of the sum is
right. Until then, the outputs are briefly wrong. So a 32-bit adder is
slower than a 4-bit one, and faster adders use extra logic to work
out the carries side by side.

```check
(number
  (ask "In a ripple-carry adder, each full adder takes 2 ns to pass its carry on. At worst, how long does a carry take to pass through 8 full adders, in ns?")
  (answer 16) (unit "ns") (from "8 * 2")
  (wrong-if 2 "That is one full adder. The carry can pass through all 8, one after another, and the delays add up.")
  (wrong-if 4 "The delays add, rather than doubling once: 2 ns for each of the 8 full adders.")
  (hint "Delays along a chain add up: 2 ns, 8 times."))
```

---

```check
(number
  (ask "A ripple-carry adder for 32-bit numbers. How many full adders does it have?")
  (answer 32)
  (wrong-if 64 "Each full adder adds one bit of A and one bit of B together: one for each place, not one for each bit of each number.")
  (wrong-if 16 "Each full adder handles one place, and there are 32 places.")
  (hint "One full adder for each place."))
```

So: a full adder adds two bits and a carry, a row of them adds whole
numbers, the same row subtracts with B turned over, and the carry
takes time to ripple through.
