# The line rasterizer

```lesson
(objective "Say how the Zeitlos line rasterizer draws a line, and count the pixels it sets.")
(minutes 15)
(keywords "line rasterizer" "command FIFO" "Bresenham's algorithm")
```

You will learn how Zeitlos draws lines in hardware.

The **line rasterizer** (`rtl/gpu/gpu_raster.v`) draws straight lines
into the framebuffer. A program writes the two ends and an operation,
then starts it:

| Address | Register |
|---|---|
| 0xA000_0000 to 0xA000_000C | X0, Y0, X1, Y1: the two ends |
| 0xA000_0010 | the operation: 0 clear, 1 set, 2 XOR |
| 0xA000_0014 | START |

It works out the pixels with **Bresenham's algorithm**: it steps one
pixel at a time along the longer direction, and moves one pixel in the
other direction whenever the true line has drifted half a pixel away.

```sources
(source "Zeitlos docs: gpu_raster.md, Overview; Memory Map; Raster operations")
```

```check
(choice
  (ask "Where are the rasterizer's registers?")
  (right "In region A")
  (wrong "In region D" "Region D is the blitter. Lines have their own engine in region A.")
  (wrong "In VRAM" "The rasterizer draws into VRAM, but its registers are in a region of their own."))
```

---

## One pixel a step

Drag the ring to move the line's far end. The line starts at the
pixel in the top left corner. Count the pixels.

```figure
(define p (drag 'end 240 120 10))
(define cx (clamp (floor (/ (- (car p) 32) 16)) 0 15))
(define cy (clamp (floor (/ (- (cadr p) 40) 16)) 0 9))
(define n (zi-bresenham 0 0 cx cy
  (lambda (i j) (rect (+ 33 (* i 16)) (+ 41 (* j 16)) 14 14 'ink))))
(for-range 0 17 (lambda (i) (line (+ 32 (* i 16)) 40 (+ 32 (* i 16)) 200 'grey 1)))
(for-range 0 11 (lambda (j) (line 32 (+ 40 (* j 16)) 288 (+ 40 (* j 16)) 'grey 1)))
(text 160 25 (string-append "(0, 0) to (" (fmt cx 0) ", " (fmt cy 0) "): " (fmt n 0) " pixels") 'body 'centre)
```

```check
(number
  (ask "A line from (0, 0) to (9, 3). How many pixels does the rasterizer set?")
  (answer 10) (from "9 - 0 + 1")
  (wrong-if 9 "Count both ends: x goes 0, 1, ... 9, which is ten pixels.")
  (wrong-if 13 "The rasterizer does not step in both directions at once for each pixel. It sets one pixel for each step along the longer direction, x.")
  (hint "One pixel for each x from 0 to 9."))
```

---

## A queue of commands

The rasterizer has a **command FIFO** of 16 entries: a program can queue
up to 16 lines without waiting, and the rasterizer draws them in order.
The operation travels with each line in the queue, so changing it after
queuing a line does not change that line.

The clip rectangle is different: it is read while the line is drawn.
Since every process uses the same clip registers, the kernel saves and
restores them at each context switch, as it does the CPU's registers.

```sources
(source "Zeitlos docs: gpu_raster.md, Key Features; The op travels in the FIFO entry; kernel.md, The scissor is part of the context")
```

```check
(choice
  (ask "A program queues an XOR line, then sets the operation register to set before the line is drawn. How is the line drawn?")
  (right "With XOR: the operation was taken into the queue with the line")
  (wrong "With set, the value in the register when it is drawn"
    "That is how it would work if the operation were read at drawing time. It is captured when the line is queued.")
  (wrong "Not at all: the change cancels it"
    "Changing a register does not remove anything from the queue."))
```

---

## Practice

```check
(number
  (ask "A vertical line from (2, 5) to (2, 12). How many pixels?")
  (answer 8) (from "12 - 5 + 1")
  (wrong-if 7 "Count both ends: lines 5 to 12 are eight lines.")
  (hint "The longer direction is y, from 5 to 12."))
```

```check
(choice
  (ask "The window manager draws a window's outline with XOR while you drag it. Why XOR?")
  (right "Drawing it again in the same place takes it away and leaves what was underneath")
  (wrong "XOR lines are faster than set lines"
    "They take the same time. XOR is chosen because it is its own inverse.")
  (wrong "XOR always draws white"
    "XOR inverts: white over black and black over white, which is why it shows on anything."))
```

So: the line rasterizer queues up to 16 lines and draws each with Bresenham's algorithm, one pixel per step along the longer direction.
