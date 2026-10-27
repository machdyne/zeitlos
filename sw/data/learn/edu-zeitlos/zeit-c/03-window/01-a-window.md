# Opening a window

```lesson
(objective "Write a program that opens a window and answers the window manager's messages.")
(minutes 25)
(keywords "window manager" "message (between programs)" "mailbox (of a program)" "subject (of a message)" "message loop" "global variable")
```

The frames, title bars and dock on the screen are drawn by one
program, the **window manager**, `wm`. A program that wants a window
asks wm for one, and wm then tells it when to draw.

They talk in **messages**: a small record that one program sends to
another, which waits in the receiver's **mailbox** until it reads it.
Every message has a **subject**, a number that says what kind of
message it is, and carries one value.

libz does the asking for you: `z_win_create_flags` sends wm a message
and waits for the reply. What comes after is up to your program.

```sources
(source "docs/window_manager.md, App protocol")
(source "docs/messaging.md, The messaging system")
```

```check
(choice
  (ask "Who draws a window's title bar and frame?")
  (right "wm, the window manager")
  (wrong "Your program, with z_win_draw_text"
    "Your program draws only inside the window. wm draws the frame, the title bar and its icons.")
  (wrong "zcc, when it compiles the program"
    "zcc only makes the program file. Windows appear when the program runs and asks wm for one.")
  (hint "Which program owns everything on the screen outside the windows' insides?"))
```

---

## win.h

The full declarations are in zwin.h, too large to read on every
compile, so you write the part you use. Save this as /home/c/win.h:

```c
/* win.h: what our programs use of wm */
struct win {             /* 440 bytes, as zwin.h's */
    int id;
    unsigned x, y, w, h;
    char rest[420];
};
struct obj { int type; unsigned val; };
struct msg {             /* 216 bytes, as zmsg.h's */
    unsigned to, from, subject, tag;
    struct obj obj;
    char rest[192];
};
struct font;             /* only passed along */

#define WM_REDRAW    104
#define WM_KEY       106
#define WM_MOUSE     109
#define WM_SET_CLIP  119
#define WIN_CLOSE    3   /* close icon ends us */

int z_win_create_flags(struct win *w, const char *t,
    unsigned width, unsigned height, int x, int y,
    unsigned flags);
void z_win_clear(struct win *w);
void z_win_fill_rect(struct win *w, int x, int y,
    int width, int height, int colour);
void z_win_draw_text(struct win *w, int x, int y,
    const char *s, int colour, struct font *f);
int z_win_apply_clip(struct win *w, struct obj *o);
void z_win_apply_redraw(struct win *w, unsigned v);
void z_win_redraw_done(struct win *w);
int z_win_mouse_content_xy(struct win *w, unsigned v,
    int *x, int *y);
struct font *z_font_5x8_get(void);
int z_msg_read(struct msg *m);
void z_proc_wait(unsigned ticks);
```

```check
(choice
  (ask "Why does struct win end with char rest[420]?")
  (right "libz writes the whole 440-byte window record, so ours must be as big; we only name the fields we read")
  (wrong "To hold the window's title"
    "The title is passed to z_win_create_flags as a string. rest is room for the parts of libz's record we never read.")
  (wrong "It is not needed: libz uses only id, x, y, w and h"
    "libz keeps more in the record, such as which parts of the window are visible. Make it smaller and libz writes past its end.")
  (hint "libz's code was compiled with zwin.h's record, which is 440 bytes."))
```

---

## The program

Save this as /home/c/hellow.c:

```c
#include "win.h"

struct win w;

void draw(void)
{
    z_win_clear(&w);
    z_win_draw_text(&w, 8, 8, "Hello, window!", 1,
        z_font_5x8_get());
}

int main(void)
{
    struct msg m;

    if (z_win_create_flags(&w, "hello", 200, 80,
            -1, -1, WIN_CLOSE) != 0)
        return 1;
    for (;;) {
        z_proc_wait(0);
        while (z_msg_read(&m) == 0) {
            if (m.subject == WM_SET_CLIP)
                z_win_apply_clip(&w, &m.obj);
            else if (m.subject == WM_REDRAW) {
                z_win_apply_redraw(&w, m.obj.val);
                draw();
                z_win_redraw_done(&w);
            }
        }
    }
    return 0;
}
```

