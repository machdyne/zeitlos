# Look-up tables

```lesson
(objective "Work out the 16-bit INIT that makes an ECP5 look-up table compute a function.")
(minutes 20)
(keywords "look-up table (FPGA)" "LUT (FPGA)" "INIT (of a LUT)" "slice (of an FPGA)" "tile (of an FPGA)" "row (of tiles)" "column (of tiles)")
```

The logic in an FPGA is not made of fixed gates. It is made of
**look-up tables**, or **LUTs**: tiny memories that stand in for any
gate.

A LUT on the ECP5 has 4 inputs, called a, b, c and d, and one output.
The four inputs make an address from 0 to 15, and the LUT holds one
bit for each address: 16 bits in all. The output is the bit at the
address the inputs make.

A truth table with four inputs has 16 lines, so a LUT can hold any
truth table of four inputs. Load it with the right 16 bits, and it is
an AND gate, an XOR gate, or anything else with up to four inputs.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Look-Up Tables")
(source "docs/zfpga.md, section 5.1")
```

```check
(choice
  (ask "How many different bits does an ECP5 LUT hold?")
  (right "16, one for each pattern of its 4 inputs" (true "2^4 = 16"))
  (wrong "4, one for each input"
    "The inputs choose which bit comes out. There is one stored bit for each pattern of the inputs, and 4 inputs make 2^4 patterns.")
  (wrong "1, its output"
    "The output is one bit, chosen from many stored ones. That choosing is what lets a LUT be any gate.")
  (hint "How many patterns can 4 bits make?"))
