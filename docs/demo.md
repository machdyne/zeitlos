# The demos

Scripted demos of Zeitlos, played by [`automate`](automate.md),
narrated by the system's own voice ([tts.md](tts.md)) with
[captions](captions.md). The scripts are `automate`'s, in its folder,
and the media they show are the card's examples, in `/media`
([layout.md](layout.md)):

| File | |
|---|---|
| `/data/automate/short.zds` | about 55 seconds, for social media: captions at 3x, one line each |
| `/data/automate/long.zds` | about four minutes, for YouTube: captions at 2x |
| `/data/automate/store.zds` | `long.zds` forever, in [attract mode](automate.md#attract-mode) |
| `/data/automate/demo.zds` | what `run automate` plays: `long.zds` |
| `/data/automate/lang.zds` | about 80 seconds, programming: Scheme, BASIC and C ([below](#the-languages-demo)) |
| `/data/automate/mandel.c` | the C program `lang.zds` compiles on the board: a 16-colour Mandelbrot |
| `/data/automate/slide.zds` | about 90 seconds, [slide](slide_app.md): a flowchart drawn live, a pitch printed to PDF, the deck shown, then a phone book built from nothing and used ([below](#the-slide-demo)) |
| `/data/automate/slide_demo.zsl` | the deck `slide.zds` opens (and adds a card to, without saving) |
| `/media/images/zeitlos.svg` | a vector clock face — made by `gen_media.py` |
| `/media/images/squirrel.pgm` | a photograph, grey, 1.6x — made by `gen_media.py` from `sw/data/images/squirrel.jpg` |
| `/media/images/squirrel.jpg` | the original, for testing `view`'s JPEG path on its own |
| `/media/audio/lvb11.mid` | a MIDI file (from `sw/data/audio`) |

`release/lib/mkfatimg.py` ships everything in `sw/data/demo` except the
generator, plus the two files that live elsewhere in the tree. The
tracker modules are in `/media/audio` too, which `track`, `midi` and
`play` list; `track` accepts a file to start with, as `midi` and `view`
already did.

## What they show

| | short | long |
|---|---|---|
| the system itself: one FPGA, a RISC-V CPU, GPU, mixer | ✓ (over a live desktop: `cal`, `calc`, `clock`) | ✓ (`info`) |
| the voice is Zeitlos too | | ✓ |
| windows and the pointer: dragging, pre-emptive multitasking | | ✓ (`clock`, `cal`) |
| typing English, German, French, Spanish and Japanese in `text` | ✓ (3: English, German, Japanese) | ✓ (5, with the on-screen keyboard following the layout) |
| reading Markdown in `read` (`/docs/welcome.md`, maximized, paged) | ✓ | |
| dithered images: a photograph; SVG rendered on the board | ✓ (photo) | ✓ (both, full screen) |
| the 3D cube, rotated with the pointer | ✓ | ✓ |
| drawing in `draw` | ✓ | ✓ |
| playing the piano in `midi` (Für Elise, live), then a MIDI file | ✓ | ✓ |
| a tracker module under a chess engine playing itself | | ✓ |
| the four phosphors | ✓ | ✓ |

Für Elise is Beethoven, 1810, and public domain; it is played on
`midi`'s tracker keyboard (`z`–`m` is one octave, `q`–`u` the next).

## Running them

Rebuild the card (`release/zrelease sdcard`); `DEMO_FILES` in
`release/lib/mkfatimg.py` lists what goes where.

**The recorded voice needs the speech pack**, `/data/tts/speech/en.spk`. It is
built, not committed:

    ./tools/speech/speech fetch dist/en.spec
    ./tools/speech/speech align
    ./tools/speech/speech diphones
    ./tools/speech/speech build          # -> tools/speech/build/en/speech.zspk

and `zrelease sdcard` copies it to the card (or set `SPEECH_PACK`).
Without it the card builds with a warning, and `tts` speaks in the
synthesised voice throughout -- `voice recorded` changes nothing, and
`automate` says so on the console. With it, `tts` logs
`tts: lexicon from /data/tts/speech/en.spk` when it starts; that line is the
check.

**Is the card current?** `automate` logs its build
(`automate: build ...`) and each script its revision
(`automate: -- short.zds, revision 4`), and the line count
(`automate: /data/automate/short.zds, 108 lines`). When unzipping an update over
the tree, let it overwrite (`unzip -o`), or the old scripts stay.
Then, from the serial console:

    > run automate /data/automate/short.zds
    > run automate /data/automate/long.zds
    > run automate /data/automate/store.zds

Running it again stops the one that is running and starts over.

The pointer needs a bitstream with the virtual mouse
(`Z_FEATURE2_VMOUSE`); flash gateware and software together as always.
Speech needs `tts` and its speech pack on the card, and a bitstream
with the audio mixer.

## The languages demo

`lang.zds`, "interpreted languages": three languages built in, each
typed live.

    > run posix
    > run repl
    > run automate /data/automate/lang.zds

Start the two services first. The script begins with `need posix0` and
`need repl0` ([automate.md](automate.md)), which pass at once when they
are running and stop the demo with a reason when one is not -- so
nothing is waited on in front of the camera.

**It is silent**: captions only. It begins with `speech off`, which
stops a `tts` left running from something else -- otherwise `wm` would
announce every window that takes the focus -- and it uses no `narrate`
or `say`, either of which would start `tts` again.

| | What happens |
|---|---|
| stage | `term` at the top left; its start screen stays up 1.5 s, then REPL |
| Scheme | six lines typed into `term`: a window, a line-art "fan", `help button` (the built-in reference -- every procedure has an entry: how to call it, what it does, an example), a handler, and a button; the pointer clicks it and the art fills in; then `(win-destroy w)` closes the window again |
| BASIC | opened at the lower right, beside the terminal, only now (so it is not under the terminal while Scheme is typed); one line of circles and `RUN` |
| C | `port posix0` in the same terminal, `cat` the source, then `zcc /data/automate/mandel.c -o /ram/mandel && /ram/mandel` on the board -- absolute paths, because posix gives a program its arguments but not the shell's directory, so `-o mandel` after `cd /ram` did not land in `/ram`: full screen, 16 colours, row by row, then 4 s of colour cycling, and the screen comes back |

Captions throughout: "Three languages: Scheme, BASIC and C" at the
start and "Zeitlos. Timeless computing." at the end.

**The Scheme** is one complete form per line, because `repl` reads each
line as a form ([scheme.md](scheme.md)), and every line fits `term`'s 80
columns with the prompt. The click mirrors the fan along the top as well and draws both twice
as densely -- two curves become a web of four; the new lines include
the old ones, so nothing is erased.

**`mandel.c`** is 4.12 fixed point (`zcc` has no floating point), 32
iterations, with the main-cardioid and period-2-bulb tests, 32 pixels
per plane built in plain variables and stored as one word per quadrant
(`docs/color.md`, the 16-colour layout). The set is symmetric about the
real axis, so the top half is computed and the bottom half mirrored.

How fast, at the board's 6.98 MIPS: the first version was 89M
instructions (13 s); the tests, symmetry and plain variables made it
37.5M; `zcc`'s peephole ([zcc.md](zcc.md#the-peephole)) makes the same
code 17.8M -- about 2.6 s.

**It waits four frames after taking the screen** before clearing it.
The grab is a message, and `wm` -- with `term` still printing the
compile's output -- could still be drawing when `mandel` cleared,
through the line engine and blitter, which finish on their own: thick
stray lines at the top and bottom when it started.

**It includes no headers.** It declares the three runtime calls it
makes and names the five registers it writes. With `libz.h`, `zsoc.h`
and `zwm.h` it was 67.6M instructions to compile and needed more than
`zcc`'s 4MB -- on the board, out of memory after 30 s. Without them it
is 2.6M instructions in under 1MB (measured with the device `zcc` on the
simulator): almost all of a compile is reading headers
([zcc.md](zcc.md#what-it-costs-to-run)). Checked off the board: compiled by the host `zcc`
against libz and run on the simulator with its framebuffer pointed at
memory (the simulator's VRAM is only 512x384), every one of the 76,800
pixels matches a GCC build of the same function. It refuses, with a message, on a board
without game-mode colour.

**`cat` hanging, then "cannot open".** Not the typing: `fs` (the
filesystem driver) keeps a reply slot for each of its six most recent
clients until that client has read its reply -- sent again, died, or
256 more requests went by. On a quiet machine the 256 never came: by
the C part six processes that had each touched a file once (automate,
basic, repl, term, wm, ...) held all six, and `posix` was told "try
again" until it gave up -- blocked inside the call meanwhile, so the
terminal looked disconnected. One take hung, the next said `cat: ...:
cannot open` after about 3 s and then `run: zcc: cannot start`. By
hand, with fewer programs about, it worked. A slot is now also free
once its reply is a second old ([fs_driver.md](fs_driver.md)). `fs`
runs from flash: it needs `make dev-flash`, not just a new card.

**The narrator lease** lasts 30 s and speaking renews it, but this demo
is silent for longer than that. When it ran out, `wm` announced every
window that took the focus ("Scheme", "BASIC", "term0") over the demo;
`automate` now renews it every 10 s while it waits.

**Focus.** The Scheme window takes the focus when `win-create` opens
it, and the keys with it; the script gives them back with `focus term`
before typing on. The click on its button does the same, which is why
the window is closed with `focus term` first, then `(win-destroy w)`.

**The fractal is waited for, not timed**: `await-fullscreen` returns
when `mandel` gives the screen back, however long the board takes to
compile and draw.

**Typing** is 16 ms a character (`type-speed 16`), from 25 in two
steps of a quarter.

**Term's text inside the BASIC window.** basic used to open a window,
measure wm's frame from it, close it and open the real one, in the same
place. In that moment term, underneath, got the area back, was asked to
repaint it, and lost it again, and its text and scrollbar were left in
BASIC's screen. Resizing the first window instead hung the demo once
in two takes. The frame is a constant (`Z_WIN_CHROME_W`/`_H`,
`zwin.h`), so basic now opens one window at the right size and nothing
else ([`basic_app.c`](../sw/apps/basic/basic_app.c), `open_window()`).

**Tidying up** happens after the end caption: back to REPL (F12) for
the next take, then `reset`.

## The slide demo

`slide.zds`, about 90 seconds: [slide](slide_app.md) walked through --
drawing, a pitch printed, the show -- ending with an interactive deck
built in it from nothing.

    > run automate /data/automate/slide.zds

| | What happens |
|---|---|
| a deck | `slide` opens `slide_demo.zsl` at (40, 32): a title card. "slide: design and print decks" |
| a flowchart, live | Ctrl+M, a new card; `/pill Start`, `/diamond Ready?` (`/fill 2`), `/cylinder Saved`, each dragged into place; two arrows drawn with the arrow tool from shape to shape, which connects them; the diamond and the store dragged, and the arrows follow; `/transition iris 600` |
| a pitch, printed | `/go pitch`: a fictional seed-round card (EUR 10M for a toaster screensaver, a bar chart of toasters planned); `/print this /tmp/pitch.pdf` -- a PDF named on the line, so no dialog and no paper; the print window shows the job and closes itself; `focus slide` gives the keys back to slide (without it, Shift+F5 went nowhere) |
| the show | Shift+F5: the title, the flowchart (an iris), the pitch, the next card; Esc |
| an interactive deck | "Build interactive slide decks": Ctrl+N ("Save the deck first?" No); a title and two labels with the text tool; `/field` twice (`/name name`, `/name phone`); `/button Add` with `/action (add-item "book" (take-text "name") "  " (take-text "phone")) (save-deck)`; `/button Delete` with `/action (remove-item "book") (save-deck)`; `/list Ada Lovelace  1815, Alan Turing  1912`, made larger by its corner, `/name book`; `/app on`; `/save as /tmp/phonebook.zsl` ("y" if a run before left one) |
| using it | F7: the window is the card. "Grace Hopper", "1906", Add: the record is in the list, the fields empty, the deck saved. Ada chosen, Delete |
| the end | `slide` killed, the repository's address, and "Zeitlos" |

The finished app is also an example deck,
`/data/slide/examples/phonebook.zsl`.

The pointer positions are the window's content: the editor's card at
(30, 6) at 100% (548 x 384 inside); a deck run as an app has its card
at (0, 0) whatever the window's size (so a click lands before wm has
made the window the card's size as well as after). The same steps,
at the same positions, run on the build machine in `make -C
sw/apps/slide render` (`tests/render_ui.c`), which checks what the
demo makes -- Ctrl+M as wm sends it (13) making an empty card, three
shapes where the drags put them, connectors that move with the
diamond, `/go pitch`, the PDF named on the print line, the phone
book's objects where the drags put them, the records after Add and
Delete, and the saved file holding them -- and draws
`/tmp/slide_ui_demo.pbm`, `_demo_build.pbm` and `_demo_app.pbm`.

**A pause after every drag.** A 500 ms drag is about 30 pointer
messages, and an app's mailbox holds 32: keys typed straight after one
could find slide's full and be lost. That lost the phone book's title
once (the text box, still empty, went with Esc). Every drag in the
script is followed by `pause 400`.

**Typing speed.** wm delivers each key as a message, and a full
mailbox (32) loses the rest. Code is typed at 40 ms a character, not
faster: in revision 3 the Add button's action, typed at 18 ms, could
lose a character and leave a button that did nothing.

**Captions at the top.** At the bottom, a 3x caption sits at y
372-423, just above the dock (y 432), and covered slide's command line
-- the typing the demo is about. At the top it is y 8-59; the window
opens at y 32, so the caption covers its title bar and nothing else,
and its 399 pixels end above the dock.

One `slide` from start to end, started on an ordinary deck. Revisions
1 and 2 started `slide` on the calculator example, an app deck; both
times the script got no further than that window appearing on the
board (not reproduced on the build machine: `render appstart` starts
slide that way and goes on). Captions go by themselves after a minute
at most ([captions.md](captions.md)).

The narration follows the rule: each line is said, then the thing
happens under its caption.

## Honesty

Everything on screen is Zeitlos doing it, live. An earlier version
showed a ray-traced scene that had been rendered on a PC and only
dithered on the board; it was dropped. The photograph is a photograph
(dithered on the board), and the SVG is drawn by `view`'s
own vector renderer on the board.

## If the recorded voice does not come

`tts` logs `tts: recorded voice from /data/tts/speech/en.spk` when it has one.
On hardware, a `tts` started by `automate` loaded the pack's lexicon
but not its recorded voice, while the same card gave the recorded
voice to a `tts` started by hand afterwards -- cause not yet known.
`dsyn_open()` now says why it refuses (`tts: recorded voice: ...`).
Until that is settled, start `tts` first; `automate` uses the one that
is running. The whole setup, from the serial console:

    > run tts
    > run jfont
    > run automate /data/automate/short.zds

## Claims

Every sentence has to be true of the board it is recorded on. Two were
not: "it types in every language" (it is 22 keyboard layouts, Latin-9
and Japanese kana), and "a RISC-V processor we designed ourselves" --
the default core is PicoRV32; Zeitlos32 is ours, but only a board built
with `` `CPU_ZEITLOS32 `` runs it (`info` says which). If yours does, say
so. "A USB host" became "USB": only one board builds `` `USB_HOST ``.

`view` dithers while it decodes, but shows the picture only when it is
finished -- so the scripts do not say "as they load" or "a row at a
time": nobody can see that happen.

## Spelling for the ear

Each `narrate` line is `caption | spoken`, and the spoken half is
spelled for how it should SOUND. Without a speech pack `tts` falls back
to its built-in letter-to-sound rules, which get many ordinary words
wrong: "timeless" came out "timless", "designed" as "dizzig-nd",
"together" as "tog-a-t-h-er", "computer", "photos", "MIDI" and "RISC-V"
wrong too. The scripts say "cum pewter", "de zined tuh gheh thur",
"Time less", "risk five" instead; the captions stay spelled normally.

Check a line before putting it in a script -- this runs the same front
end as the board:

    cd sw/apps/tts
    cc -std=gnu99 -no-pie -I ../../common -DPACK_HOST -o /tmp/t2p \
       tests/t2p_cli.c text2ph.c lts.c pack.c phon.c synth.c dsyn.c
    echo "Time less cum pewting." | /tmp/t2p

It prints the phonemes (ARPAbet). With a speech pack on the card its
lexicon knows the ordinary spellings, and the respellings still sound
right. `tts` also takes raw phonemes: an utterance that is entirely
`[...]` is spoken as written (`text2ph_chunk()`).

A sentence whose twelfth word came just before its full stop used to
end in "dot": the chunker cut at twelve words and the lone "." was
spelled. Fixed in `text2ph.c`; the scripts also avoid it, so they work
on a `tts` from before the fix.

## The voices

Both demos open in the synthesised (formant) voice and hand over to
the recorded one — `voice synth` at the top, `voice recorded` after the
first line. The synthesised voice mangles "computer" and "FPGA", so its
line uses neither.

`jfont` (the Japanese font) is not started by the scripts: start it
before the demo. Loading it reads the card, and on the first run it
loaded while the voice was also reading the card, and the opening
sentence suffered.

## Making the video

- **Capture** the HDMI/DVI output with a capture device, and the audio
  output at the same time. Everything on screen and everything heard
  comes from the board; nothing is added afterwards.
- **Start from the serial console**, so no launcher window is in the
  shot. Both scripts begin with `interruptible off`, so a bumped mouse
  does not end the take (attract mode ignores it).
- **Framing.** The screen is 640x480 (4:3). Both demos keep windows
  in the top three quarters and the caption band above the dock. For a
  1:1 or 9:16 crop, switch the short demo to
  `caption-style compact` so the caption box stays in the middle.
- **Phosphor.** White records cleanest. Amber or green make a striking
  thumbnail; `video amber` at the top of a script changes the whole
  demo.

## Tuning

The scripts are data: nothing needs to be rebuilt to change them. The
numbers most likely to need adjusting on real hardware:

- **Window positions and pointer coordinates** (`run ... at`, `drag`,
  `click`). `origin APP` makes coordinates relative to an app's content
  area, so the `draw` block keeps working wherever the window is. The
  tool buttons in `draw` are 20 pixels square from the content origin;
  the canvas starts at x=43; the pattern swatches are 16 pixels wide
  along the bottom.
- **The squirrel.** The first runs used `squirrel.jpg`, and `view`
  never finished it under the demo. The demo now uses a PGM made from
  it, which needs no decoder. Whether `view` finishes the JPEG on its
  own — `run view /media/images/squirrel.jpg` from the console, nothing else
  running — is the next thing to find out.
- **Pauses after `run`.** An app that loads from the card and draws
  a lot before its first frame may need a `pause` after `run`.
- **Speech never overlaps action** (second hardware run). Even typing
  beside the voice made it stutter, so `narrate` and `say` always wait,
  and the demos no longer use `narrate&` at all. What follows is why.
- **Speech overlap.** The recorded voice renders at 55-60% of the CPU
  (`tts: [... ms of speech, ... ms to render: 60% of one CPU]`), so it
  talks over light work only: typing, the pointer, drawing, a still
  picture. On the first hardware run it fell behind (`tts: fell behind
  by ... ms -- heard as gaps`) exactly where it overlapped image
  decoding and the spinning cube, so those now show a caption while
  they work and the sentence comes before or after. Keep to that when
  editing: an app launch, a decode, the cube or the chess engine go
  after a `sync`.
- **Pace.** `rate` for the voice, `type-speed` for typing, and the
  tempo argument of `melody`.

Every command is logged on the console as it runs (`automate: ...`),
with warnings for anything it could not do: an app that made no
window, a character not on the layout, a speech mark that timed out.

## In the store

`store.zds`: the long demo on a loop. Touch the keyboard or mouse and
it stops mid-sentence and leaves the machine to you; leave it alone for
a minute and it tidies up and starts again. To start it at boot, add
a line to `/data/cron/cron.cfg` ([cron.md](cron.md)):

    at boot automate /data/automate/store.zds

cron hands the path over as the launch argument.
