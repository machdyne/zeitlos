# The blitter

```lesson
(objective "Say what the Zeitlos blitter does, and work out the control word that starts a fill, a copy or a raster operation.")
(minutes 20)
(keywords "blitter" "raster operation" "glyph mode")
```

You will learn how Zeitlos fills and copies rectangles of pixels.

The **blitter** (`rtl/gpu/gpu_blit.v`) is a bus master that works on
rectangles of the framebuffer by itself while the CPU goes on with
other work. It can:

- **fill** a rectangle with a 32-bit pattern (all on, all off, a
  checker...);
- **copy** a picture into a rectangle, from main memory or from VRAM;
- draw characters in **glyph mode**: each glyph, up to 8 pixels wide,
  comes from the glyph memory in region 3.

Because it writes a whole 32-pixel word at a time where it can, a fill
is far faster than the CPU setting pixels one by one.

```sources
(source "Zeitlos docs: gpu_blitter.md, Overview; Key Features; Copy modes")
```

```check
(choice
  (ask "What does the CPU do while the blitter fills a large rectangle?")
  (right "It can go on with other work, and check the status register later")
  (wrong "It waits, because the blitter uses the CPU's ALU"
    "The blitter is a separate engine and a bus master of its own. The CPU only writes its registers to start it.")
  (wrong "It copies the pixels the blitter has worked out"
    "The blitter writes VRAM itself, through the VRAM bus."))
```

---

## Its registers

The blitter's registers are in region D:

| Address | Register |
|---|---|
| 0xD000_0000 | CTRL: what to do, and go |
| 0xD000_0004 | STATUS: bit 0 is BUSY |
| 0xD000_0008 | DST_X, where the rectangle starts |
| 0xD000_000C | DST_Y |
| 0xD000_0010 | WIDTH |
| 0xD000_0014 | HEIGHT |
| 0xD000_0018 | PATTERN, for a fill |

The bits of CTRL: bit 0 START, bit 1 FILL (1 to fill, 0 to copy), bit 2
CLIP (keep inside the clip rectangle), bit 3 GLYPH, bit 4 SRCMEM (copy
from main memory rather than VRAM). A program writes the position, size
and pattern, then CTRL with START set.

```sources
(source "Zeitlos docs: gpu_blitter.md, Memory Map; Control Register (BLIT_CTRL); Status Register (BLIT_STATUS)")
```

```check
(number
  (ask "What value written to CTRL starts a fill that keeps inside the clip rectangle? (In decimal.)")
  (answer 7) (from "1 + 2 + 4")
  (wrong-if 3 "That is START and FILL. Clipping is bit 2, worth 4 more.")
  (wrong-if 6 "Without bit 0, START, the blitter does nothing.")
  (hint "START is bit 0, FILL bit 1, CLIP bit 2.")
  (hint "1 + 2 + 4."))
```

---

## Raster operations

Bits 6 and 5 of CTRL choose a **raster operation**: how each new pixel
is combined with the one already there. Choose one and compare.

- 0, COPY: the new pixel;
- 1, OR: on if either is on;
- 2, XOR: on if exactly one is on;
- 3, ANDN: the old pixel, cleared where the new one is on.

```figure
(define op (choice 'op '("COPY" "OR" "XOR" "ANDN") "Operation"))
(define (res i j)
  (let ((d (zi-dst i j)) (s (zi-src i j)))
    (cond ((= op 0) s) ((= op 1) (or d s))
          ((= op 2) (not (eq? d s))) (else (and d (not s))))))
(zi-pixels 14 50 14 zi-dst)
(zi-pixels 118 50 14 zi-src)
(zi-pixels 222 50 14 res)
(text 56 40 "in VRAM" 'small 'centre)
(text 160 40 "new" 'small 'centre)
(text 264 40 "result" 'small 'centre)
(text 160 170 (string-append "operation " (list-ref '("COPY" "OR" "XOR" "ANDN") op)) 'body 'centre)
```

```sources
(source "Zeitlos docs: gpu_blitter.md, Raster operations")
```

```check
(choice
  (ask "A pointer shape is drawn with XOR, then drawn again with XOR in the same place. What is left?")
  (right "Exactly what was there before")
  (wrong "A blank rectangle"
    "XOR inverts the pixels it crosses; inverting them again restores them. Nothing is cleared.")
  (wrong "The shape, drawn twice as dark"
    "A pixel is only on or off. Two XORs cancel out.")
  (hint "Try XOR in the figure in your head: invert, then invert again."))
```

---

## Physical addresses

To copy from main memory, the blitter is given the source's address.
The blitter is its own bus master, and the MTU translates only the
CPU's addresses, so this must be a **physical** address. The drawing
library, `z_fb_hw_blit_mem()`, reads the MTU's base and translates an
app's pointer before handing it over.

```sources
(source "Zeitlos docs: gpu_blitter.md, Source addresses are physical")
```

```check
(choice
  (ask "Why does z_fb_hw_blit_mem() read the MTU's base register?")
  (right "To turn the app's window address into the physical address the blitter needs")
  (wrong "To check that the app is allowed to use the blitter"
    "Permission is the MPU's business. The base is read to translate the pointer.")
  (wrong "Because the blitter's registers move with each app"
    "The registers are always at 0xD000_0000. It is the source pointer that needs translating."))
```

---

## Practice

```check
(number
  (ask "Which value in CTRL starts a fill with XOR, without clipping? (In decimal. XOR is 2 in bits 6 and 5.)")
  (answer 67) (from "1 + 2 + 2 * 32")
  (wrong-if 5 "The raster operation sits in bits 6 and 5, so its value 2 is worth 2 x 32 = 64, not 2.")
  (wrong-if 66 "Without START, bit 0, nothing happens.")
  (hint "Bit 5 is worth 32, so the value 2 there is worth 64.")
  (hint "64 + 2 + 1."))
```

```check
(number
  (ask "Which value in CTRL starts a plain copy from main memory? (In decimal.)")
  (answer 17) (from "1 + 16")
  (wrong-if 19 "That has FILL set too. A copy has bit 1 clear.")
  (wrong-if 16 "Without START, the blitter waits.")
  (hint "START is bit 0, SRCMEM bit 4."))
```

So: the blitter fills, copies and draws glyphs in rectangles of the framebuffer by itself, combining pixels by a raster operation, and it takes physical addresses.
