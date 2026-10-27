# Choosing and selecting

```lesson
(objective "Say what a multiplexer, a decoder and a comparator do, and work out their outputs.")
(minutes 20)
(keywords "combinational (circuit)" "multiplexer" "select input" "decoder" "comparator")
```

Gates joined together make circuits that do useful jobs. A circuit
whose outputs depend only on its inputs as they are now is
**combinational**: change the inputs, and the outputs follow. Three
combinational circuits appear everywhere in a computer.

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: Combinational Circuits")
```

```check
(choice
  (ask "An AND gate's output depends only on its inputs as they are now. Is it combinational?")
  (right "Yes")
  (wrong "No"
    "Give an AND gate the same inputs and it always gives the same output, whatever came before. That is what combinational means.")
  (hint "Does the AND gate remember anything?"))
```

---

## The multiplexer

A **multiplexer** chooses one of its inputs and passes it to its
output. A 2-to-1 multiplexer has data inputs A and B and a
**select input** S: when S is 0, the output is A; when S is 1, it is B.

In Boolean algebra:

```text
out = (A & ~S) | (B & S)
```

The line inside the figure shows which input is passed through.

```figure
(define (il-wire x0 y0 x1 y1 v)
  (line x0 y0 x1 y1 'ink (if (= v 1) 3 1)))
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define a (if (toggle 'a "A") 1 0))
(define b (if (toggle 'b "B") 1 0))
(define s (if (toggle 's "S (select)") 1 0))
(define out (if (= s 0) a b))
(path '(120 40 180 70 180 150 120 180 120 40) 'ink 1.5)
(text 60 74 "A" 'body 'bold)
(text 60 154 "B" 'body 'bold)
(il-wire 74 70 120 70 a)
(il-wire 74 150 120 150 b)
(text 132 74 "0" 'small)
(text 132 154 "1" 'small)
(line 126 (if (= s 0) 70 150) 180 110 'ink 1)
(il-wire 150 220 150 166 s)
(text 150 236 "S" 'centre 'body 'bold)
(il-wire 180 110 250 110 out)
(il-lamp 262 110 out)
(text 262 145 (fmt out 0) 'centre 'big)
```

```check
(choice
  (ask "A 2-to-1 multiplexer has A = 0, B = 1 and S = 1. What is its output?")
  (right "1")
  (wrong "0"
    "S = 1 selects B, not A. B is 1, so the output is 1.")
  (hint "S = 0 passes A, S = 1 passes B."))
```

---

## More inputs

With 2 select bits, a multiplexer chooses among 4 inputs; with 3,
among 8. A processor uses multiplexers to choose which number goes into its
arithmetic, and which result is kept.

```check
(number
  (ask "How many select bits does a multiplexer need to choose among 16 inputs?")
  (answer 4) (true "2^4 = 16")
  (wrong-if 16 "That is one bit for each input. The select bits form a number, and 4 bits make 16 patterns.")
  (wrong-if 8 "8 select bits would make 256 patterns. Each select bit doubles the number of inputs: 2, 4, 8, 16.")
  (hint "n select bits make 2 to the power n patterns. Which n gives 16?"))
```

---

## The decoder

A **decoder** turns a binary number into one active line. A 2-to-4
decoder has two inputs and four outputs, numbered 0 to 3. Exactly one
output is 1: the one whose number the inputs spell in binary.

A computer uses decoders to pick one memory chip or one device out of
many, by its number.

```figure
(define (il-wire x0 y0 x1 y1 v)
  (line x0 y0 x1 y1 'ink (if (= v 1) 3 1)))
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define i1 (if (toggle 'i1 "Bit 1") 1 0))
(define i0 (if (toggle 'i0 "Bit 0") 1 0))
(define n (+ (* 2 i1) i0))
(frame 110 30 80 170 'ink 1.5)
(text 150 120 "2-to-4" 'centre 'small)
(text 32 84 "bit 1" 'small)
(text 32 154 "bit 0" 'small)
(il-wire 70 80 110 80 i1)
(il-wire 70 150 110 150 i0)
(for-range 0 4
  (lambda (k)
    (let ((y (+ 50 (* k 40))) (v (if (= k n) 1 0)))
      (il-wire 190 y 250 y v)
      (text 200 (- y 6) (fmt k 0) 'small)
      (il-lamp 262 y v))))
(caption (string-append "Inputs " (fmt i1 0) (fmt i0 0) " = " (fmt n 0) ": output " (fmt n 0) " is 1"))
```

```check
(number
  (ask "A 2-to-4 decoder's inputs are bit 1 = 1 and bit 0 = 0. Which output is 1?")
  (answer 2) (from "2*1 + 0")
  (wrong-if 1 "Output 1 would need the inputs 01. Here the 1 is in bit 1, which is worth 2.")
  (wrong-if 10 "10 is the inputs read as a decimal number. Read as binary, 10 is 2.")
  (hint "Read the two input bits as a binary number."))
```

---

## The comparator

An equality **comparator** tells whether two numbers are equal. It
compares each pair of bits in the same place with an XNOR gate, which
gives 1 when the two bits are the same. Then one AND gate joins all
the results: the numbers are equal only if every pair matches.

```check
(number
  (ask "A comparator for two 8-bit numbers. How many XNOR gates does it use?")
  (answer 8)
  (wrong-if 16 "There is one XNOR for each pair of bits, not for each bit. 8-bit numbers make 8 pairs.")
  (wrong-if 1 "One XNOR compares only one pair of bits. Every place needs its own.")
  (hint "One XNOR for each place: how many places has an 8-bit number?"))
```

---

```check
(choice
  (ask "A comparator compares 1011 with 1001. What does it output?")
  (right "0: the numbers are not equal")
  (wrong "1: the numbers are equal"
    "Three of the four pairs match, but the AND at the end needs every pair to match. The second bit from the right differs.")
  (hint "Compare the bits place by place. Does any pair differ?"))
```

---

```check
(choice
  (ask "Which circuit would you use to pass on one of four numbers, chosen by a 2-bit number?")
  (right "A multiplexer")
  (wrong "A decoder"
    "A decoder turns a number into one active line; it does not pass a number through. Choosing which input goes through is a multiplexer's job.")
  (wrong "A comparator"
    "A comparator only says whether two numbers are equal. It does not pass either of them on.")
  (hint "Which circuit chooses one of its inputs?"))
```

So: a multiplexer chooses an input, a decoder turns a number into one
active line, and a comparator tells whether two numbers are equal.
