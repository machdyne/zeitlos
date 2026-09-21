# Speech (text-to-speech)

Zeitlos can speak. An app says something with one call, the window
manager gives the user a handful of Super keys to drive it, and a
service, `tts`, does the talking. With speech off the service is not
running at all, and asking it to speak costs an app about fourteen
instructions.

    sw/common/ztts.h        the protocol (subjects, flags, marks)
    sw/common/zspeak.h/.c   what apps call
    sw/apps/tts             the service
    sw/apps/tts/tts_queue.c its queue, host-testable
    sw/apps/tts/synth.c     the formant synthesiser
    sw/apps/tts/phon.c      phonemes -> synthesiser frames
    sw/apps/tts/text2ph.c   text -> phonemes: words, numbers, symbols
    sw/apps/tts/lts.c       letter-to-sound rules
    sw/apps/tts/tts_audio.c the voice on a mixer channel
    sw/common/zsayall.h/.c  reading a document aloud, for any app
    sw/apps/ttstest         on-target cost measurement and protocol checks
    sw/apps/wm              the Super keys (speech_hotkey(), wm.c)

**Status: phase 4 of 9, and the plan grew** -- learning the rules from
public-domain data is now most of what is left, and it lives in
a separate data pipeline, not yet published.

**Phase 3 status:** The protocol, the keys, reading a window
(`text` so far), the voice and the text front end are built.
everything since was built so it can be re-judged by ear.

