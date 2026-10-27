# Change the blinky

```lesson
(objective "Copy the example design, change it, and build your own version.")
(minutes 20)
(keywords "working copy (FPGA)")
```

The examples in `/data/zfpga/examples` are yours to read, but keep
them as they are and change a copy. In posix, copy `blink.v` to your
own folder under a new name:

```text
$ cp /data/zfpga/examples/blink.v /home/slow.v
```

`/home/slow.v` is now your **working copy**. A build writes its files
beside the design, so they will be in `/home` too.

```sources
(source "docs/posix.md, The shell; docs/zfpga-formats.md, section 7")
```

```check
(choice
  (ask "After zfpga build /home/slow.v -b lakritz, where is the bitstream?")
  (right "/home/slow.bit")
  (wrong "/data/zfpga/examples/slow.bit"
    "Each stage writes beside its input. The input is in /home, so the outputs are too.")
  (wrong "/home/blink.bit"
    "The outputs take the input's name: slow.v gives slow.zl, slow.zn, slow.cfg and slow.bit.")
  (hint "Outputs go beside the input, with its name."))
```

---

## Slower

To blink at half the rate, make the counter one bit wider and take
the new top bit:

```verilog
module top(input CLK_48, output LED_B);
	reg [24:0] ctr;
	always @(posedge CLK_48) ctr <= ctr + 1;
	assign LED_B = ctr[24];
endmodule
```

Edit the file in the text editor: open `text` from the dock, press
Ctrl+O and open `/home/slow.v`, change `23` to `24` in both places,
and press Ctrl+S. (In posix, `vi /home/slow.v` does the same job, if
you know vi.)

The figure blinks an LED at the rate a counter's top bit would. Bit
24 is the slow version.

```figure
(define b (slider 'bit 23 27 24 "Top bit" 1))
(define half (/ (expt 2 b) 48000000))
(define on (= 1 (modulo (floor (/ (seconds) half)) 2)))
(still
  (text 160 30 (string-append "LED_B = ctr[" (fmt b 0) "]") 'centre 'body 'bold)
  (text 160 190 (string-append (fmt half 2) " s on, " (fmt half 2) " s off") 'centre 'body)
  (ring 160 110 40 'ink 2))
(if on (circle 160 110 36 'warm))
```

```sources
(source "docs/zfpga-test.md, section 4 (blinkh.v's rate)")
```

```check
(number
  (ask "How many flip-flops will the report give for slow.v?")
  (answer 25)
  (wrong-if 24 "That was blink.v. The counter in slow.v has 25 bits, ctr[24] to ctr[0].")
  (hint "reg [24:0] is bits 24 down to 0."))
```

---

```try
(try (app "term")
  (task "Make /home/slow.v as above, then in posix run: zfpga build /home/slow.v -b NAME, with your board's name. Wait for zfpga: wrote, then come back.")
  (check (number
    (ask "How many carry cells does the synthesised line report?")
    (answer 13) (true "12 * 2 < 25") (true "13 * 2 >= 25")
    (wrong-if 12 "12 carry cells add only 24 bits. Check that the file says reg [24:0] and was saved before the build.")
    (wrong-if 25 "25 is the number of flip-flops. The carry cells are in brackets at the end of the line.")
    (hint "Each carry cell adds two bits; 25 bits need one more than 12."))))
```

---

## A mistake on purpose

Change `ctr[24]` to `ctr[25]` and build again. The counter has no bit
25, and zfpga says so, with the file and the line:

```text
/home/slow.v:4: error: index 25 is outside [24:0]
```

When the build refuses, fix the line the message names, save, and
build again. That loop -- edit, build, read the message -- is most of
the work.

```check
(choice
  (ask "zfpga stops with an error naming /home/slow.v and a line number. What did it write?")
  (right "Nothing new: a refused build writes no files")
  (wrong "A bitstream with that part left out"
    "zfpga never writes a partly built design. A refusal writes nothing at all.")
  (wrong "A .zl, but no bitstream"
    "A refused synthesis leaves no .zl behind; the tests check for exactly this.")
  (hint "zfpga refuses rather than guesses."))
```

So: copy an example to /home, change it, build it with a whole path,
and read zfpga's message when it refuses.
