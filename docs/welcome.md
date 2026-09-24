# Welcome to Zeitlos

If you're reading this, Zeitlos has already booted into its
graphical desktop -- you should see a dock at the bottom of the
screen. This page covers the basics of getting around.

## The desktop

A dock sits at the bottom of the screen with icons for launching
apps. Windows can be moved around by their titlebar.

### Using the mouse

- **Click a window** to bring it to the front and focus it.
- **Click and drag a titlebar** to move a window around. Windows are
  kept fully on-screen.
- **Drag the corner grip** of a resizable window to resize it.
- **Double-click a titlebar** to maximize a resizable window -- it fills
  the screen and the dock hides -- and double-click again to put it
  back. `term` does not maximize: its screen is always 80x25.
- **Alt+double-click a titlebar** to shade the window -- only its
  titlebar stays -- and again to unshade it. A maximized window cannot
  be shaded; put it back first.
- **Click a dock icon** to launch that app.

### Using the keyboard

Zeitlos works fully without a mouse if you don't have one plugged in --
and without a keyboard, through the on-screen one (Super+K). The rule
of thumb: **Alt** acts on the focused window, **Super** on the system as
a whole -- game mode, speech, keyboard layouts. Every key combination
there is:

**Windows and the dock** ([window_manager.md](window_manager.md))

| Keys | What they do |
|---|---|
| Alt+Tab | cycle focus between windows and the dock |
| Alt+Arrow | move the focused window |
| Alt+Equal | maximize the focused window, or put it back (the key left of Backspace, whatever it types) |
| Alt+Minus | shade the focused window, or unshade it (the key right of 0, whatever it types) |
| Alt+[ / Alt+] | previous / next page of dock icons (the two keys right of P, whatever they type) |
| Arrows, Enter | while the dock has focus: move between icons, launch one |

**Keyboard and languages** ([keyboard_layouts.md](keyboard_layouts.md))

