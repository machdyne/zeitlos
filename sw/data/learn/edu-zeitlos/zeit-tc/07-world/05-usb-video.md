# USB, screens, sound and keys

```lesson
(objective "Say how USB finds out what is plugged in, how a picture reaches a screen, how sound is played, and what a keyboard and a mouse really send.")
(minutes 25)
(keywords "USB" "host (USB)" "enumeration" "descriptor" "device class" "pixel" "raster" "sync pulse" "frame (of video)" "pixel clock" "framebuffer" "VGA" "DVI" "HDMI" "composite video" "sample (of sound)" "mixer" "scan code" "keyboard layout")
```

**USB** (universal serial bus) connects keyboards, mice, storage and
most other things to a computer. It is far more complex than UART, SPI
or I2C; here is how it is organised.

USB has one **host**, the computer, and **devices**. The host controls
all the talking: a device sends only when the host asks it to. When a
device is plugged in, the host asks it to describe itself. This is
**enumeration**, and the device answers with **descriptors**: records
that say what it is and what it can do.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: USB")
```

```check
(choice
  (ask "You move a USB mouse. How does the computer find out?")
  (right "The host asks the mouse again and again, and the mouse answers with what has changed")
  (wrong "The mouse sends a message the moment it moves, whenever it likes"
    "On USB a device sends only when the host asks. The host asks the mouse often enough that it seems instant.")
  (wrong "The mouse writes into the computer's memory"
    "The mouse cannot reach the computer's memory. It answers the host's questions over the cable.")
  (hint "Who controls all the talking on USB?"))
```

---

## Classes

Many devices belong to a standard **device class**. A keyboard or a
mouse is a human interface device (HID); a USB stick belongs to the
mass storage class; a serial adapter to the communications device
class. A host that supports a class works with every device of that
class without extra software, so a USB keyboard from any maker works
on any computer.

This matters for keeping data: a drive that appears as a standard
mass storage device can be read by any computer that supports the
class. Each USB connector also carries 5 V, which powers many small
devices.

```check
(choice
  (ask "Why can a USB stick bought years ago still be read by a new computer with no special software?")
  (right "It belongs to the mass storage class, which the computer supports")
  (wrong "Every USB stick comes with its own software stored on it"
    "No software is needed, because the stick belongs to a standard class. A host that supports the class reads any stick of that class.")
  (wrong "USB sticks are all made by one company"
    "They are made by many companies. What they share is the standard mass storage class.")
  (hint "What does a host that supports a class work with?"))
