# The demos

Two scripted demos of Zeitlos, played by [`automate`](automate.md),
narrated by the system's own voice ([tts.md](tts.md)) with
[captions](captions.md). Everything is on the card in `/demo`:

| File | |
|---|---|
| `short.zds` | about 55 seconds, for social media: captions at 3x, one line each |
| `long.zds` | about four minutes, for YouTube: captions at 2x |
| `store.zds` | `long.zds` forever, in [attract mode](automate.md#attract-mode) |
| `demo.zds` | what `run automate` plays: `long.zds` |
| `zeitlos.svg` | a vector clock face — made by `gen_media.py` |
| `squirrel.pgm` | a photograph, grey, 1.6x — made by `gen_media.py` from `sw/data/images/squirrel.jpg` |
| `squirrel.jpg` | the original, for testing `view`'s JPEG path on its own |
| `lvb11.mid` | a MIDI file (from `sw/data/audio`) |

`release/lib/mkfatimg.py` ships everything in `sw/data/demo` except the
generator, plus the two files that live elsewhere in the tree. The
tracker modules stay in `/audio`; `track` and `midi` now also list
`/demo`, and `track` accepts a file to start with, as `midi` and `view`
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
`release/lib/mkfatimg.py` lists what goes in `/demo`.

**The recorded voice needs the speech pack**, `/speech/en.spk`. It is
built, not committed:

    ./tools/speech/speech fetch dist/en.spec
    ./tools/speech/speech align
    ./tools/speech/speech diphones
    ./tools/speech/speech build          # -> tools/speech/build/en/speech.zspk

and `zrelease sdcard` copies it to the card (or set `SPEECH_PACK`).
Without it the card builds with a warning, and `tts` speaks in the
synthesised voice throughout -- `voice recorded` changes nothing, and
`automate` says so on the console. With it, `tts` logs
`tts: lexicon from /speech/en.spk` when it starts; that line is the
check.

**Is the card current?** `automate` logs its build
(`automate: build ...`) and each script its revision
(`automate: -- short.zds, revision 4`), and the line count
(`automate: /demo/short.zds, 108 lines`). When unzipping an update over
the tree, let it overwrite (`unzip -o`), or the old scripts stay.
Then, from the serial console:

    > run automate /demo/short.zds
    > run automate /demo/long.zds
    > run automate /demo/store.zds

Running it again stops the one that is running and starts over.

The pointer needs a bitstream with the virtual mouse
(`Z_FEATURE2_VMOUSE`); flash gateware and software together as always.
Speech needs `tts` and its speech pack on the card, and a bitstream
with the audio mixer.

## Honesty

Everything on screen is Zeitlos doing it, live. An earlier version
showed a ray-traced scene that had been rendered on a PC and only
dithered on the board; it was dropped. The photograph is a photograph
(dithered on the board), and the SVG is drawn by `view`'s
own vector renderer on the board.

## If the recorded voice does not come

`tts` logs `tts: recorded voice from /speech/en.spk` when it has one.
On hardware, a `tts` started by `automate` loaded the pack's lexicon
but not its recorded voice, while the same card gave the recorded
voice to a `tts` started by hand afterwards -- cause not yet known.
`dsyn_open()` now says why it refuses (`tts: recorded voice: ...`).
Until that is settled, start `tts` first; `automate` uses the one that
is running. The whole setup, from the serial console:

    > run tts
    > run jfont
    > run automate /demo/short.zds

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
  own — `run view /demo/squirrel.jpg` from the console, nothing else
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
a line to `/user/cron.cfg` ([cron.md](cron.md)):

    at boot automate /demo/store.zds

cron hands the path over as the launch argument.
