# learn on Linux

[learn](learn.md) as a program on a Linux computer: the same app as on
Zeitlos -- the same screens, lessons, figures, records -- in an SDL2
window. Everything but the window and the machine's services is the
code the board runs (`sw/apps/learn/*.c`, as `lplat.h` splits it), so a
change to learn is a change to both, and browse mode, review and the
figures are the same here.

## Building

```
make -C sw/apps/learn linux        # -> sw/apps/learn/learn-linux
```

It needs a C compiler, Python 3 (for the build only) and SDL2's
development files:

| system | package |
|---|---|
| Debian, Ubuntu | `apt install libsdl2-dev` |
| Fedora | `dnf install SDL2-devel` |
| Arch | `pacman -S sdl2` |

The program is one file. It links SDL2, libm and libc, nothing else:
the fonts (`sw/data/font`) and Machdyne Scheme (`sw/ext/ms`, for the
figures; check out the submodule first, `git submodule update --init
sw/ext/ms`) are built into it, so it runs from any folder.
`make -C sw/apps/learn clean-linux` removes what the build made.

## Running

```
learn-linux [options] [FOLDER...]
```

A FOLDER is any of:

- **a pack**: a folder of courses -- an Ark checkout's `edu/`, or
  `sw/data/learn/edu-zeitlos` in this tree;
- **a folder of packs**, each a folder of courses;
- **one course** (a folder with `course.cfg`): it opens at once, and the
  catalogue shows the courses beside it.

```
git clone https://github.com/machdyne/ark ~/ark
./sw/apps/learn/learn-linux ~/ark/edu                        every Ark course
./sw/apps/learn/learn-linux ~/ark/edu sw/data/learn/edu-zeitlos   and the Zeitlos courses
./sw/apps/learn/learn-linux ~/ark/edu/lang-de-1              German 1, opened
```

With no folder, learn reads the packs in `$XDG_DATA_HOME/learn/packs`
(`~/.local/share/learn/packs` when it is not set): put or link packs
there to have them every time.

| option | | |
|---|---|---|
| `--light`, `--dark` | dark | the theme: dark is the board's look (light ink on black); light is dark ink on paper |
| `--scale N` | as it fits | each pixel of learn's page as N x N on the screen (1 to 6) |
| `--no-colour` | colour on | a figure that asks for colour drawn in black, white and patterns (F10 switches) |
| `--quiet` | speech on | no speech |
| `--data DIR` | `$XDG_DATA_HOME/learn` | where learners and their progress are kept |
| `--help` | | |

### Keys

Every key of learn is the same as on Zeitlos ([keys.md](keys.md), F1
lists them). The window adds:

| key | |
|---|---|
| F11 | full screen, and back |
| F10 | a figure's colour on or off |
| Ctrl+= , Ctrl+- | bigger, smaller (the scale) |
| Ctrl+0 | the scale that fits the window |
| Ctrl+T | light or dark |
| Ctrl+Q | quit (Esc on the home screen too) |

Typed answers take any character the keyboard makes -- ä, ö, ü, ß from
a German layout, a compose key or an input method -- as learn's answer
boxes take Unicode.

## Where things are

learn names its places as Zeitlos does; here they are folders of this
computer (`linux/lplat_linux.c`):

| learn's | on Linux |
|---|---|
| `/opt/ark/edu...` (the packs) | the folders named on the command line; each is a pack named after its folder (`edu-` put before a name that does not start with `edu`) |
| `/data/learn` | `$XDG_DATA_HOME/learn`, or `--data DIR` |
| `/data/learn/learners/NAME/` | the learner's records: `learner.cfg`, `progress.txt`, `review.txt`, `reports.txt` |
| `/usb/...` | nothing: there is no stick |

A learner's folder is the same plain text as on the board
([learn.md](learn.md#learners)), so it can be copied between a Zeitlos
card and a Linux computer either way. Times in it are UTC, as Zeitlos
keeps them ([clock_app.md](clock_app.md)). Files are written as on the
board: a record is one line added and synced; `learn.cfg` and the like
are written whole (`NAME.part`, then renamed).

## What is different from the board

- **Drawing.** learn draws its page, one bit a pixel, exactly as on the
  board; the window puts it on the screen scaled by a whole number, in
  the theme's two colours. The page is the window's size divided by the
  scale -- 640 x 480 or more -- so a bigger window shows more, not
  blurrier text.
- **Colour.** A figure that says `(colour)` is shown in colour whenever
  it shows whole, as the board's colour window shows it: the page holds
  the colours' lowest bit there and four planes the rest
  (`la_figure_plane()`), 16 colours. Every other figure is black,
  white and patterns, as everywhere.
- **Boxes** -- a new learner's name, a report of a mistake, a search,
  yes or no, F1's help -- are drawn in the window in learn's own look
  (the board's are zdialog's).
- **Speech** is `espeak-ng`, if it is installed (any package manager
  has it); otherwise learn is silent. One thing is said at a time: F6
  again starts over. It has learn's limit on the board too: one voice,
  so German words are read with the English one.
- **Other apps.** A `try` step on Zeitlos opens an app beside the
  lesson (`basic`, `calc`, `sheet` ...). Here a box says which app the
  step opens on Zeitlos, and learn goes on. A link to a web page opens
  in this computer's browser (`xdg-open`); a link into Zeitlos's
  documentation says where the page is in the source.
- **Slow work** -- finding courses, opening one -- runs about 15 ms at
  a time between the window's events (`la_work()`), and a step's words
  are drawn before its figure is run, as on the board
  ([responsive.md](responsive.md)). Files on a PC are fast enough that
  no coroutine is needed.

## Files

| | |
|---|---|
| `sw/apps/learn/linux/main.c` | the window, keys and the mouse, the boxes, the main loop |
| `sw/apps/learn/linux/lplat_linux.c` | files, the clock, speech: `lplat.h` over POSIX |
| `sw/apps/learn/linux/mkfonts.py` | the fonts as a C file (`linux/obj/fonts.c`, made by the build) |

`linux/obj/` holds what the build makes (the fonts, Scheme compiled for
this computer); neither it nor `learn-linux` is kept in git.
