# Does it fit, and is it fast enough?

```lesson
(objective "Read a board's utilization and timing figures, and say whether there is room and margin.")
(minutes 15)
(keywords "utilization (FPGA)" "fmax (of a clock)")
```

Every build of the gateware on the larger computer reports two
numbers that decide whether it will work.

**Utilization** is how much of the chip the design uses: its LUTs,
flip-flops, block RAMs and multipliers, out of those there are. Above
about 75% of the LUTs, the tools have little room to place things, and
the clock gets harder to meet.

[docs:boards](docs:boards) keeps these figures for each board. Measured
at different times, with different tools:

```figure
(bar-chart '(("Lakritz" 78) ("ML1" 47) ("ULX3S" 22) ("ML0" 92)) 'warm)
(caption "% of LUTs used. ML0, ML1: Mozart; ULX3S: 85F")
```

```sources
(source "docs/boards.md, Utilisation; Mozart ML0 and Sergei ML0")
(source "Timeless Computing (docs/tc.md), Describing Hardware: Reading the Reports")
```

```check
(choice
  (ask "Which of these boards is the most comfortable for adding a block to the gateware?")
  (right "The ULX3S with an 85F, at 22%")
  (wrong "The Mozart ML0, at 92%"
    "92% is the fullest in the figure. There is almost no room left, and the tools struggle long before 100%.")
  (wrong "The Lakritz, at 78%"
    "78% is already past the three-quarters line where placing and timing get hard.")
  (hint "Look for the shortest bar."))
```

---

## Timing

The tools also work out the slowest path through the logic between
two flip-flops, and from it the fastest clock the design can take: its
**fmax**. If fmax is below the clock the design runs at, it may still
seem to work, then fail when warm, or on another board.

For the Lakritz, docs/boards.md gives CLK_48, the 48 MHz clock that
runs the processor, an fmax of 53.88 MHz. The Mozart ML0 is the
tightest: 51.19 MHz.

```check
(number
  (ask "How many MHz of margin does the Lakritz's CLK_48 have over 48 MHz?")
  (answer 5.88) (within 0.01) (unit "MHz") (from "53.88 - 48")
  (wrong-if 101.88 "That adds the two. The margin is how far fmax is above the clock: subtract.")
  (hint "Subtract 48 from 53.88."))
```

---

## No timing from zfpga

The usual open tools, on the larger computer, report fmax. zfpga does
not: it has no timing analysis at all. A design zfpga
builds either works at the clock you give it or it does not, and zfpga
will not tell you which. For small designs, such as a counter, that is
rarely a problem; it is one more reason the SOC is built elsewhere.

```sources
(source "docs/zfpga.md, section 1 (non-goals) and 23.3")
```

```check
(choice
  (ask "You build a design with zfpga. How do you learn its fmax?")
  (right "You cannot from zfpga: it does no timing analysis")
  (wrong "From the line zfpga prints after routing"
    "That line counts the connections it routed. zfpga has no model of delays, so it reports no fmax.")
  (wrong "From the info app"
    "info shows what the running gateware reports about itself, not the timing of a design you built.")
  (hint "zfpga's status says what it does not do."))
```

So: utilization says whether a design fits comfortably and fmax
whether it is fast enough; zfpga counts a design's LUTs and flip-flops,
but says nothing of its timing.