`struct win w;` is outside every function: a **global variable**,
which lasts for the whole run and which every function can use.
z_win_create_flags gives back 0 when wm made the window, and `-1, -1`
lets wm choose where it goes.

```check
(choice
  (ask "Why is w declared outside main, not inside it?")
  (right "Both main and draw use it, and it must last for the whole run")
  (wrong "Local variables cannot hold structures"
    "They can: m, the message, is a local structure in main. w is global so that draw can reach it too.")
  (wrong "zcc puts every structure outside main"
    "zcc puts variables where you declare them. A global one exists for the whole run and every function can use it.")
  (hint "Which functions use w?"))
```

---

## The message loop

`for (;;)` is a loop with no condition: it runs until the program
ends. It is the **message loop**, the heart of every windowed program:

- `z_proc_wait(0)` sleeps until a message arrives, so a waiting
  program takes no time from the others;
- `z_msg_read(&m)` takes the next message from the mailbox and gives
  back 0, or 1 when the mailbox is empty.

Two subjects must be answered:

| subject | means | you must |
|---|---|---|
| WM_SET_CLIP | which part of your window is visible | `z_win_apply_clip` |
| WM_REDRAW | draw your window's inside now | draw, then `z_win_redraw_done` |

Until the first WM_SET_CLIP, nothing you draw appears. wm sends a
WM_REDRAW straight after it.

```sources
(source "docs/window_manager.md, What an app owes the compositor")
(source "docs/app_runtime.md, What a windowed app owes the compositor; Idling: why background processes must block")
```

```check
(choice
  (ask "A program draws its text once, right after z_win_create_flags, and has no message loop. What appears?")
  (right "An empty window: nothing can be drawn before the first WM_SET_CLIP, and no WM_REDRAW is answered")
  (wrong "The text, until another window covers it"
    "A new window is invisible to drawing until wm has sent its visible part, WM_SET_CLIP. Drawing at once draws nothing.")
  (wrong "No window at all"
    "z_win_create_flags does make the window. Without a message loop, though, nothing is ever drawn inside it.")
  (hint "When does a window's drawing first reach the screen?"))
```

---

## Run it

In the POSIX window:

```text
zcc /home/c/hellow.c -o /home/c/hellow && hellow
```

The window should appear. The shell waits while it runs; click its close
icon to end the program, and the `$` comes back. WIN_CLOSE, 3, is two
of wm's flags together: draw a close icon (1), and let it end the
program (2).

To be honest about it: zcc compiles this program, but no window
program made by zcc has yet been run on a Zeitlos board. If zcc
reports nothing and still no window appears, or it misbehaves, press
F7 here in learn to report it.

```sources
(source "sw/common/zwm.h, Z_WIN_FLAG_CLOSE_ICON and Z_WIN_FLAG_CLOSE_KILLS_OWNER")
```

```try
(try (app "term")
  (task "Save win.h and hellow.c in /home/c with text. In term's POSIX window, compile and run hellow. Close its window when you have seen it, then come back.")
  (check (choice (ask "What did the window show?")
    (right "Hello, window!")
    (wrong "Nothing: it stayed empty" "Check the message loop against the listing: WM_SET_CLIP must be applied and WM_REDRAW must call draw().")
    (wrong "No window appeared" "Read the POSIX window: zcc may have reported a mistake, so nothing was run. Fix the line it names. If it reported nothing, press F7 to report it."))))
```

So: wm sends messages to your program's mailbox, and a window program
is a message loop that applies its visible part and draws when asked.
