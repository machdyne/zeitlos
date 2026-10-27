# The framebuffer

```lesson
(objective "Find the byte and the bit of VRAM that hold any pixel of the Zeitlos screen.")
(minutes 20)
(keywords "framebuffer" "game mode")
```

You will learn where each pixel of the screen lives.

The desktop is 640 x 480 pixels, and each pixel is one bit. All the
bits together are the **framebuffer**, kept in VRAM, which starts at
`0x2000_0000`. VRAM is block RAM inside the FPGA itself, not a chip on
the board. (Where a board shows colour, it reads more planes of bits
beside these; the framebuffer itself stays one bit a pixel. See
[colour](docs:color).)

The pixels are stored line by line from the top left. A line of 640
pixels takes 640 / 8 = 80 bytes.

```sources
(source "Zeitlos docs: gpu_blitter.md, Screen Specifications; game_mode.md (one 640x480x1bpp surface at 0x2000_0000)")
(source "Zeitlos source: sw/os/sh.c, FB_SIZE")
```

```check
(number
  (ask "How many bytes does the whole framebuffer take?")
  (answer 38400) (from "640 * 480 / 8")
  (wrong-if 307200 "That is the number of pixels. Each byte holds 8 of them.")
  (wrong-if 9600 "That is 640 x 480 / 32, the number of 32-bit words.")
  (hint "80 bytes a line, 480 lines."))
```

---

## Which bit is which pixel

Number the pixels from 0 at the top left, along each line:
pixel (x, y) is number n = y x 640 + x. Then:

- its byte is at `0x2000_0000` + n / 8, rounded down;
- inside that byte, its bit is n mod 8, and **bit 0 is the leftmost**
  of the 8 pixels.

Move the sliders and watch where the pixel lands.

```figure
(define x (slider 'x 0 639 10 "x" 1))
(define y (slider 'y 0 479 1 "y" 1))
(define n (+ (* y 640) x))
(define byte (floor (/ n 8)))
(define b (modulo n 8))
(text 160 30 (string-append "pixel (" (fmt x 0) ", " (fmt y 0) ")  n = " (fmt n 0)) 'body 'centre)
(text 160 60 (string-append "byte " (zi-addr (+ 536870912 byte))) 'body 'centre 'bold)
(for-range 0 8
  (lambda (k)
    (let ((bx (+ 48 (* k 28))))
      (if (= k b) (rect bx 90 28 28 'ink))
      (frame bx 90 28 28 'ink 1)
      (text (+ bx 14) 109 (fmt k 0) 'small 'centre (if (= k b) 'paper 'ink)))))
(text 160 140 "bits of the byte, left to right on the screen" 'small 'centre)
(text 160 170 (string-append "bit " (fmt b 0) " of the byte") 'body 'centre)
```

```check
(text
  (ask "At which address does the second line of the screen, y = 1, begin?")
  (accept "0x20000050") (accept "20000050") (accept "0x2000_0050") (accept "2000_0050")
  (wrong-if "0x20000080" "80 is decimal. The line is 80 bytes long, and 80 in hex is 0x50.")
  (wrong-if "0x20000280" "That is 640, counted in bytes. 640 pixels are 80 bytes.")
  (hint "The first line takes 80 bytes. What is 80 in hex?"))
```

---

```check
(number
  (ask "Pixel (13, 0) is in the second byte of VRAM. Which bit of that byte holds it?")
  (answer 5) (from "13 - 8")
  (wrong-if 2 "That counts from the right. Here bit 0 is the leftmost pixel of the byte.")
  (wrong-if 13 "A byte has only bits 0 to 7. Pixels 8 to 15 are in the second byte.")
  (hint "Pixels 8 to 15 are bits 0 to 7 of the second byte."))
```

---

## From VRAM to the screen

The video circuit in the GPU reads the framebuffer over and over and
sends it to the display, 640 x 480 at 60 frames a second. Which output a
board has is set by its board block: DVI over an HDMI-type connector
(DDMI) on most boards, VGA on a few such as the Obst, and composite
video, for a television, on builds that ask for it. Composite is always
320 pixels wide, because a television signal cannot carry 640, and in
its PAL form shows 50 frames a second.

**Game mode** shows a 320 x 240 part of the same framebuffer, each pixel
doubled to fill the screen. The monitor still receives 640 x 480: it is
a camera moved over the picture, not a new picture.

```sources
(source "Zeitlos docs: game_mode.md, What it actually is; composite.md, The one number that shapes everything")
(source "Zeitlos source: rtl/boards.vh, GPU_DDMI and GPU_VGA")
```

```check
(choice
  (ask "A game switches to game mode. What happens to the windows outside the 320 x 240 view?")
  (right "Nothing: they are still in the framebuffer, just not shown")
  (wrong "They are erased to make room"
    "Game mode has no second framebuffer. It only changes which part of the one framebuffer is shown, so nothing is destroyed.")
  (wrong "They are copied to main memory until game mode ends"
    "Nothing needs saving. The whole 640 x 480 framebuffer stays in VRAM."))
```

---

## Practice

```check
(text
  (ask "At which address does line y = 2 begin?")
  (accept "0x200000A0") (accept "200000A0") (accept "0x2000_00A0") (accept "2000_00A0")
  (wrong-if "0x20000100" "160 bytes is right, but 160 in hex is not 0x100 (that is 256). 160 = 10 x 16.")
  (wrong-if "0x20000160" "160 is decimal. In hex it is 0xA0.")
  (hint "2 lines of 80 bytes is 160 bytes.")
  (hint "160 = 10 x 16, and ten is the hex digit A."))
```

```check
(number
  (ask "How many 32-bit words of VRAM does one line of the screen take?")
  (answer 20) (from "640 / 32")
  (wrong-if 80 "80 is the line in bytes. A word is 4 bytes, 32 pixels.")
  (hint "640 pixels, 32 to a word."))
```

So: the screen is 38,400 bytes at `0x2000_0000`, 80 bytes a line, with each byte's bit 0 the leftmost of its 8 pixels.
