# Keys

```lesson
(objective "Read key messages, take a key's code out of the message's value with a shift and a mask, and move a shape with the arrow keys.")
(minutes 25)
(keywords "focus (of a window)" "shift (of bits)" "mask (of bits)" "key code" "hexadecimal")
```

Keys go to one window at a time: the one with the **focus**, which is
the window you last clicked or opened. wm sends its program a message
with the subject WM_KEY, 106, each time a key goes down and again when
it comes up.

If your window does not react to keys, click inside it first.

```sources
(source "sw/common/zwm.h, Z_WM_KEY")
```

```check
(choice
  (ask "You press and release the A key once while your window has the focus. How many WM_KEY messages arrive?")
  (right "Two: one as it goes down, one as it comes up")
  (wrong "One"
    "wm sends a message for each change: the press, and the release.")
  (wrong "None until you click the window"
    "Your window has the focus, so its keys come straight to it. Clicking is only needed to give the focus to it.")
  (hint "wm reports a key going down and a key coming up."))
```

---

## Inside the value

A WM_KEY message carries one 32-bit value with three things packed
into its bits:

| bits | holds |
|---|---|
| 0 | 1 if the key went down, 0 if it came up |
| 1 to 8 | Shift, Ctrl and the other keys held with it |
| 9 and up | which key it was |

Which key it was is the key's **key code**. Two operators take the
parts out. `v & 1` keeps only bit 0: the 1 is a
**mask**, a number with a 1 in each bit to keep. `v >> 9` is a
**shift**: every bit moves 9 places right, so bit 9 becomes bit 0.

```c
unsigned down = v & 1;
unsigned code = (v >> 9) & 0x7FFFFF;
```

`0x` starts a number written in **hexadecimal**, base 16, whose digits
run 0 to 9 and then A to F for 10 to 15. Each digit is 4 bits:
0x7FFFFF is 23 bits of 1, the width of the key code.

```sources
(source "sw/common/zwm.h, Z_WM_PACK_KEY and Z_WM_UNPACK_KEY_*")
(source "Timeless Computing (docs/tc.md), C: A Complete Program (masks)")
```

```check
(number
  (ask "A WM_KEY value is 49665, which is 97 \\* 512 + 1. What is v >> 9?")
  (answer 97) (from "(49665 - 1) / 512")
  (wrong-if 1 "1 is v & 1, the bit that says the key went down. Shifting right by 9 divides by 512 and drops the rest.")
  (hint "Shifting right by 9 places is dividing by 2 to the power 9, which is 512, and dropping the remainder."))
```

---

## Key codes

A key that types a character sends that character's number: `'a'` is
97, Enter 13, Esc 27, Backspace 127. Keys with no character have codes
from 0x110000 up:

| key | code |
|---|---|
| Up | 0x110000 |
| Down | 0x110001 |
| Left | 0x110002 |
| Right | 0x110003 |

Add them to win.h:

```c
#define KEY_UP     0x110000
#define KEY_DOWN   0x110001
#define KEY_LEFT   0x110002
#define KEY_RIGHT  0x110003
```

```sources
(source "sw/common/zkbd.h, keysyms: Unicode code points, and Z_KEY_* from 0x110000")
```

```check
(choice
  (ask "Which test is true for the Enter key going down?")
  (right "code == 13 && down == 1")
  (wrong "code == KEY_DOWN"
    "KEY_DOWN is the Down arrow key. Whether a key went down is bit 0 of the value, not its code.")
  (wrong "code == 'E'"
    "Enter types no letter. Its code is the character 13, the end of a line.")
  (hint "Enter's code is 13, and down is bit 0."))
```

---

## A square that moves

Add these to hellow.c's program in place of draw(), with the KEY lines
in win.h:

```c
int bx = 20;
int by = 20;

void square(int colour)
{
    z_win_fill_rect(&w, bx, by, 10, 10, colour);
}

void key(unsigned v)
{
    unsigned code = (v >> 9) & 0x7FFFFF;

    if ((v & 1) == 0)
        return;            /* a key let go */
    square(0);             /* erase it where it was */
    if (code == KEY_LEFT)  bx -= 5;
    if (code == KEY_RIGHT) bx += 5;
    if (code == KEY_UP)    by -= 5;
    if (code == KEY_DOWN)  by += 5;
    square(1);             /* draw it where it is */
}
```

In the message loop, `else if (m.subject == WM_KEY) key(m.obj.val);`,
and on WM_REDRAW, `z_win_clear(&w); square(1);`.

```check
(number
  (ask "The square starts at bx 20. You press Right three times and Left once. What is bx?")
  (answer 30) (from "20 + 3 * 5 - 5")
  (wrong-if 40 "Each press moves 5, but each release does nothing: key() returns at once when bit 0 is 0.")
  (hint "Only presses move the square, 5 pixels each."))
```

---

## Draw only what changed

key() does not clear the window. It erases the square where it was
and draws it where it is: two small rectangles. Clearing the whole
window and drawing everything again would make the window flash on
every key, and take longer.

A WM_REDRAW is different: then wm may have wiped some or all of your
window's inside, and everything must be drawn again.

```sources
(source "sw/apps/hello_win/hello_win.c, draw_counter(): redraw only what changed; a whole clear visibly flashes")
```

```check
(choice
  (ask "Why does key() call square(0) before changing bx?")
  (right "To erase the square at its old place; afterwards bx no longer says where that was")
  (wrong "To clear the whole window"
    "square(0) fills only the 10 by 10 square, in the background colour. The rest of the window is left alone.")
  (wrong "It is not needed: wm erases the old square"
    "wm does not know what your program drew. Without square(0), the old squares stay, and the square leaves a trail.")
  (hint "What would be on the screen at the old place if nothing erased it?"))
```

```try
(try (app "term")
  (task "Make a copy of hellow.c called move.c with the square and key() added. Compile and run it, and move the square with the arrow keys. Close it, then come back.")
  (check (choice (ask "Did the square move without leaving a trail?")
    (right "Yes")
    (wrong "It left a trail" "Call square(0) before changing bx or by, as key() does above.")
    (wrong "It did not move" "Click inside the window to give it the focus, and check the WM_KEY line in the message loop."))))
```

So: keys come in WM_KEY messages to the window with the focus, a mask
and a shift take out the key code, and a program erases and redraws
only what moved.