```

---

## A picture on a screen

A picture is made of **pixels**, the small dots of one colour that make
it up. A screen draws its picture as a **raster**: line by line, each
from left to right, top to bottom, then back to the top to start again.
The signal carries each pixel's colour in turn, with **sync pulses** to
mark the end of each line and of each picture. One whole picture is a
**frame**.

The standard mode of 640 x 480 pixels at 60 frames a second also
spends time in margins and sync pulses: each frame takes 800 x 525
pixel times. The number of pixel times each second is the
**pixel clock**.

```figure
(define s 0.32)
(rect 30 30 (* s 800) (* s 525) 'grey)
(rect 30 30 (* s 640) (* s 480) 'paper)
(frame 30 30 (* s 640) (* s 480) 'ink 1.5)
(frame 30 30 (* s 800) (* s 525) 'ink 1)
(arrow 40 42 200 42 'ink 1)
(arrow 200 46 40 54 'cool 1)
(arrow 40 58 200 58 'ink 1)
(text 132 110 "640 x 480 seen" 'centre 'small 'bold)
(text 132 126 "line by line, top to bottom" 'centre 'tiny)
(text 30 214 "shaded: margins and sync pulses; 800 x 525 in all" 'tiny)
(caption "One frame, in pixel times")
```

```check
(number
  (ask "Each frame is 800 x 525 pixel times. How many pixel times is that?")
  (answer 420000) (from "800 * 525")
  (wrong-if 307200 "307,200 is 640 x 480, the pixels you see. The margins and sync pulses take time too: 800 x 525.")
  (hint "Multiply 800 by 525."))
```

```check
(number
  (ask "At 60 frames a second, what is the pixel clock, in millions of pixel times a second?")
  (answer 25.2) (within 0.05) (unit "MHz")
  (from "800 * 525 * 60 / 1000000")
  (wrong-if 18.432 "That counts only the 640 x 480 pixels you see. The clock also runs through the margins and sync: 800 x 525.")
  (hint "420,000 pixel times a frame, 60 frames a second."))
```

---

## The framebuffer

A computer makes this signal from a **framebuffer**: an area of memory
that holds one number for each pixel. A video circuit reads the
framebuffer in step with the raster and turns each number into a
colour. A program draws by writing into the framebuffer.

The same picture can leave by several kinds of output:

- **VGA** sends the colours as analog voltages on separate wires;
- **DVI** and **HDMI** send the same picture digitally, as fast serial
  streams;
- **composite video** puts the whole picture on one wire, in the form
  older televisions take.

In an FPGA computer the video circuit is part of the design, so one
machine can offer several modes and outputs.

```check
(number
  (ask "A 640 x 480 framebuffer uses 1 bit for each pixel (black or white). How many bytes does it take?")
  (answer 38400) (from "640 * 480 / 8")
  (wrong-if 307200 "307,200 is the number of pixels, and so of bits. A byte holds 8 of them.")
  (hint "Count the pixels, then divide by the 8 bits in a byte."))
```

---

## Sound

Sound out is sampling in reverse. A stream of numbers, one for each
**sample** (a measurement of the sound wave at one moment), is turned
back into a voltage by a DAC, or by a fast pulse stream and a filter,
and then made louder by an amplifier. 16-bit samples at tens of
thousands a second give good sound.

A **mixer** adds several streams together, so that music, speech and
sound effects can play at the same time.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Video; Audio")
```

```check
(choice
  (ask "A game plays music and, at the same moment, the sound of a door. What combines them?")
  (right "A mixer, which adds the two streams of samples")
  (wrong "The DAC, which plays them one after the other, very fast"
    "A DAC turns one stream of numbers into a voltage. Two streams are first added into one by the mixer.")
  (wrong "Nothing: one sound must stop for the other"
    "Adding the samples of two streams gives one stream that holds both sounds. That is the mixer's job.")
  (hint "Sound waves that happen together add up."))
```

---

## Keyboards and mice

A keyboard does not send letters. It sends **scan codes**: numbers
that say which key was pressed and which was released. The computer
turns them into characters by a **keyboard layout**, so the same key
gives Z under one layout and Y under another. Shift, or a key such as
AltGr, adds to what the computer must keep track of.

A mouse reports small movements: how far it moved left or right, and
up or down, since its last report, and which buttons are down. The
computer adds up the movements to place the pointer.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Keyboards and Mice")
```

```check
(number
  (ask "The pointer is at x = 100. The mouse then reports moves of +5, +3 and -2 across. Where is the pointer now?")
  (answer 106) (from "100 + 5 + 3 - 2")
  (wrong-if 6 "6 is the sum of the moves. The pointer started at 100, and the moves are added to where it was.")
  (wrong-if 98 "That is only the last move. Each report adds to the position.")
  (hint "Start at 100 and add each move in turn."))
```

```check
(choice
  (ask "A keyboard is set up with the wrong layout. You press the key marked Z and get a Y. What went wrong?")
  (right "The computer turned the key's scan code into a character with the wrong layout")
  (wrong "The keyboard sent the letter Y"
    "A keyboard sends a scan code for the key, not a letter. The computer chose Y by its layout.")
  (wrong "The key is broken"
    "A broken key sends nothing. Here a scan code arrived, and the layout turned it into Y.")
  (hint "What does a keyboard actually send?"))
```

So: USB hosts ask and devices answer, a picture is a raster read from
a framebuffer, sound is a stream of samples, and keys are scan codes
turned into characters by a layout.
