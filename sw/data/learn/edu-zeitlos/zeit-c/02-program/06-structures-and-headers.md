# Structures and headers

```lesson
(objective "Group values in a structure, reach its fields directly and through a pointer, and share declarations in a header file of your own.")
(minutes 20)
(keywords "structure (in C)" "field (of a structure)" "macro (in C)" "header file")
```

A **structure** groups named values into one record:

```c
struct point {
    int x;
    int y;
};

struct point p;
p.x = 30;
p.y = 40;
```

`struct point` is now a type, like int. Each named value inside is a
**field**, reached with a dot: `p.x`. An array of them works too:
`struct point corners[4];` and `corners[2].y`.

```check
(choice
  (ask "struct point p; Which line sets p's y to 5?")
  (right "p.y = 5;")
  (wrong "point.y = 5;"
    "point is the name of the type. The variable is p, so its field is p.y.")
  (wrong "y = 5;"
    "A field belongs to one structure. y alone is a separate variable, which is not declared here.")
  (hint "The variable's name, a dot, the field's name."))
```

---

## Through a pointer

Functions take a structure by its address. With `struct point *q =
&p;`, the field is `q->x`, short for `(*q).x`.

```c
void move(struct point *q, int dx, int dy)
{
    q->x += dx;
    q->y += dy;
}
```

`move(&p, 5, 0);` moves p itself. zcc **refuses** to pass a structure
by value, or to return one: always pass its address.

```sources
(source "docs/zcc.md, The supported subset: structs or unions by value")
```

```check
(choice
  (ask "With p at x 30, y 40: move(&p, 5, 0); move(&p, 0, -10); Where is p?")
  (right "x 35, y 30")
  (wrong "x 30, y 40"
    "move gets p's address, so it changes p itself, not a copy.")
  (wrong "x 35, y 50"
    "The second move adds -10 to y: 40 - 10 is 30.")
  (hint "Add each dx to x and each dy to y."))
```

---

## #define

A line that starts with `#` is handled before the program is compiled.
`#define` makes a **macro**: a name that is replaced by its text
wherever it appears.

```c
#define WIDTH 200
#define HEIGHT 80
```

After this, `WIDTH * HEIGHT` is compiled as `200 * 80`. Names in
capitals for macros are a habit every C programmer follows. Change the
number in one place, and every use changes with it.

```check
(number
  (ask "#define SIDE 12 and later int area = SIDE \\* SIDE; What is area?")
  (answer 144) (from "12 * 12")
  (wrong-if 24 "SIDE \\* SIDE is 12 times 12, not 12 plus 12.")
  (hint "Replace SIDE by 12 and work it out."))
```

---

## Header files

`#include "shapes.h"` reads the file shapes.h in at that point, as if
you had typed it there. zcc looks for it first in the folder of the
source file, so /home/c/demo.c finds /home/c/shapes.h.

A **header file**, whose name ends in `.h`, holds what several source
files share: declarations, structures and macros, never function
bodies. Write printf's declaration and your struct point into one
header, and every program starts with one `#include` line.

```c
/* shapes.h */
int printf(const char *format, ...);
struct point { int x; int y; };
#define WIDTH 200
```

```sources
(source "sw/apps/zcc/cpp.c, search_include(): the including file's folder first")
```

```check
(choice
  (ask "/home/c/game.c says #include \"keys.h\". Where does zcc look first?")
  (right "/home/c/keys.h")
  (wrong "/keys.h, at the top of the card"
    "For #include with quotes, zcc first looks in the folder of the file that includes it.")
  (wrong "Only in zcc's own folder of headers"
    "That folder is searched too, but after the including file's own folder.")
  (hint "The same folder as game.c."))
```

---

## Why your own

Zeitlos comes with full headers for C programs, in
/data/zcc/libz/include: libz.h declares everything libz has. But a
header is read on every compile, and zcc on the board has 4 MB to work
in. A program that included libz.h, zsoc.h and zwm.h took more than
that and ran out of memory; the same program declaring the few things
it used compiled in under 1 MB.

So in this course you write small headers of your own, with only what
you use. The next unit starts with one: win.h.

```sources
(source "docs/zcc.md, Fixes found by the demo (October 2026); Memory")
(source "docs/demo.md, The languages demo: mandel.c includes no headers")
```

```check
(choice
  (ask "Why does this course write its own small headers instead of including libz.h?")
  (right "Every header is read on each compile, and big ones can take more memory than zcc has on the board")
  (wrong "libz.h is not on the card"
    "It is, in /data/zcc/libz/include. It is left out because reading it is slow and takes much of zcc's memory.")
  (wrong "Programs with headers run slower"
    "Headers hold declarations, which make no code. The cost is at compile time, in zcc's memory and time.")
  (hint "When is a header read: while compiling, or while running?"))
```

So: a structure groups fields, reached with . or through a pointer
with ->; #define and #include are handled first, and a header shares
declarations.
