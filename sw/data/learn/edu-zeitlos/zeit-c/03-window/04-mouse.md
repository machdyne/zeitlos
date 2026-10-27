# The mouse

```lesson
(objective "Read mouse messages, turn their screen position into the window's, and keep what was drawn so it can be drawn again.")
(minutes 25)
(keywords "screen coordinates")
```

wm sends WM_MOUSE, 109, to the window with the focus while the
pointer is over it, whenever the pointer moves or a button changes. A stationary mouse sends nothing.

While you hold a button down that you pressed inside the window, the
messages keep coming to it even when the pointer leaves, so a drag is
never cut off at the edge.

```sources
(source "sw/common/zwm.h, Z_WM_MOUSE")
(source "docs/window_manager.md, App protocol")
```

```check
(choice
  (ask "The pointer rests over your window and nobody touches the mouse. How many WM_MOUSE messages arrive each second?")
  (right "None: wm sends one only when something changes")
  (wrong "About 60, one for each frame"
    "wm sends a mouse message only when the position or a button differs from the last one it sent.")
  (wrong "One a second, to say where it is"
    "There is no reminder. A program keeps the last position it was told.")
  (hint "What has to change before wm sends one?"))
```

---

## Inside the value

The value packs the position and the buttons:

| bits | holds |
|---|---|
| 0 to 9 | x |
| 10 to 19 | y |
| 20 to 23 | the buttons: bit 20 is the left one |

x and y are **screen coordinates**: counted from the top left of the
whole screen, not of your window. `z_win_mouse_content_xy(&w, v, &x,
&y)` stores the position in your content area's coordinates in x and
y, through their addresses, and gives back 1 when the point is inside
the content area.

```c
unsigned left = (v >> 20) & 1;
```

```check
(number
  (ask "A WM_MOUSE value has y = 50 in bits 10 to 19. What is (v >> 10) & 1023?")
  (answer 50)
  (wrong-if 1023 "& 1023 is a mask: 1023 is ten 1 bits, so it keeps the ten bits of y after the shift.")
  (hint "The shift moves y down to bit 0; the mask 1023 keeps its ten bits and drops the buttons above."))
```

---

## A sketch pad

This program draws a dot wherever you drag with the left button:

```c
#include "win.h"

#define MAXPTS 500

struct win w;
int px[MAXPTS];
int py[MAXPTS];
int n = 0;

void mouse(unsigned v)
{
    int x;
    int y;

    if (((v >> 20) & 1) == 0)
        return;               /* left button up */
    if (!z_win_mouse_content_xy(&w, v, &x, &y))
        return;               /* outside the window */
    if (n < MAXPTS) {
        px[n] = x;
        py[n] = y;
        n++;
    }
    z_win_fill_rect(&w, x, y, 2, 2, 1);
}
```

Its main is hellow's, with `else if (m.subject == WM_MOUSE)
mouse(m.obj.val);` in the loop.

```check
(choice
  (ask "Why does mouse() pass &x and &y, not x and y?")
  (right "So that z_win_mouse_content_xy can store the position in x and y")
  (wrong "Because x and y are arrays"
    "x and y are plain ints. Their addresses let the function store into them, as twice(&x) did.")
  (wrong "zcc needs & on every value passed to libz"
    "Only where the function must change the caller's variable. v is passed as a plain value.")
  (hint "Think back to the pointers lesson: how does a function change its caller's variable?"))
```

---

## Drawing it all again

When another window passes over yours, wm sends a WM_REDRAW, and
whatever was under it is gone. A program must be able to draw its
whole window from what it remembers.

That is why mouse() keeps every dot in the arrays px and py. On
WM_REDRAW:

```c
z_win_clear(&w);
for (int i = 0; i < n; i++)
    z_win_fill_rect(&w, px[i], py[i], 2, 2, 1);
```

`if (n < MAXPTS)` keeps the arrays from overflowing: after 500 dots,
new ones are still drawn but not remembered.

The arrays are global: a program from zcc has 16 KB for its local
variables and malloc together, but global arrays are part of the
program itself.

```sources
(source "docs/window_manager.md, Content redraw protocol")
(source "docs/executables.md, The stack size: code 0, the default, 16KB; sw/apps/zcc/emit.c writes flags 0")
```

```check
(choice
  (ask "A sketch pad keeps no record of its dots. You drag another window across it. What happens?")
  (right "The dots under the other window's path are lost, because the program cannot draw them again")
  (wrong "wm puts the dots back"
    "wm draws only frames and title bars. The inside of a window is its program's job, and it asks with WM_REDRAW.")
  (wrong "Nothing: the screen keeps them under the other window"
    "There is one picture on the screen. Where the other window was drawn, your dots were drawn over.")
  (hint "Who draws the inside of a window again after it was covered?"))
```

```check
(number
  (ask "Each dot keeps two ints of 4 bytes. How many bytes do px and py take for 500 dots?")
  (answer 4000) (from "500 * 2 * 4")
  (wrong-if 2000 "There are two arrays, px and py, each of 500 ints.")
  (wrong-if 1000 "An int is 4 bytes, not 1.")
  (hint "500 dots, 2 ints each, 4 bytes an int."))
```

So: WM_MOUSE carries screen coordinates and buttons, z_win_mouse_content_xy
turns them into the window's, and a program remembers what it drew so
it can draw it all again.