| Keys | What they do |
|---|---|
| Super+Space | next keyboard layout (set the list in `settings`) |
| Super+K | the on-screen keyboard ([keyboard_app.md](keyboard_app.md)) |
| AltGr (right Alt) | the third and fourth characters on a key, on layouts that have them: `@` on German is AltGr+Q |
| a dead key, then a letter | an accented letter: on German, `´` then `e` is `é`; the dead key twice, or then Space, types the accent alone; Escape cancels it |
| romaji, on a `ja` layout | kana: `konnichiha` is こんにちは; Backspace takes back a waiting letter, Escape drops them ([Japanese input](keyboard_layouts.md#japanese-input)) |

**Speech** ([tts.md](tts.md))

| Keys | What they do |
|---|---|
| Super+S | speech on / off |
| Super+A | read the focused window, from the caret |
| Super+C | speak the highlighted text |
| Super+V | speak the clipboard |
| Super+W | what is under the pointer (the focused window with no mouse) |
| Super+R | repeat the last thing said |
| Super+E | switch between the recorded and the synthesised voice |
| Ctrl, tapped alone | stop speaking |

**Game mode** ([game_mode.md](game_mode.md))

| Keys | What they do |
|---|---|
| Super+Esc | game mode on / off |
| Super+Arrow | move the viewport |
| Super held + mouse | the viewport follows the pointer |

**The terminal** ([terminal.md](terminal.md))

| Keys | What they do |
|---|---|
| F11 | the Open bar -- go somewhere new |
| F12 | disconnect and return to the start panel |
| Esc | on the start panel: hide it to read the old session |
| Shift+PgUp / Shift+PgDn | scroll back / forward a page |
| Shift+Up / Shift+Down | scroll one line |
| Shift+Home / Shift+End | oldest line / back to live |
| Ctrl+Shift+C / Ctrl+Shift+V | copy the selection / paste |

In `term` and `text`, the titlebar's **Aa** icon switches between the
small and the large font; `term` keeps its 80x25 screen and resizes to
fit. Japanese is drawn at the large size, with **Japanese font** turned
on in `settings`.

**The text editor** ([text_editor.md](text_editor.md))

| Keys | What they do |
|---|---|
| Ctrl+N / Ctrl+O / Ctrl+S | new / open / save |
| Ctrl+Q | close |
| Ctrl+A | select all |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Ctrl+Z | undo |
| Ctrl+Y or Ctrl+Shift+Z | redo |
| Ctrl+F | find |
| Ctrl+G or F3 | find the same text again |
| Shift + any movement key | extend the selection |
| Tab | four spaces |

## The terminal

`term` is your main way of interacting with Zeitlos -- launch it from
the dock. It opens on a start panel: press **REPL** (Enter) for `repl`,
a command/scripting server, or **POSIX** for a Unix-style shell. The
first press starts the shell -- it takes a moment -- and connects to it. Most of what you'll want to do day to day
(run commands, transfer files, connect out to another machine, edit a
file, evaluate Scheme) happens from here.

At the `>` prompt, a few built-in commands:

| Command | What it does |
|---|---|
| `help` | list available commands |
| `ping` | check the connection is alive |
| `uptime` | show how long the system has been running |
| `free` | show memory usage |
| `te <filename>` | open a small text editor on a file |
| `telnet <ip-or-host>` | connect out to a remote telnet server |
| `quit` | end the session |

### Files and the network

`ls` lists files, and `tget`/`tput` transfer files over the network
via TFTP (networking starts automatically at boot, so these work out
of the box if you're on a wired network):

```
> ls
("/APPS" "/DOCS" "/ARK" "/USER")
> ls /APPS
("/APPS/FILES" "/APPS/TEXT" "/APPS/READ" ...)
> tget 192.168.1.100 firmware.bin
> tput 192.168.1.100 notes.txt
```

Applications live in `/APPS`, but you never have to type that: `run
term` searches the root, then `/APPS`, then the flash archive. See
`docs/flash_apps.md`.

### Connecting to a remote machine

```
> telnet bbs.machdyne.com
```

connects your `term` window out to a remote telnet server, the way a
classic terminal client would -- everything you type after that goes
to the remote system. Press **F12** at any time to disconnect; the
window returns to its start panel, where **REPL** gets you back to the
prompt.

### The text editor

`te <filename>` opens a small built-in editor. It uses familiar VT100
editing keys -- arrow keys and Page Up/Down to move around, `Esc :w`
to save, and `Esc :q` to quit. Files are limited to a couple of
kilobytes, so it's meant for quick edits, not large documents.

### Scheme

Anything you type that isn't one of the commands above is evaluated
as [Scheme](https://github.com/machdyne/ms) (a Lisp dialect), so
`term` doubles as a live programming console:

```
> (+ 1 2)
3
```

Most commands, including `ls`/`tget`/`tput` above, are really just
Scheme procedures you can also call as plain space-separated words --
`ls`, `tget 192.168.1.100 firmware.bin`, and their Scheme-syntax
equivalents `(ls)`, `(tget "192.168.1.100" "firmware.bin")` all do the
exact same thing. This also means you can draw directly on screen
from the prompt:

```
> (define w (win-create "hi" 200 100))
> (line w 10 10 190 90 1)
> (text w 10 10 "hello" 1)
```

(colors are `0` for black, `1` for white)

## Taking a screenshot

`ss` (typed at a kernel shell, not `term` -- see "Using the serial
console" below) saves the entire screen to `ss.bin` on the SD card.
Convert it to a viewable PNG on your computer with:

```
$ python3 tools/ssconv.py ss.bin
```

## Other apps

A few graphical demo apps are included, launchable from the dock:

- **gpu3d** -- a small hardware-accelerated 3D demo.
- **gpudemo** -- a graphics demo exercising the GPU's drawing
  hardware directly.

More apps will appear here as they're added.

## Using the serial console

Most day-to-day use happens through `term`, above -- but Zeitlos also
has a lower-level kernel shell, reachable over the same serial
connection used to build/flash the system (see the main `README.md`
for connecting and flashing). This is where you land before the
desktop starts, and it has its own separate command set (`ls`, `run`,
`ps`, `kill`, `xf`, `ss`, and more -- type `help` there to see them
all). A file name with a space in it is written in quotes there, as
in the posix shell: `cat 'My Notes.txt'` ([posix.md](posix.md),
"Quoting"). The graphical desktop (`wm`, `net`, `repl`) starts automatically
here a few seconds after boot; if you need to get a new app or file
onto the SD card without removing it, `xf` receives a file over the
serial connection (you'll need the `xfer` utility on the other end):

```
> xf myfile.bin
```

`xmf` does the same thing over XMODEM/CRC, which needs no host-side
tooling at all -- just the file-send your terminal program already
has (minicom's `Ctrl-A S`, picocom's `--send-cmd sx`, Tera Term's
File > Transfer > XMODEM > Send):

```
> xmf myfile.bin
```

Run the command first, then start the send; the receiver waits about
three minutes and prints `C` characters while it waits, which is how
it asks the sender for CRC mode.

Prefer `xf` for executables. XMODEM has no length field -- the last
block is padded out to a 128- or 1024-byte boundary and the padding
has to be guessed at on arrival -- so a file whose final bytes are
genuinely `0x00` or `0x1a` can come out slightly short. That does not
matter for most files, but the loader derives an executable's data
length from its size on disk, so `xf`, which carries an exact byte
count, is the safer choice there.

If something goes wrong and the desktop doesn't come up, this is also
where you'd start it manually with `init`, or investigate with `ps`.

For anything deeper than day-to-day use -- how the system is built,
the hardware it runs on, or how to write your own apps -- see the
rest of the documents in `docs/`.
