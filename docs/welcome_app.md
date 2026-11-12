# welcome

Kind: a **desktop app** ([app_kinds.md](app_kinds.md)), and a **core
app**, in flash ([flash_apps.md](flash_apps.md)).

What a new machine shows first: what this computer is, where to start,
the keys, and the guide. With a card it also offers the first
questions, each optional: keyboard, time zone, display.
`sw/apps/welcome`.

```
> run welcome                the kernel shell, or posix
> (run "welcome")            term's REPL
```

**Status: host-tested, not yet run on a board.** Every page is walked
by keyboard and by pointer in `tests/render.c` (98 checks), and the app
and the core archive build for the board. What to check on hardware is
at the end of this page.

```
+------------------------------------------------------+
| Welcome to Zeitlos                            1 of 4 |
| A few optional questions follow -- Skip leaves       |
| everything as it is                                  |
|                                                      |
| Every part of this computer is open and documented...|
| If you like, the next pages ask for your keyboard... |
|                                                      |
| [ Read the guide ]  windows, the dock, files and ... |
|                                                      |
| Keys that work here, and in every app: ...           |
| ---------------------------------------------------- |
| [ Skip ]                                   [ Next ]  |
+------------------------------------------------------+
```

Its first job is the welcome. The questions are the second, and only
where there is a card to keep the answers on.

## The welcome

The first page, on every machine:

- what this computer is: open and documented from the processor to the
  window, and made to go on working, with no account and nothing to
  sign in to;
- **with a card:** that a few optional questions follow, that Skip
  leaves everything as it is, and that settings changes any answer
  later;
- **with no card:** where to start, all in the dock -- `term` (Scheme,
  telnet, ssh and serial), `text` and `files` -- and that a memory card
  adds documents, courses, games and the manuals;
- **Read the guide:** [welcome.md](welcome.md), the guide to windows,
  the dock, files and the keys, in `read`. Both are in flash
  (`CORE_APPS`, `CORE_FILES`), so a machine with no card has them; in a
  build without `read`, `text` shows the same page as it is written.
  The welcome stays open behind it;
- the keys that work in every app.

With no card the button at the bottom is **Close**, and there is
nothing else to it: nothing is asked, because there is nowhere to keep
an answer. The page does not talk about that; a person in their first
minute does not need to know what is not there.

**What decides it is whether a card is mounted** -- `fs_df()` reports a
volume -- not a card-detect line. Few boards wire one into the
gateware, and a card that is in but cannot be read is no better than
none.

## The questions, with a card

| page | offers | writes ([config.md](config.md)) | when |
|---|---|---|---|
| Welcome | the welcome, above | -- | -- |
| Language | English and the languages the card has, each by its own name | `system.language` | on Next; windows opened from then on |
| Keyboard | the layout the machine starts in | `system.keyboard.layouts` | on Next; used from the next key |
| Time zone | a city, or a whole-hour offset | `system.rtc.timezone` | on Next |
| Display | white, amber, green or paper | `system.video.mode` | at once, and saved |
| Ready | nothing: what is in use, and three places to go next | -- | -- |

- **Every question is optional.** Skip on Welcome leaves everything as
  it is; so does the close box on any page, and Next on a page where
  nothing was changed.
- **It has no settings of its own.** Every value is one
  [settings](settings_app.md) already edits, written the way settings
  writes it: the one line for that key replaced in `/sys/zeitlos.cfg`,
  every other line copied back byte for byte, then a reload. Anything
  chosen here is changed there.
- **An answer that changes nothing writes nothing.** Going through
  every page with Enter on a card that is already set up does not
  touch the card.
- **A page the board cannot do is not shown.** A bitstream with no
  display-mode register has no Display page, and the count at the top
  right says "of 4", not "of 5". Desktop colour themes are left to
  settings.
- **Keyboard: the chosen layout becomes the first.** Any others already
  listed stay behind it as they were (`de, us` with French chosen
  becomes `fr,de,us`), so Super+Space still reaches them. The list is
  every ordinary layout in `zkbd`'s table
  ([keyboard_layouts.md](keyboard_layouts.md)), taken from
  `sw/common/zkbd_layouts.c` when the app is built (`gen_kbd_names.awk`),
  so a layout added there appears here. The Japanese input methods are
  left out: they are a second layout to switch to, and settings turns
  them on with the font. A layout in use that is not in the list
  (`ja-us`) is shown by name, with nothing selected, and kept.
- **Time zone: the rows settings shows.** UTC, every city in
  `z_tz_cities`, then `UTC-12` .. `UTC+14`. A value with no row
  (`UTC+5:30` typed into the file) shows nothing selected, and Next
  keeps it.