See [Phases](#phases).

---

## Who this is for

- **Blind and low-vision users.** The primary case. Everything on the
  desktop is already reachable from the keyboard
  ([window_manager.md](window_manager.md), "Keyboard-only operation");
  speech makes it reachable without the screen.
- **A headless machine.** Headphones and a keyboard, no monitor:
  launch `text`, write, have it read back.
- **Non-standard input.** Switch access, brain-computer interfaces,
  anything that drives the keyboard path. Keyboard-only mode already
  exists; this gives it feedback.

All three depend on one thing: **the speech has to be understood.**
Robotic is fine; unclear is not. That is the gate for phase 2, and
the reason phase 2 starts with a listening test on the build machine
rather than with code on the board.

---

## Using it

### The keys

Handled by `wm`, never forwarded to apps.

| key | action |
|---|---|
| **Super+S** | speech on / off |
| **Super+A** | read the focused window |
| **Super+C** | speak the clipboard |
| **Super+W** | what is under the pointer -- the window, or the dock icon; with no mouse attached, the focused window |
| **Super+R** | repeat the last thing said |
| **Ctrl**, tapped alone | stop speaking |

Super+S starts `tts` if it is not running (it says "Speech on") and
tells it to quit if it is (it says "Speech off", then exits). With
speech off the other keys do nothing, silently -- there is nothing
that could say otherwise -- but they are still consumed, so they never
reach an app as a stray letter.

**Why Super.** Nothing else in the system binds Super with a key, and
game mode already uses Super (held, with the mouse) for its magnifier
-- the other accessibility feature -- so the modifier reads as "the
system's own assistive functions". Alt is window management, Ctrl
belongs to apps.

**Why a Ctrl tap stops speech.** It is the convention in every
mainstream screen reader, so it is what a user's hands already do. A
Ctrl press with no other modifier arms it, any other key press
disarms it, and the Ctrl release fires it -- so Ctrl+C in a terminal
never stops speech and a lone tap always does. It needs the raw
modifier events `sw/os/hid.c` synthesises (usage `0xE0`/`0xE4`),
because a bare modifier has no keysym.

**Super+A** is a toggle, and the app keeps the state -- see
[Reading a window](#reading-a-window). On a window whose app does not
support reading it says the title and "No readable text", so the key
is never silent. On the dock it explains how to use the dock.

**Super+W** hit-tests the pointer exactly as a click would. Over a dock
icon it gives the app's name as a person would say it ("Calculator",
not `calc` -- `dock_spoken_name()`, `wm.c`).

**Remote desktop.** The browser client forwards the Meta key
(`esp32/zeitlos-nic/web/index.html`), but the host operating system
usually takes the Windows/Super key before the browser sees it. A
remap in the web client is future work.

### Turning it on for someone else

Somebody will usually set a machine up for a blind user. Today: put
`tts` on the sdcard (the release image does) and show them Super+S.
Rate, pitch and volume are `system.tts.rate`, `.pitch` and `.volume`
in `/zeitlos.cfg` ([config.md](config.md)); the service rereads them
whenever the file is reloaded. Phase 4 adds `system.tts.enabled`, which
`init` will read to start speech at boot. Whether `tts` becomes a core app
in flash is an open decision ([flash_apps.md](flash_apps.md)).

### Testing it

From the serial console:

    > run ttstest

Best with speech off, so it measures that path first. It then starts
`tts` itself, measures the send path, runs the protocol checks
(ordering, interrupt, low priority, stop, spell, repeat -- each printed
PASS or FAIL), and stops `tts` if it started it. The transcript lines
(`tts: ...`) interleave with its output.

On the build machine:

    cd sw/apps/tts && make test

runs the queue's and the client's test suites; see [Testing](#testing).

---

## What speaks by itself

With speech on, these announce without an app doing anything:

| what | says |
|---|---|
| keyboard focus moving between widgets | "Save, button" / "Wrap lines, check box, checked" / "Volume, slider, 40" / "Print, button, dimmed" |
| a window taking focus -- a click, Alt+Tab, or a window opening | its title, or "Dock" |
| arrowing along the dock | the app's name ("Calculator", "Next page") |
| moving through a list | the row and where it is: "readme.txt, 3 of 12" |
| editing in `text` | the character stepped over, the line arrived on, the word just typed, the character deleted ([text_editor.md](text_editor.md)) |
| a dialog opening | its title and question: "Save as. Type a name." |

The list box says its position as well as its row, because that is
what a sighted user gets from the scrollbar and nobody else gets at
all -- and only when the list has keyboard focus, so a list scrolling
for some other reason does not talk over whatever is being said. A
dialog speaks as it opens, since it is the one moment the screen
demands an answer; its buttons then announce themselves as focus
reaches them.

The widget announcements come from `z_widget_focus_set()` in
`sw/common/zwidget.c`, which is the one place focus moves, so every
app using the toolkit speaks -- `z_widget_announce()` is there for the
rare case of announcing something focus did not move to. The window
ones come from `wm`. All of them interrupt: holding Tab or Alt+Tab
speaks where you land, not everything you passed.

The cost when speech is off is the same fourteen instructions as any
other `z_speak()` (below), which is why none of this is conditional on
a setting.

**Starting at boot.** `system.tts.enabled` in `/zeitlos.cfg`
([config.md](config.md)) makes `init` start the service, after the
desktop is up so there is something to announce. Off by default;
Super+S still works either way. It is what you set on a machine being
prepared for someone who cannot see it, so they are told something
before having to find a key.

## Audio: mixer or FIFO

`tts` speaks through whichever the bitstream has (`rtl/boards.vh`:
`AUDIO_MIXER` is optional, and the expensive half):

- **The mixer**, on channel 7 (`Z_AUDIO_CH_SPEECH`): the hardware reads
  the voice's ring by DMA, alongside whatever else is playing, and
  `play`, `track` and `midi` leave that channel alone while `tts` runs.
- **The plain FIFO**, when there is no mixer: the service feeds it
  itself, at 11kHz (44.1kHz with each sample repeated, over S/PDIF,
  which cannot run that slow). Nothing mixes in this mode, so speech
  and another player take turns -- whichever started last wins the
  FIFO's rate.

The service says which at startup (`tts: audio: ...`), and says so
when a channel or the FIFO stops consuming rather than falling silent
without a word: an early version on a board without the mixer passed
every protocol check and made no sound, and nothing in the log said
why. `ttstest` also prints the audio hardware it finds.

## Performance

The first run on hardware (48MHz, rv32im) measured synthesis at **about
80% of one CPU**, and launching a program starved it: speech cut out.
Two causes, both fixed:

| | before | after |
|---|---|---|
| instructions per sample, vowel | 202 (average) | 98 |
| instructions per sample, fricative | 202 (average) | 120 |
| memory accesses per sample | 73 | 17-21 |
| estimated share of the CPU | 74% (measured: ~80%) | 36-44% |
| audio rendered ahead | 370ms | 1.5s |

- **Block processing** (`synth.c`): each resonator runs across a whole
  block with its coefficients and state in registers, instead of every
  sample being carried through every stage with a call and seven memory
  operations per resonator. Bit-exact: the output is identical to the
  sample-by-sample version, checked over the whole test set.
- **Silence is skipped**: a resonator with no input and nothing left
  ringing is not run -- during a fricative that was the whole vowel
  cascade, doing nothing, every sample. "Nothing left" is within +/-2,
  because integer resonators can idle at +/-1 forever; dropping that
  changes the output by at most 53dB below the signal.
- The synthesiser is compiled `-O2` (the rest of the app stays `-Os`),
  and the pitch contour's per-frame pass over every segment of the
  utterance is now worked out once per utterance (bit-exact).
- The ring holds 1.5s instead of 370ms, rendered in 200ms slices so a
  stop or interrupt is still answered at once.

**Measuring:** `make prof` compiles `synth.c` for rv32im and counts
instructions and memory accesses per sample in an emulator
(`pip install unicorn`). At about 16 cycles per instruction on the
board, that model reproduced the first hardware measurement, so it is
worth running before a change goes to a board. `ttstest` and the
service's own `[... % of one CPU]` lines are the real measurement.

## Voice settings

All in `/zeitlos.cfg` ([config.md](config.md)), read again whenever the
file is reloaded:

| key | default | range | |
|---|---|---|---|
| `system.tts.voice` | `male` | `male`, `female` | sets pitch and tract size together |
| `system.tts.rate` | 180 | 80-450 wpm | |
| `system.tts.pitch` | 110 (200 female) | 50-300 Hz | overrides the voice |
| `system.tts.formants` | 100 (117 female) | 85-120 % | vocal tract size; overrides the voice |
| `system.tts.expression` | 100 | 0-200 % | how much the pitch moves; 0 is a monotone |
| `system.tts.volume` | 200 | 0-255 | |

**Female is not just higher.** A woman's vocal tract is about 17%
shorter, which raises every resonance -- F1 to F5 -- by that proportion.
Pitch alone makes a squeaky male voice, so `female` sets both.

Getting that right found a real bug: the synthesiser kept F4 and F5 at
fixed frequencies, so raising F1-F3 crowded F3 into F4, and two
resonators that close multiply each other's peaks. The first female
voice came out 12dB louder on aspiration and clipped. F4 and F5 now
move with the rest of the tract (`synth_set_tract()`).

**Loudness is held constant** across the range, measured over the test
set: every combination of pitch 50-300Hz and tract 85-120% is within
2dB of the default voice. The tract range is limited to 85-120% because
that is what was measured not to clip at every pitch; 125% clipped.

**Expression** scales every movement of pitch around the base at once
-- declination, stress, the rise at a comma, the fall at a full stop.
Many people turn it down at high speaking rates, where intonation
becomes a distraction rather than a help.

`make wav` takes the same settings from the environment for listening
tests: `ZTTS_VOICE=female`, `ZTTS_PITCH`, `ZTTS_FORMANTS`,
`ZTTS_EXPRESSION`.

## For app authors

    #include "../../common/zspeak.h"      // and link zspeak.o

    z_speak("Save, button", Z_TTS_F_INTERRUPT);

That is the whole API for most purposes. **Do not guard it** with a
setting or a check of your own: with speech off it returns false in a
few instructions, and that is the intended way to be silent.

| call | |
|---|---|
| `z_speak(text, flags)` | speak a NUL-terminated string, up to 127 bytes |
| `z_speak_n(text, len, flags)` | the same, for `len` bytes of a larger buffer |
| `z_speak_mark(text, len, flags, mark)` | and be told when it has been spoken |
| `z_speak_static(text, flags)` | no copy, no length limit -- for literals and static buffers only |
| `z_speak_stop()` | stop, discard the queue |
| `z_speak_repeat()` | say the last thing again |
| `z_speak_set(param, value)` | rate, pitch, volume (`Z_TTS_PARAM_*`) |
| `z_speak_available()` | is speech on? only to skip building an expensive string |

### Flags

| flag | |
|---|---|
| *(none)* | queue behind whatever is being said |
| `Z_TTS_F_INTERRUPT` | discard everything, say this now -- **use for focus changes**, so holding an arrow key speaks only where the user lands |
| `Z_TTS_F_SPELL` | read it character by character |
| `Z_TTS_F_LOW` | drop it if anything is being said or queued |
| `Z_TTS_F_CONTINUES` | runs on into the next utterance: no final pause or fall |

### What to say

Speak what a sighted user would take in at a glance, most important
first: the name, then the role, then the state. "Wrap lines, check
box, checked." "Save, button." "readme.txt, 3 of 12." Leave out
anything the user has just heard, and anything visual that carries no
meaning ("blue", "bold").

### Why the text is copied

A string payload is **borrowed** ([messaging.md](messaging.md)): the
receiver reads the sender's bytes in place, later. A caller speaking
from a stack buffer would have returned and reused that stack before
`tts` got round to reading it. So `z_speak()` copies into a four-slot
static ring and sends a pointer into the ring; a slot is reused only
four sends later, and `tts` copies on read and is woken by every send.
That is the same narrowing `wm` uses for `Z_WM_WINDOW_MOVED`, and for
the same reason: it is a message sent in response to something the
user can do repeatedly, so its payload must not be allocated.

`z_speak_static()` skips the copy for text that will not change --
`wm` uses it for the clipboard (4KB, far larger than a ring slot) and
for its fixed phrases.

### Reading a window

A window created with `Z_WIN_FLAG_READABLE` (`zwm.h`) gets `Z_WM_READ`
when the user presses Super+A on it. The app reads its own content:

- **Toggle.** If not reading, start from where the user is -- the
  cursor, or the top of what is visible. If reading, stop. The app
  keeps this state; `wm` cannot, because a read ends by itself when
  the text runs out.
- **Pace on marks.** Send a line or a sentence at a time with
  `z_speak_mark()`, keep one or two ahead, and send the next when a
  `Z_TTS_MARK_DONE` arrives. Scroll to follow the line being spoken.
  Do not send the whole document: that fills the service's queue,
  and a Ctrl tap would throw away everything the user has not heard
  rather than leaving the position where speech stopped.
- **Stop on navigation.** Any key that moves the cursor or the view
  stops the read (`Z_TTS_F_INTERRUPT` on whatever it speaks next does
  that). The position stays where speech stopped, so moving down and
  pressing Super+A again reads from the new place -- the `read` app's
  "read, skip ahead, read" is exactly this.
- **Never wait forever** on a mark. Every accepted marked utterance
  gets exactly one DONE or CANCELLED, but a reply can be lost if the
  app's own mailbox is full; use a timeout.

`sw/common/zsayall.h` does all of that for any app that can hand it
"unit *n*" -- see its header for the five lines of wiring. **`text`
uses it** (one unit per display line, the caret following the voice,
Super+A at the end of the document reading it from the top, since
"read me what I wrote" is the commonest reason to ask). `term` and
`read` follow in phase 5; until then Super+A on them says the title
and "No readable text".

A unit that runs on into the next -- a line that wrapped in the
middle of a sentence -- carries `Z_TTS_F_CONTINUES`, so the voice does
not pause and fall in pitch as if a sentence had ended.

---

## The protocol

`sw/common/ztts.h`. The service registers `tts` and so answers to
`tts0`.

| direction | subject | payload | |
|---|---|---|---|
| app → tts | `Z_TTS_SAY` | `Z_STR`; tag = `Z_TTS_TAG(flags, mark)` | speak |
| app → tts | `Z_TTS_STOP` | none | stop, discard the queue |
| app → tts | `Z_TTS_REPEAT` | none | say the last utterance again |
| app → tts | `Z_TTS_QUIT` | none | say "Speech off" and exit |
| app → tts | `Z_TTS_SET` | `Z_UINT32`, `Z_TTS_SET_PACK(param, value)` | voice setting |
| tts → app | `Z_TTS_MARK_DONE` | `Z_UINT32` mark; tag = mark | a marked utterance was spoken |
| tts → app | `Z_TTS_MARK_CANCELLED` | `Z_UINT32` mark; tag = mark | ...or will not be |
| wm → app | `Z_WM_READ` | `Z_UINT32` window id | Super+A on a `Z_WIN_FLAG_READABLE` window |

**Subjects are `0x5454xxxx`** ("TT"), nowhere near any other subject in
the tree. That is deliberate: a cached pid can briefly outlive the
service ([Cost](#cost)), and a message that lands on an unrelated
process must be one it has no case for.

**Marks.** A 16-bit non-zero value in the top half of the tag. Every
marked utterance the service accepts gets exactly one reply, to its
sender, in order -- including ones it refuses outright (queue full,
`Z_TTS_F_LOW` while busy), which are reported CANCELLED at once. An
unmarked utterance gets nothing.

**Why the service exits rather than muting.** With `tts` gone, every
`z_speak()` in the system takes the cheap path and the memory comes
back. A muted service would leave every app paying for a real send to
a process that discards it.

**One instance.** A second `tts` looks up `tts0`, finds it, and exits.
Two started at the same instant can both miss; the one that is not
granted `tts0` exactly exits too.

---

## The queue

`sw/apps/tts/tts_queue.c`. No I/O and no malloc, so it runs on the
build machine under test.

- 32 utterances, sharing an 8KB text ring. An utterance is never
  split across the end of the ring -- if it does not fit it starts
  again at 0 -- so each one is a plain C string the backend can walk.
- Text is copied, and cleaned, the moment the message is read:
  control characters become spaces (a terminal line's escape sequences
  must not reach a synthesiser as bytes), newlines are kept as pauses,
  and anything past `Z_TTS_UTTER_MAX` (4096, the clipboard's size) is
  cut.
- `INTERRUPT` stops the backend, cancels everything, then queues.
  `LOW` is refused if anything is queued or playing.
- **Empty utterances are kept.** A reader pacing itself on marks sends
  blank lines too, and must still hear DONE for them, or it would stop
  at the first paragraph break.
- `REPEAT` repeats what was last *started*, so during a long read it
  repeats the current line, not the whole document. Up to 1KB.

---

## Cost

What an app pays per `z_speak()`, from the code `gcc -Os` generates
for rv32im (read off `ttstest.dasm`; confirm on hardware with
`ttstest`):

| case | what happens | cost |
|---|---|---|
| speech off, cached | `rdcycle`, two loads, a subtract, three branches, return | **14 instructions**, no stack frame, no stores |
| speech off, cache stale | one `z_pid_lookup()` table scan | at most twice a second, only when something speaks |
| speech on | copy up to 127 bytes, one `z_msg_send()` of a 24-byte envelope | estimated ~150-200µs; `ttstest` measures it |
| never called | section GC drops it | nothing |

**Memory:** 512 bytes of `.bss` for the ring, only in an app that calls
`z_speak()`/`z_speak_n()`/`z_speak_mark()`. `wm` grew by about 4KB of
code, strings and the dock name table.

**The cache.** Both answers, present and absent, are kept for half a
second of `rdcycle` time -- not `z_uptime_ticks()`, which is a syscall.
The counter wraps every ~89s and the unsigned subtraction is correct
across the wrap. A failed send drops the cache at once, so a service
that has just exited is noticed on the very next call rather than half
a second later.

**Keeping the fast path fast.** The first version tested the cache and
then made an ordinary call to the lookup; that was enough for gcc to
build a stack frame on entry for every caller, speech on or off,
roughly doubling the common case. The entry points are now written so
every exit is a plain return or a tail call (`ZS_SAY()`, `zspeak.c`).
**Check the disassembly if you restructure it** -- the `.dasm` every
app build leaves is enough.

**A stale pid.** Between the service exiting and a cached "present"
expiring, a pid can in principle be reused by a new process, which
would then receive a `0x5454xxxx` subject it ignores. Bounded to half
a second after someone has just turned speech off. A kernel-exported
"tts pid + generation" word would close it completely and reduce the
speech-off path to a single load; not done, because nothing has shown
it to be needed.

**What speaking costs the service** is the synthesiser's business,
phase 2 -- estimated at 12-20% of the CPU while actually speaking, and
nothing while silent, since `tts` blocks on its mailbox
(`z_proc_wait(0)`).

---

## Audio

`tts_audio.c`. The service renders into an 8KB ring in its own memory
and plays it through mixer channel 7 with `MIXPOS` readback, exactly as
`play` streams ([audio.md](audio.md), [play_app.md](play_app.md)). The
mixer's `STEP` resamples 11025Hz to the output rate in gateware, so the
CPU never touches a 44.1kHz sample. 16-bit samples where the mixer
supports them, 8-bit otherwise.

- About 190ms of speech is rendered before the channel starts; after
  that the service wakes every 40ms and renders ahead of the read
  position.
- A looping ring would replay its last lap forever, so once the speech
  is all rendered, silence is written ahead of the reader until it
  passes the end, and then the channel is switched off.
- If the read position stops moving for half a second -- a player
  cleared every channel on startup -- the utterance is abandoned
  rather than waited on.
- The channel is stopped before the service exits: the mixer reads
  the service's memory.
- On a bitstream without the mixer the service keeps its transcript
  and timing and says so at startup.

Every utterance logs what it cost:

    tts: [1450 ms of speech, 210 ms to render: 14% of one CPU]

**That line is the number to check on hardware.** Under 100% the voice
keeps up on an otherwise idle machine; the phase 2 estimate is 12-20%.

**Channels are not reserved.** An audio app that starts while speech is
off gets all eight, exactly as before. While `tts0` is registered,
`track`, `play` and `midi` leave channel 7 (`Z_AUDIO_CH_SPEECH`,
`zaudio.h`) alone, deciding once when they claim the mixer:

- `play` uses channels 0-1 and only skips 7 when clearing.
- `midi` runs seven voices instead of eight (`synth_t.nvoices`), and its
  one channel-write path refuses channel 7. Its tests still pass.
- `track` skips 7 when clearing; only an 8-channel module would use
  it, and then the two collide.

An app that starts while speech is OFF, and speech started afterwards,
still collide: the next utterance takes channel 7 back. Other mixer
users (`gamedemo`) do not follow the convention yet. The reason
this is not "let them collide": someone who cannot see the screen and
has music playing still has to hear the file list to find the stop
control. If starting speech takes a channel an app already holds, the
next utterance simply takes it; that glitch is documented, not
engineered away.

`tts` only ever sets `MIXEN`/`EN` and never clears other channels. On
a build without `AUDIO_MIXER` it falls back to the FIFO, which is
exclusive and will fight software-mixed apps such as `audiotest`.

---

## The voice

A rule-based formant synthesiser: no recorded voice data at all.

- **Synthesis** (`synth.c`, built): a cascade/parallel resonator
  network, implemented from the published description in Klatt's
  1980 paper (JASA). 11025Hz. Voicing is the derivative of a polynomial
  glottal flow pulse (t^2 - t^3 over a 50% open phase), which already
  includes the radiation characteristic. It feeds five cascaded
  resonators, F1-F3 moving and F4/F5 fixed. Noise feeds the same
  cascade for aspiration, and five parallel resonators plus a bypass
  for frication, each with its own amplitude. There is no nasal
  pole/zero pair: its antiresonator's coefficients do not fit 32-bit
  fixed point comfortably, and nasals are made instead with a low,
  damped F1 and their transitions. Integer only; `cos()` and `exp()`
  come from tables generated by `gen_tables.py`, so no libm or soft
  float is linked. About 15 multiplies per voiced sample, 35 during
  frication.
- **Prosody** (`phon.c`): the utterance is divided into PHRASES at
  every pause. Each phrase starts near the top of the pitch range and
  declines across itself, reset at the next pause -- the breath a
  reader takes -- with successive phrases starting a little lower so a
  long sentence still descends overall. A phrase ending in a comma
  rises ("there is more"); one ending in a full stop falls away. The
  syllable before a pause is lengthened, more before a full stop than
  before a comma. Pauses are graded: 190ms at a comma, 300ms at a
  sentence, 420ms at a paragraph. Function words ("the", "of", "to" --
  76 of them, `text2ph.c`) are de-stressed, which both flattens and
  shortens them; left stressed they make a sentence sound like a list
  of equally important words, which is most of what makes rule-based
  speech tiring over a page.

  A pause is a 16-bit field. It was a byte until the pauses were
  measured, which meant a sentence's 280ms had been wrapping to 24ms
  since the synthesiser was first written -- most of why sentences
  ran into each other.

  These are rules, and modest ones: measured over the book samples
  they shorten an utterance by 3-7% and move the pitch at a phrase
  boundary by a few Hz. Phase 7 replaces the numbers with models
  trained on real speech; the shape above is what those models fill
  in.

- **Phonemes** (`phon.c`, built): our own table of 39, with formant
  targets or loci, durations, amplitudes and fricative/burst spectra,
  set from standard acoustic-phonetics ranges. A stop becomes a
  closure, a place-shaped burst and, for p/t/k, aspiration at the
  start of the next vowel during its formant transition -- voice
  onset time, most of the difference between "pat" and "bat". A
  diphthong glides between two targets. Formants meet at each
  boundary at the consonant's locus moved part of the way toward the
  vowel. Pitch declines over the utterance, rises on stressed vowels,
  falls at a sentence's end and rises at a question's.
- **Text** (`text2ph.c`, `lts.c`, `pack.c`): see [Text](#text) below. A
  card carrying a speech pack adds
  391,159 pronunciations between the built-in dictionary and the
  rules, plus letter-to-sound rules trained on that lexicon: 46% of
  unknown words exactly right against the hand-written rules' 20% on
  the same test.
- **Letters to sounds:** rules written fresh in the style of the NRL
  rules (Elovitz et al., NRL Report 7948, 1976 -- a US Government work,
  in the public domain), plus a small exception dictionary for the UI
  vocabulary rules always get wrong. Phase 5 replaces these with rules
  LEARNED from the public-domain Moby Pronunciator, and adds a real
  lexicon.
- **Front end:** numbers, punctuation, symbols, single characters and
  spelling, key names, simple stress, falling and question intonation;
  rate up to about 2x, because experienced users listen fast.

### Text

`text2ph.c` turns text into phonemes, a sentence (at most a dozen
words) at a time, so an utterance of any length never needs more than
one chunk of phonemes in memory.

- **Words** are looked up in an exception dictionary of 213 entries
  -- function words and irregular spellings the rules get wrong ("the",
  "of", "one", "said", "people"), and interface words that must be
  exact ("calculator", "clipboard", "Zeitlos", key names). Everything
  else goes to the letter-to-sound rules.
- **Acronyms** (2-5 capitals: USB, CPU), **vowelless words** (mkdir,
  pwd) and **lone letters** are spelled, as people say them.
- **camelCase** and **HTTPServer**-style humps are split into words.
- **Numbers** are read as numbers up to the trillions ("four thousand
  ninety six"), with thousands separators, decimals ("three point one
  four"), a leading minus and a trailing percent. More than 12 digits,
  or a leading zero ("007", a serial number), is read digit by digit.
- **Symbols** that mean something in running text are named -- @ & + =
  % $ # * / \ | ~ ^ < > -- and brackets and quotes are silent. A dot
  inside a word is "dot" (readme.txt). A run of three or more of the
  same symbol ("-----") is a separator and becomes a pause.
- **Homographs** -- words spelled the same and said differently -- are
  settled by the word BEFORE, which is where English puts most of the
  evidence. A determiner ("the", "a", "his") means a noun is coming, so
  "the RECord"; "to" or an auxiliary means a verb, so "to reCORD". A
  perfect auxiliary makes "read" past ("I have read"), anything else
  present ("I read"). 18 stress pairs plus read and lead; with no
  evidence the noun reading wins, since these are nouns more often in
  prose and a noun said as a verb sounds like a mistake while the
  reverse often passes.
- **A lone character** -- a key echo, the cursor landing on it -- is
  always its name, punctuation included ("semicolon", "right paren").
  `Z_TTS_F_SPELL` does the same for a whole utterance.
- Text in `[brackets]` is phonemes, passed straight through.

### Letters to sounds

`lts.c`: context rules in the classic format of the public-domain NRL
report (Elovitz et al., 1976) -- `left [match] right = phonemes`, first
match wins, specific rules before each letter's default. **The rules
are this project's own**, about 250 of them written from ordinary
English phonics: magic E, vowel teams, -tion/-sion/-ture, -ed and -s
voicing, soft C and G, silent K/W/GH/B, and so on. After the rules, a
stress pass puts primary stress on the first syllable (the second after
an open-syllable prefix like re-/de-/be-, and before -tion/-ic), and
reduces unstressed short vowels to schwa.

    cd sw/apps/tts && make lts_eval

scores the rules against CMUdict on 2000 random ordinary words
(dictionary words that also appear in the recogniser's language model,
since CMUdict alone is mostly surnames):

| | words exactly right | phoneme error rate |
|---|---|---|
| rules alone | 41% | 17.8% |

"Exactly right" is strict -- stress is ignored, but a schwa where the
dictionary has a full vowel counts as a miss -- and most remaining
errors are in unstressed vowels, which cost little intelligibility.
Words that matter and come out wrong go in the exception dictionary
rather than into ever more specific rules.

### Tuning, and the gate

    cd sw/apps/tts && make wav        # tests/testset.txt -> /tmp/tts_wav/
    make wav PACK=/path/to/speech.zspk
    make wav WPM=250 OUT=/tmp/fast
    python3 tests/asr_eval.py         # recogniser check

`PACK` renders with a speech pack's lexicon and trained rules, which
is what the device does; without it the built-in dictionary and rules
are used. The same text both ways is how to hear what the pack is
worth -- "chemistry" stops starting with a "ch" sound, "colonel"
becomes "kernel". The voice itself is identical either way: a pack
changes which phonemes are spoken, never how they sound.

`make wav` renders the test set -- 45 single words, a few sentences in
phonemes, and some plain text through `text2ph.c` -- with the real
`phon.c` and `synth.c`, and **fails if any resonator's accumulator came
within 2x of 32-bit overflow or any sample clipped**. That check has
already earned its place: two silences in a row once left a resonator
at 0Hz with zero bandwidth -- poles on the unit circle -- and the
aspiration of a final /k/ fading into it ran the output away. Current
figures: 14x headroom, peak output 42% of full scale.

`asr_eval.py` is an automatic stand-in for a listening test: an
off-the-shelf recogniser (pocketsphinx, whose English model ships in
its pip wheel) identifies each single-word file from the 45-word
vocabulary, like a listener choosing from a word list. It evaluates the
synthesiser and contributes nothing to it.

| step | score |
|---|---|
| first render | 60% |
| voicing, aspiration and frication levels separated (vowels had been 20dB under /s/) | 71-73% |
| chance | 2% |

A recogniser trained on natural speech is a much harsher judge of
formant speech than a person, and its remaining misses are mostly
voiceless stops and nasals. So the number is for comparing versions,
not a verdict; the verdict is people listening to the files. **That
is the gate:** if the WAVs are not clearly understandable, the
approach changes here, before phase 3 builds on it.

### Clean-room provenance

The references are published papers and general phonetics knowledge.
No existing speech synthesiser's source -- eSpeak, rsynth, Festival,
Flite, SAM, `klatt.c` or any other -- is consulted, and no voice data
is used. Anything added later that departs from this must be recorded
here, with its source and licence.

The outside programs and data involved are **evaluators only**:
pocketsphinx (BSD-licensed) scores the audio in `tests/asr_eval.py`,
and the CMUdict that ships with it is the answer key `tests/lts_eval.py`
scores the letter-to-sound rules against. No entry from CMUdict was
copied into the rules or the exception dictionary, nothing from either
is in the service, and the service does not depend on them. Rules were
corrected when the scores showed a *class* of error (unstressed vowels
not reduced; a prefix test that fired on "belts"), not word by word.

---

## Phases

| phase | | status |
|---|---|---|
| 1 | protocol, client library, service with transcript backend, Super keys, `ttstest` | **done** |
| 1.5 | `zsayall`, Super+A in `text`, `Z_TTS_F_CONTINUES` | **done** |
| 2 | formant synthesiser, phoneme input, host WAV harness and recogniser check, mixer streaming | **built; listening test and first on-board run pending** |
| 2b | speech-channel convention in `track`/`play`/`midi` | **done** |
| 3 | text front end: words, numbers, symbols, acronyms; letter-to-sound rules and stress; exception dictionary; `system.tts.*` settings | **done** |
| 4 | the ZSPK speech pack, its reader here, and the release plumbing (the pipeline that builds packs is separate) | **done** |
| 5 | the rest of pronunciation: word frequencies for a resident core | **lexicon, trained rules and homographs done** |
| 6 | automatic announcements and boot (was phase 4) | **done** except a Settings panel (the `system.tts.*` keys work from the config file today) |
| 7 | prosody: phrases, phrase-final lengthening, graded pauses and function-word de-stressing by rule; the rules' numbers fitted to LJSpeech  | **done; fitted values pending a run on the corpus** |
| 8 | acoustics: vowel formants measured from the corpus; then transitions | **vowels done** |
| 9 | reading: Super+A in `term` and `read`, typed-key echo, optional earcons (was phase 5) | **editor echo and Super+A in `text` done** |

Superseded numbering, kept so older notes still read:
| 4 | automatic announcements: focus, Alt+Tab, window open, dock selection, widget/list/dialog focus; Settings section; start at boot -- now phase 6 | |
| 5 | reading: Super+A in `term` and `read`, typed-key echo, optional earcons -- now phase 9 | |

---

## Testing

    cd sw/apps/tts && make test

- **`tests/tts_test.c`** -- the real `tts_queue.c`: order and marks,
  interrupt (the backend told to stop, earlier marks cancelled in
  order), low priority, stop, repeat (spelled again if it was spelled,
  never re-reported), sanitising, empty and NULL text still answered,
  truncation, the item limit, a 4000-round randomised wrap of the text
  ring checking every utterance comes back as its own text, and a full
  ring refusing rather than overwriting what is playing.
- **`tests/zspeak_test.c`** -- the real `zspeak.c` against a stubbed
  kernel. Counts rather than times, because a count is exact: one
  lookup for a hundred calls with speech off, the text pointer never
  dereferenced when nobody is listening, the service found within one
  cache window of starting and forgotten on the first failed send
  after exiting, the ring surviving the caller's buffer being
  overwritten.

- **`tests/zsayall_test.c`** -- the real `zsayall.c` against a
  scripted service: reads to the end by itself without stopping
  anyone's speech, keeps two lines queued, follows the voice, stops on
  a cancel and keeps its place, ignores a previous read's replies after
  a restart, survives a lost reply, gives up on a timeout, never uses
  mark 0.

- **`tests/text2ph_test.c`** -- the real `text2ph.c` and `lts.c`: exact
  phonemes for numbers and symbol names; the decisions elsewhere
  (acronym spelled, capitalised word not, camelCase split, brackets
  silent in text but named alone, a separator line a pause, a sentence
  ending a chunk); a handful of rules UI words depend on.

The first three suites were checked against deliberate bugs (an overlapping
arena allocation; the missing cache drop on a failed send; ignoring a
cancel mid-read) to make
sure they fail when they should.

The voice: `make wav` and `tests/asr_eval.py`, above. The rules:
`make lts_eval`, above.

On the board, `ttstest` (above), and the render-cost line every
utterance logs.
