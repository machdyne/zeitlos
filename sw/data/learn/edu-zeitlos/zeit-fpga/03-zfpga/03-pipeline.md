# Four stages

```lesson
(objective "Name zfpga's four stages and the file each writes, and say where a build starts for each kind of file.")
(minutes 20)
(keywords "logical netlist (FPGA)" "physical netlist (FPGA)" "arc (of routing)" "net (connection)")
```

`zfpga build` runs four stages, each one a subcommand of its own:

1. **synth** reads Verilog (`.v`) and writes a **logical netlist**
   (`.zl`): LUTs, flip-flops and carry cells, joined by named **nets**
   (connections),
   with no place on the chip yet.
2. **place** gives each of them a site in a tile, and writes a
   **physical netlist** (`.zn`).
3. **pnr** routes every connection through the chip's wiring, and
   writes the tile settings (`.cfg`): each switch it sets is an
   **arc**, from one wire to another.
4. **pack** turns the tile settings into a bitstream (`.bit`).

`build` starts from the stage that the input's ending names. Choose an
input below.

```figure
(define k (choice 'start '(".v" ".zl" ".zn" ".cfg") "Input"))
(define names '(".v" ".zl" ".zn" ".cfg" ".bit"))
(define stages '("synth" "place" "pnr" "pack"))
(for-range 0 5
  (lambda (i)
    (let ((x (+ 8 (* 63 i))))
      (if (= i k) (fp-box-on x 30 48 34 (list-ref names i)) (fp-box x 30 48 34 (list-ref names i))))))
(for-range 0 4
  (lambda (i)
    (let ((x (+ 56 (* 63 i))))
      (if (>= i k)
          (begin (arrow x 47 (+ x 15) 47 'ink 2)
                 (text (+ x 7) (+ 92 (* 18 (modulo i 2))) (list-ref stages i) 'centre 'small 'bold))
          (text (+ x 7) (+ 92 (* 18 (modulo i 2))) (list-ref stages i) 'centre 'small 'grey)))))
(text 160 160 (string-append "runs " (fmt (- 4 k) 0) (if (= k 3) " stage" " stages")) 'centre 'body 'bold)
(text 160 185 "each stage's file is left beside the input" 'centre 'small)
```

```sources
(source "docs/zfpga-formats.md, sections 1 and 7; sw/apps/zfpga/build.c")
```

```check
(choice
  (ask "You give zfpga build a .zn file. Which stages run?")
  (right "pnr and pack")
  (wrong "All four"
    "A .zn is already placed. build starts from the stage that reads a .zn: pnr.")
  (wrong "Only pack"
    "pack reads a .cfg. A .zn still has to be routed first.")
  (hint "Choose .zn in the figure."))
```

---

## Each stage, checked

Every stage has a well-known open tool that does the same job on a
larger computer, and zfpga is tested against it:

| zfpga | does the job of | checked by |
|---|---|---|
| synth | yosys | simulating both netlists |
| place, pnr | nextpnr | a legality check, and simulating the result |
| pack | ecppack | the same bitstream, byte for byte |
| unpack | ecpunpack | the same text, line for line |

Because the files between stages are in the same formats those tools
use, a stage can be swapped for the other tool's to find which half of
a broken build is wrong.

```sources
(source "docs/toolchain.md, section 7; docs/zfpga.md, section 3.3")
```

```check
(choice
  (ask "zfpga pack and ecppack are given the same .cfg. What do they produce?")
  (right "The same bitstream, byte for byte")
  (wrong "Bitstreams that do the same thing but differ"
    "pack is tested for exact sameness: every byte of its output must match ecppack's.")
  (hint "The table says how pack is checked."))
```

---

## Stages one at a time

You can run the stages yourself, and look at or change each file in
between. The pins come from the constraints file you give `synth`:

```text
$ zfpga synth /home/blink.v -l /data/zfpga/boards/lakritz.lpf
$ zfpga place /home/blink.zl
$ zfpga pnr /home/blink.zn
$ zfpga pack /home/blink.cfg -c
```

Each writes the next file beside its input. Run each only when the
one before has finished.

```sources
(source "docs/zfpga.md, section 19; sw/apps/zfpga/main.c, usage()")
```

```check
(text
  (ask "Which subcommand reads a .zl file?")
  (accept "place") (accept "zfpga place")
  (wrong-if "pnr" "pnr reads the placed .zn. The .zl, with nothing placed yet, goes to place.")
  (wrong-if "synth" "synth writes the .zl. The next stage reads it.")
  (hint "The .zl is the logical netlist: nothing has a site yet."))
```

---

```check
(choice
  (ask "Which file has every LUT and flip-flop but no place on the chip for any of them?")
  (right "The .zl, the logical netlist")
  (wrong "The .zn"
    "The .zn is the physical netlist: placing has given every part its site.")
  (wrong "The .cfg"
    "The .cfg is tile settings after routing. Everything is placed and wired by then.")
  (hint "Which file comes before place?"))
```

So: synth writes the .zl, place the .zn, pnr the .cfg and pack the
.bit, and build starts from whichever you give it.