- **Ready** shows the keyboard, time zone and display colour in use,
  and two buttons. Each starts another app and leaves this window
  open behind it:

  | button | starts |
  |---|---|
  | Start the course | `learn`, whose first course is Getting started ([learn.md](learn.md)) |
  | Set a password | `settings security` ([security.md](security.md)) |

  These are card apps. One that is not on the card is reported on the
  status line, and its argument is taken back so that it is not handed
  to whatever opens next. (There was a "Take the tour" button; the tour
  it opened, `/data/slide/tour.zsl`, is a tour of slide, not of
  Zeitlos, so it is slide's own, from its Open dialog.)
- **A card that refuses writes** (write-protected): the status line
  says `not saved: the card could not be written`, and the pages go
  on.
- **Speech.** When speech is on (Super+S), each page says its number,
  title and note, and the controls announce themselves as focus moves,
  as everywhere ([tts.md](tts.md)).
- **F1** shows the help in the window, in place of the page; any key or
  click goes back to the page, and does nothing else.

## When it opens by itself

Once, in either of two cases (`first_run_check()` in `wm`, and
`sw/common/zwelcome.h`), when boot has finished and the dock is live:

| case | the mark | who writes it | who clears it |
|---|---|---|---|
| a new card | `/sys/firstrun` on the card | the release tool, on every card image, never in a manifest | `welcome`, as it starts |
| a machine with no card that has never shown it | no `apps.welcome.seen` in the flash key/value store ([kvstore.md](kvstore.md)) | -- | `welcome` writes the key, as it starts |

- **`update` follows the manifest**, so it never writes
  `/sys/firstrun`. A card that is already somebody's is never
  interrupted by a newer release ([philosophy.md](philosophy.md),
  "Tool").
- **A card that is in and has no mark is never interrupted** -- not
  even when the store has no key, as after an image that erased it
  ([kvstore.md](kvstore.md), "Images").
- **`welcome` clears both as soon as it starts**, before anything is
  asked. Finish, Skip, Close, the close box and a power cut all count as
  having been shown. Cleared first, not last, because a machine that
  asks again after every interrupted attempt is nagging.
- **A store that cannot be written** (a bitstream without flash
  writes): `wm` does not start it for a card-less machine, which would
  otherwise show it at every boot.
- **A mark that cannot be removed** (a write-protected card): the
  console says so, and it will open again at the next boot.
- **Not in the console profile**, which has no desktop.
- **The Machdyne case.** A computer shipped with a written card has
  both: the card's mark and an empty store. The first boot shows the
  welcome and the questions, and both marks are cleared.
- **When it does not open, the console says why** -- one line from
  `wm` at the end of boot:

  | the line | so |
  |---|---|
  | `wm: welcome: not a new card (no /sys/firstrun)` | the card has been booted before, or was updated or copied rather than written from an image -- by design, it is somebody's |
  | `wm: welcome: no card, and the flash store is read-only` | this bitstream cannot write the flash (`FLASHW`) |
  | `wm: welcome: no card, and shown already (apps.welcome.seen)` | this machine has shown it |
  | `wm: could not start welcome`, or no line at all | it is not in flash -- `/apps/welcome` is missing from `ls /apps` -- because the core apps' archive (and so its `wm`) predates it: `make BOARD=<board> flash_apps`, and for the emulator `zrelease sim`, which now refuses an archive without it |

  `run welcome` at the console, or from a terminal, opens it any time.
- **A card for a bench or a test rig:**

  ```
  $ release/zrelease sdcard --no-first-run
  ```

  or delete `/sys/firstrun` from the card. To see it again as a new
  card would show it, put any non-empty file there
  (`echo new > /sys/firstrun` in posix); as a card-less machine would,
  `kv del apps.welcome.seen` at the console and boot with no card.

### And what the machine opens with

`system.start` ([config.md](config.md)) names an app the machine opens
with -- `text /home/book.txt` on a writing machine. `wm` starts it at
the same moment, **first**, so that on a new machine the welcome is the
window on top. Both happen once boot has finished; the desktop is
behind them.

## Keyboard-only, pointer-only

Fully usable with either alone ([philosophy.md](philosophy.md),
"Accessible").

| | |
|---|---|
| **Enter** | Next -- except on one of the page's own buttons, which it presses |
| **Esc** | Back |
| **Tab / Shift+Tab**, arrows | the next or previous control: the list, the page's buttons, Back, Next, wrapping; disabled ones are skipped |
| **Up / Down**, PgUp / PgDn, Home / End | in a list: move the selection |
| **A-Z, 0-9** | in a list: jump to the first row starting with it, wherever the keyboard is; letters typed within a second build a prefix (`ber` is Berlin) |
| **Enter** in a list, or a **double-click** on a row | take the row and go on |
| **Space** | press the focused button |
| the **scroll wheel** | in a list: moves the view, three rows a notch; the selection stays ([user_input.md](user_input.md#scroll-wheel)) |
| **F1** | the help, in the window |

When a page opens, the keyboard is on its list if it has one and on
Next if it has not, so Enter alone goes from Welcome to Ready. A click
on a control moves the keyboard to it. A button that leads nowhere
(Back on Welcome, Skip with no card) is not drawn.

## Size

It is in flash, so it is kept small:

- no dialogs: F1 draws its help in the window;
- no desktop colour themes (settings has them);
- the layouts' names from a table generated at build time
  (`kbd_names.h`, not committed) rather than `zkbd.o`, whose key tables
  are about 17KB;
- the integer-only printf without newlib's stdio (`ZFMT_LITE`,
  `ZFMT_NO64`, [build.md](build.md)).

| | |
|---|---|
| `welcome.bin` | 60,516 bytes (134 KB as the first, card-only version with dialogs and themes) |
| in flash | 40,828 compressed, of a 44K budget ([flash_apps.md](flash_apps.md)) |
| RAM | 72,948 image, of 76K, plus the default 16 KB stack and heap |
| `wm` | about 1.1KB for `first_run_check()` and `start_app()`: 118,356 in flash (stored) of 118,784, and 153,100 in RAM of 150K, with xPack GCC 14.2 |

Most of what is left is the window toolkit every windowed app carries
(`zgfx`, `zwin`, `zwidget`, the fonts). Saving allocates nothing: the
file is edited in two static 4 KB buffers, for the reason settings
gives.

## Testing

```
$ sudo sysctl -w vm.mmap_min_addr=0
$ make -C sw/apps/welcome test        98 checks, 12 pictures in /tmp/welcome-render
$ python3 release/lib/test_mkfatimg.py
$ python3 release/lib/test_store.py
```

`tests/render.c` is the real `welcome.c` with a scripted kernel that
keeps the file, the mark, a card that can be in or out or
write-protected and the key/value store in memory, a scripted display
register and scripted launches. `zkbd.c` is linked into the test, and
only there, to check `kbd_names.h` against the real table. It checks:

- the layouts are exactly zkbd's ordinary ones, in order;
- the mark and the key are cleared before anything is asked, and only
  when there is something to clear;
- Read the guide opens `read` with `/docs/welcome.md`, and `text` with
  `/docs/welcome.txt` where `read` is not, with the welcome left open;
- no card: one page, Read the guide and Close, no Skip or Back, nothing
  written; a store that cannot be written is survivable;
- a new card by keyboard alone: `g` Enter, `ber` Enter, Enter, and the
  file is two lines;
- an edited file by pointer alone: comments, unknown keys, lines that
  are not settings and CRLF endings all come back as they were; an
  input-method layout in use is shown and kept;
- the same answers again write nothing;
- a double-click takes a row; a zone with no row is kept; the wheel
  scrolls and leaves the selection;
- a board with a display register has the Display page, and colours
  apply and save by click, Space and Enter;
- F1 shows the help, and Enter, Esc or a click there only goes back;
- the three Ready buttons start the right app with the right argument,
  and a missing app is reported with its argument taken back;
- a write-protected card: said, and survivable;
- every control inside the window, and no allocation from start to
  finish.

`release/lib/test_mkfatimg.py` formats real images and checks that a
card has `/sys/firstrun` and not `welcome` (it is in flash), that the
manifest does not list the mark, and that `first_run=False` leaves it
off. `release/lib/test_store.py` allows the mark as the third file on a
card that no manifest lists.

### On a board

Not yet done. Flash the gateware and core apps from the same tree
(`make BOARD=... flash`: `wm` and `welcome` are both in flash).

1. **No card, a fresh flash** (or `kv del apps.welcome.seen`): after the
   pointer turns from Z to X, the welcome opens. The console shows
   `wm: no card, and this machine has not shown the welcome -- starting
   welcome` and `welcome: no card: the welcome only`. Read the guide
   opens the guide in `text`. Close; reboot: it does not open.
2. **A new card** from `release/zrelease sdcard`: it opens with "1 of
   4" (or 5). The console shows `wm: a new card (/sys/firstrun)` and
   `welcome: a card -- /sys/firstrun removed`. Read the guide opens the
   guide in `read`.
3. Keyboard alone: Enter, `g`, Enter, `ber`, Enter; on a board with
   display colours, Tab to Amber and Space; Enter to Ready.
   `/sys/zeitlos.cfg` has the lines; `clock` shows Berlin time; the
   next key typed uses the German layout.
4. Pointer alone, after `(run "welcome")`: click a row, click Next,
   through to Finish.
5. Each Ready button opens its app, and the welcome is still there
   behind it.
6. Reboot: it does not open. Take the card out and reboot: it does
   not open either (the key was written in step 2).
7. Pull the power during step 3 on a fresh card, boot again: it does
   not open, and `(run "welcome")` does.
8. With speech on (Super+S), each page is announced.
9. `update` to the same release on a card with no `/sys/firstrun`: the
   file does not appear.
10. With `system.start: text` in `/sys/zeitlos.cfg` and a new card:
    `text` opens, and the welcome opens on top of it.
