# automate -- scripted demos

`sw/apps/automate`. Plays a script: speech, captions, keys, the
pointer, and apps, in order. The [demos](demo.md) are scripts for it.

```
> run automate /demo/short.zds      from the serial console
> run automate                      plays /demo/demo.zds
```

The kernel shell's `run` passes everything after the program name as
its launch argument (through wm, `Z_WM_SET_ARG`) -- for any app, so
`run view /demo/squirrel.pgm` works too. A `.zds` file also opens in
`automate` from `files` (`sw/common/ztype.c`), though for a recording
the console is better: nothing else is on the screen. Running it again stops the copy that is
running and starts over — that is how you restart a demo.

Everything it does goes through the paths a person uses, so a demo
shows the real system rather than a recording of it:

| What | Path |
|---|---|
| keys | `hid_inject()`, the on-screen keyboard's path: the kernel stamps the layout, `wm` translates and delivers — dead keys, AltGr, Japanese input and wm's own shortcuts all work |
| pointer | the virtual mouse register (`reg_vmouse`, `Z_FEATURE2_VMOUSE`): the hardware cursor sprite moves, wm hit-tests and delivers clicks like a USB mouse's |
| apps | `z_proc_run()`, with a file through wm's launch argument, as `files` does |
| speech | the `tts` service, holding the narrator lease ([tts.md](tts.md#narration)) |
| captions | [captions.md](captions.md) |
| windows | `Z_WM_WIN_QUERY` / `_PLACE` / `_FOCUS` / `_TBICON` ([window_manager.md](window_manager.md#automation)) |

## The language

One command per line. `#` starts a comment. Words are separated by
spaces; `"..."` groups. Commands that take text take the rest of the
line (surrounding quotes are stripped, `\n` is a line break).

### Speech and captions

| | |
|---|---|
| `say TEXT` | speak, and wait until it has been spoken |
| `say& TEXT` | speak, and carry on — see "Speech and the CPU" below before using it |
| `sync` | wait until everything said has been spoken |
| `narrate TEXT` | caption TEXT and say it (waits) |
| `narrate CAPTION \| SPOKEN` | caption one thing, say another — a short caption, a full sentence |
| `narrate& ...` | the same, without waiting |
| `caption TEXT` / `caption-off` | |
| `caption-size 1..3` | 2 by default (see [captions.md](captions.md#how-big)) |
| `caption-pos bottom\|top\|center` | |
| `caption-style [compact] [inverse]` | |
| `rate WPM` | speaking rate |
| `voice synth\|female\|recorded` | switch voice, silently, once the current sentence has finished |
| `speech on\|off` | starts `tts` if needed (and silences its "Speech on") |

**Speech and the CPU.** The recorded voice renders at 55-60% of the
48MHz CPU while it speaks, and on hardware *anything* running beside it
— an app starting, an image decoding, the cube spinning, even typing
into `text` — made it fall behind, heard as gaps. So the demo scripts
never overlap speech with action: `narrate` and `say` wait, and while
something happens the caption carries the message. `narrate&` and
`say&` are still in the language for scenes that really are idle.

**Voices.** The recorded voice is in the speech pack
(`/speech/en.spk`, [tts_data.md](tts_data.md)); without one, `tts`
falls back to the synthesised voice and `automate` warns at
`voice recorded`. `voice` switches with `Z_TTS_VOICE_QUIET` (`ztts.h`), so
`tts` does not announce the change: the next sentence is the first
thing heard in the new voice. Both demos open in the synthesised voice
and hand over to the recorded one. The synthesised voice mangles
"computer" and "FPGA"; give it easy words.

**Waiting for speech** is by marks: every utterance carries one, and
`tts` answers `Z_TTS_MARK_DONE` when it has been spoken. Every wait has
a timeout (twice the estimated length plus five seconds), because a
lost reply must not hang a demo in a shop window. Without speech — no
`tts` on the card, no audio — the timing is estimated from the rate,
so captions still stay up as long as the sentence would have taken.

### Apps and windows

| | |
|---|---|
| `run APP [FILE] [at X Y]` | launch, and wait for its window (10 s at most); `at` opens the window there |
| `run& APP [FILE] [at X Y]` | launch, don't wait |
| `await APP [MS]` | wait for a window from an app started with `run&` |
| `close APP` | the close icon's action, then Ctrl+Q, then Escape, then kill |
| `kill APP` / `killall` | through wm, which takes the windows away too |
| `focus APP` | raise and focus, as a click would |
| `place APP X Y` | move an open window's top-left corner there (prefer `run ... at`) |
| `origin APP` / `origin screen` | pointer coordinates are relative to APP's content area from now on |
| `tbicon APP close\|new\|open\|save\|font` | move the pointer to a titlebar icon and click it |

**Windows open where they belong.** `run ... at X Y` sends
`Z_WM_WIN_NEXT_PLACE` (tag 0: the next new process's first window)
just BEFORE `z_proc_run()`, so wm puts the window there when the app
creates it. Sent after, it was a race: `gpu3d` creates its window fast
enough to win it, appeared in the cascade, and jumped. Moving a window afterwards (`place`)
works too, but an app moved while it is still painting its first frame
can lose that frame; wm now asks for a full repaint after every
`place`, but `at` avoids the problem entirely.

**Closing.** `close` first does exactly what clicking the close icon
does (`Z_WM_WIN_TBICON` with kind 0): every windowed app answers that,
including the ones with no key for quitting. Then Ctrl+Q, then Escape,
each given up to 0.7 s, then a kill. The polite ways come first because
an app killed from outside never stops the mixer channels it left
running (`midi`).

**A window that kills its owner** (`Z_WIN_FLAG_CLOSE_KILLS_OWNER`,
e.g. `track`) is killed by `close`'s first step, as a click on its close
icon would -- so it never runs its own shutdown. For `track` that means
its mixer channels keep playing. Quit such an app with its own key
instead (`focus track`, `key q`) and `kill` it after, as `long.zds` does.

**Killing goes through wm** (`Z_WM_WIN_KILL`), which kills the process
and destroys its windows. Killing a process directly leaves its windows
on the screen, and the kernel reuses the pid for the very next process
— which then appears to own them: every later query, placement and
close aimed at the new app lands on the dead one's window. That was the
first hardware run: `info` killed, its window left behind, and `clock`,
started next as the same pid, logging `bad clip region message` for
regions meant for `info`'s window.

### Keyboard

| | |
|---|---|
| `key COMBO...` | e.g. `key ctrl+s`, `key alt+tab enter`, `key alt+equal` |
| `hold COMBO MS` | |
| `type TEXT` | typed on the active layout, at a slightly irregular human pace |
| `type-speed MS` | average time per character (55) |
| `layout NAME` | `us`, `de`, `fr`, `ja-us` ... ([keyboard_layouts.md](keyboard_layouts.md)) |
| `melody MS KEY[:N]...` | play keys as notes, each N beats of MS; `-` is a rest |

Modifiers: `ctrl`, `shift`, `alt`, `altgr`, `super`. Named keys:
`enter esc tab space backspace delete insert home end pgup pgdn up down
left right f1`–`f12 minus equal caps`, or `0x2c` for a raw HID usage.
Anything else is a character.

**Typing follows the layout.** A reverse map from character to key is
built from the same `z_kbd_translate()` tables wm uses, for the layout
the kernel is stamping now, so it cannot disagree with wm. A character
with no key of its own is typed as a dead key and a base letter (é on a
German keyboard is ´ then e). On a Japanese input layout (`ja-us`) the
romaji go through wm's input method and come out as kana.

### Pointer

| | |
|---|---|
| `mouse X Y [MS]` | glide there (smoothstep, ~60 steps a second) |
| `click [X Y]` / `dclick [X Y]` | |
| `press` / `release` | the left button |
| `drag X1 Y1 X2 Y2 [MS]` | |
| `stroke MS X Y X Y ...` | press at the first point, glide through the rest, release |
| `pointer hide` | hand the cursor back to the USB mouse |

Needs a bitstream with the virtual mouse (`Z_FEATURE2_VMOUSE`,
universal from the release that added it). Without it the pointer
commands only track a position and `tbicon` falls back to
`Z_WM_WIN_TBICON`: the script still runs, keyboard-only.

### Flow and the machine

| | |
|---|---|
| `pause MS` | |
| `print TEXT` | a line on the console (`automate: -- TEXT`) -- the demos log their revision this way |
| `video white\|amber\|green\|paper` | the virtual phosphor |
| `reset` | close everything this script started, hide the caption, white phosphor, US layout |
| `label NAME` / `goto NAME` / `loop` | |
| `chain FILE` | carry on in another script, from its top |
| `end` | done — or, in attract mode, start over |
| `stop` | done, in any mode |
| `attract SECONDS` | attract mode (below) |
| `interruptible off` | ignore people (for recording); has no effect in attract mode |

## Attract mode

A demo in a shop has to give way the moment somebody touches the
machine, and come back when they leave.

The kernel marks every event that comes through `hid_inject()` —
bit 25, `Z_KBD_EV_INJECTED` (`zkbd.h`), set in `k_hid_inject()`, never
trusted from the injector — and wm counts key events **without** it,
and USB pointer motion, as real input. The count is in every
`Z_WM_WIN_INFO` reply. `automate` asks every 150ms while it waits.

wm logs what it counted (`wm: real input: key 0x..` or `pointer`, at
most every five seconds), and `automate` how many, so a demo that stops
by itself names its cause. Pointer changes count only on a port that
reports a mouse.

When the count moves, the script stops: speech is stopped and the
narrator lease released, the caption hidden, the cursor handed back to
the USB mouse. Whatever is on the screen stays — it is the customer's
now. Without `attract` the program then exits; with `attract N` it
waits until nobody has touched anything for N seconds, and starts the
script again from the top (which should begin with `reset`).

The remote desktop's keys come through `hid_inject()` too, so a remote
viewer does not stop a demo; a physical keyboard or mouse does. Both demo
scripts start with `interruptible off`, for recording; attract mode
ignores it.

## Size

About 150KB (100KB of it newlib's `printf`, which the command log
uses) plus a 32KB script buffer. One copy runs at a time; it registers
as `automate0`.

## Not done

- Conditionals (`if network`); the demos avoid the network.
- `include` for shared fragments (`chain` covers the in-store case).
- Waiting for a window to finish drawing: `run` waits for the window
  to exist, then 120ms. An app with a slow first paint needs a
  `pause` after it.