```

---

## INIT

zfpga, like the other ECP5 tools, writes a LUT's 16 bits as one
hexadecimal number called **INIT**. Bit *i* of INIT is the output for
address *i*, where a is bit 0 of the address, b bit 1, c bit 2 and d
bit 3.

When only a and b are used, c and d are 0 and the first four
addresses are all that matter. The same four bits are then repeated in
each hex digit of INIT.

Set the output for each of the four addresses with the toggles, and watch
INIT.

```figure
(define o0 (if (toggle 'o0 "address 0") 1 0))
(define o1 (if (toggle 'o1 "address 1" #t) 1 0))
(define o2 (if (toggle 'o2 "address 2" #t) 1 0))
(define o3 (if (toggle 'o3 "address 3") 1 0))
(define outs (list o0 o1 o2 o3))
(define d (+ o0 (* 2 o1) (* 4 o2) (* 8 o3)))
(text 60 24 "address" 'centre 'small 'bold)
(text 130 24 "b" 'centre 'small 'bold)
(text 170 24 "a" 'centre 'small 'bold)
(text 240 24 "out" 'centre 'small 'bold)
(line 30 32 280 32 'ink 1)
(for-range 0 4
  (lambda (i)
    (let ((y (+ 52 (* 22 i))) (o (list-ref outs i)))
      (text 60 y (fmt i 0) 'centre 'body)
      (text 130 y (if (>= i 2) "1" "0") 'centre 'body)
      (text 170 y (if (odd? i) "1" "0") 'centre 'body)
      (if (= o 1) (rect 226 (- y 14) 28 19 'ink))
      (text 240 y (if (= o 1) "1" "0") 'centre 'body (if (= o 1) 'paper 'ink)))))
(text 160 158 (string-append "bits 3 2 1 0 of INIT: "
  (fmt o3 0) " " (fmt o2 0) " " (fmt o1 0) " " (fmt o0 0)
  ", hex " (fp-hex d)) 'centre 'small)
(text 160 200 (string-append "INIT = 0x" (fp-hex d) (fp-hex d) (fp-hex d) (fp-hex d)) 'centre 'big 'bold)
```

```check
(text
  (ask "With the toggles at their start, the LUT is an XOR of a and b, and INIT is 0x6666. What is INIT for a AND b?")
  (accept "0x8888") (accept "8888") (accept "8888h")
  (wrong-if "0x1111" "That puts the 1 at address 0, where a and b are both 0. AND is 1 only at address 3, where both are 1: bit 3, which is worth 8 in a hex digit.")
  (wrong-if "1111" "That puts the 1 at address 0, where a and b are both 0. AND is 1 only at address 3, where both are 1: bit 3, which is worth 8 in a hex digit.")
  (hint "Set the toggles: only address 3 has both a and b at 1.")
  (hint "Bits 3 2 1 0 are 1 0 0 0. What hex digit is binary 1000?"))
```

---

## Writing the function instead

Working out INIT by hand is easy to get wrong, so zfpga lets you write
the function itself. In a netlist file it accepts:

```text
lut x1 func=a^b a=q1 b=q0 z=d1
```

`func=a^b` says the output is a XOR b; zfpga works out INIT, here
0x6666. In a `func`, `~` is NOT, `&` is AND, `^` is XOR and `|` is OR.
You will meet these files again in the last unit.

```sources
(source "docs/zfpga-formats.md, section 2.2")
```

```check
(text
  (ask "What INIT does zfpga work out for func=a, a LUT whose output is just its input a?")
  (accept "0xAAAA") (accept "AAAA") (accept "aaaa") (accept "0xaaaa")
  (wrong-if "0x5555" "0x5555 is NOT a: a 1 where a is 0. The output equals a at the odd addresses, where a, bit 0 of the address, is 1.")
  (wrong-if "5555" "5555 is NOT a: a 1 where a is 0. The output equals a at the odd addresses, where a, bit 0 of the address, is 1.")
  (hint "a is bit 0 of the address, so it is 1 at addresses 1 and 3.")
  (hint "Bits 3 2 1 0 are 1 0 1 0. That is the hex digit A."))
```

---

```example
(example
  (problem "Work out INIT for a OR b.")
  (step "OR is 1 unless both inputs are 0. So the outputs at addresses 0, 1, 2, 3 are 0, 1, 1, 1.")
  (step "Written as bits 3 2 1 0, that is 1 1 1 0.")
  (step "Binary 1110 is 14, the hex digit E.")
  (answer "Repeated in all four hex digits: INIT = 0xEEEE."))
```

```check
(text
  (ask "Now work out INIT for NOT a (func=~a).")
  (accept "0x5555") (accept "5555")
  (wrong-if "0xAAAA" "That is a itself. NOT a is 1 where a is 0: at the even addresses, 0 and 2.")
  (wrong-if "AAAA" "That is a itself. NOT a is 1 where a is 0: at the even addresses, 0 and 2.")
  (hint "NOT a is 1 at addresses 0 and 2.")
  (hint "Bits 3 2 1 0 are 0 1 0 1."))
```

---

## Slices and tiles

LUTs come in pairs. On the ECP5, two LUTs and two flip-flops make a
**slice**, so the output of a LUT can be stored at a clock edge
without leaving the slice. Four slices, called A, B, C and D, make a
logic **tile**: 8 LUTs and 8 flip-flops.

Tiles sit in a grid, and each is named by its **row** and **column**
in it: `R2C4` is the tile in row 2, column 4. You will see these names
in zfpga's files.

An LFE5U-25F, the ECP5 in a Lakritz, has 3,036 logic tiles.

```sources
(source "docs/zfpga.md, sections 2.4 and 5.1")
```

```check
(number
  (ask "How many LUTs are there in 3,036 logic tiles?")
  (answer 24288) (from "3036 * 8")
  (wrong-if 12144 "That counts 4 LUTs a tile: one a slice. Each slice has two, and a tile has four slices.")
  (wrong-if 6072 "That counts 2 LUTs a tile. A tile has four slices of two LUTs each.")
  (hint "Each tile has 4 slices, and each slice 2 LUTs."))
```

So: an ECP5 LUT is a 16-bit memory addressed by its four inputs, and
its INIT is the truth table, written in hex.
