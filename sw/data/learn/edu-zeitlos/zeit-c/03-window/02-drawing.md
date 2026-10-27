# Drawing in a window

```lesson
(objective "Place rectangles and text in a window's content area by their coordinates, and draw numbers as text.")
(minutes 20)
(keywords "pixel" "content area" "font (of letters)")
```

The screen is a grid of dots, each a **pixel**. Your program draws in
its window's **content area**: the inside, below the title bar and
within the frame. Positions in it are counted in pixels from its top
left corner, which is (0, 0): x to the right, y **down**.

The size you ask z_win_create_flags for includes the frame: the
content area is 4 pixels narrower and 15 shorter.

Drag the point to see its coordinates.

```figure
(define pt (drag 'p 150 110 6))
(define px (clamp (car pt) 40 280))
(define py (clamp (cadr pt) 50 200))
(frame 40 30 240 170 'ink 1.5)
(rect 40 30 240 20 'ink)
(text 160 44 "hello" 'centre 'small 'paper)
(line 40 50 px 50 'cool 1)
(line 40 50 40 py 'cool 1)
(dot px py 'ink)
(text 44 64 "(0, 0)" 'small)
(text 160 222 (string-append "x " (fmt (- px 40) 0) ", y " (fmt (- py 50) 0)) 'centre 'body)
```

```sources
(source "sw/common/zwin.h, Z_WIN_CHROME_W and Z_WIN_CHROME_H; z_win_draw_text")
```

```check
(number
  (ask "A window is created 200 wide and 80 high. How tall is its content area?")
  (answer 65) (from "80 - 15")
  (wrong-if 80 "80 is the whole window, frame and title bar included. The content area is 15 shorter.")
  (hint "Take the frame and title bar away: 15 pixels."))
```

---

## Rectangles

```c
z_win_fill_rect(&w, 10, 20, 50, 30, 1);
```

fills a rectangle whose top left corner is at x 10, y 20, 50 wide and
30 high. The last value is the colour: **1** is the ink, **0** the
window's background. Filling with 0 erases. `z_win_clear(&w)` fills
the whole content area with 0.

Anything outside the content area is cut off, so a program cannot draw
over another window.

```sources
(source "docs/window_manager.md, Drawing content")
```

```check
(choice
  (ask "z_win_fill_rect(&w, 10, 20, 50, 30, 1) has been drawn. Which call erases it again?")
  (right "z_win_fill_rect(&w, 10, 20, 50, 30, 0);")
  (wrong "z_win_fill_rect(&w, 10, 20, 50, 30, -1);"
    "There are two colours: 1, the ink, and 0, the background. Drawing the same rectangle in 0 erases it.")
  (wrong "z_win_fill_rect(&w, 0, 0, 0, 0, 0);"
    "That rectangle is 0 wide and 0 high, so it covers nothing. Erase the same place in colour 0.")
  (hint "Same place, same size, other colour."))
```

---

## Text

`z_win_draw_text(&w, x, y, s, 1, z_font_5x8_get())` draws the string
s with its top left corner at (x, y).

A **font** is a set of letter shapes. In the 5x8 font every character
is 5 pixels wide and 8 high, so a line of text is 8 pixels tall and
"Hello" is 25 pixels long. wm keeps this font in the graphics
hardware, so it is the one to use.

```sources
(source "sw/apps/hello_win/hello_win.c: every app draws with z_font_5x8")
(source "sw/common/zfont_data.c, z_font_5x8")
```

```check
(number
  (ask "How many pixels wide is the text \"Score: 100\" in the 5x8 font?")
  (answer 50) (from "10 * 5")
  (wrong-if 45 "Count the space too: S c o r e : space 1 0 0 is 10 characters.")
  (wrong-if 80 "8 is the height. Each character is 5 pixels wide.")
  (hint "Count every character, the space included, and multiply by 5."))
```

---

## Numbers as text

z_win_draw_text takes only a string. To draw a number, first write it
into a char array with `snprintf`, which works like printf but puts
the text in the array:

```c
int snprintf(char *buf, unsigned size,
    const char *format, ...);

char line[32];
snprintf(line, sizeof line, "Score: %d", score);
z_win_draw_text(&w, 8, 20, line, 1, z_font_5x8_get());
```

`sizeof line` is the array's size in bytes, 32: snprintf never writes
more than that, so it cannot write past the end. libz's snprintf takes at most 5
values after the format, and printf at most 7.

```sources
(source "docs/libz.md, What is in it, and what is not; The argument ceiling")
```

```check
(choice
  (ask "Why give snprintf the size of line?")
  (right "So that it stops before the end of the array, whatever the number")
  (wrong "So that it knows how many digits to print"
    "The format says how to print. The size is a limit: snprintf never writes past it.")
  (wrong "zcc needs it to make the array"
    "The array is already made by char line[32]. The size tells snprintf where it must stop.")
  (hint "What happens in C when something writes past the end of an array?"))
```

---

## Practice

This draw() draws a bar for each of three numbers:

```c
int bars[3] = { 20, 45, 30 };

void draw(void)
{
    z_win_clear(&w);
    for (int i = 0; i < 3; i++)
        z_win_fill_rect(&w, 10 + i * 30, 60 - bars[i],
            20, bars[i], 1);
}
```

```check
(number
  (ask "At what x does the third bar (i = 2) start?")
  (answer 70) (from "10 + 2 * 30")
  (wrong-if 100 "i is 2 for the third bar, not 3: 10 + 2 \\* 30.")
  (hint "Put i = 2 into 10 + i \\* 30."))
```

```check
(number
  (ask "And at what y is the top of the second bar (i = 1)?")
  (answer 15) (from "60 - 45")
  (wrong-if 45 "45 is the bar's height. Its top is at 60 - 45, since y counts down from the top.")
  (hint "The bars all end at y 60. A taller bar starts higher, so at a smaller y."))
```

So: x runs right and y down from the content area's top left corner,
colour 1 is ink and 0 is background, and numbers are drawn after
snprintf has made them into text.
