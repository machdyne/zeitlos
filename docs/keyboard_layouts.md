# Keyboard layouts

Typing in languages other than US English: keyboard layouts, the
characters they produce, and how those characters travel through the
system. The companion page on how text is stored and drawn is
[text_encoding.md](text_encoding.md).

## Status

The work is phased; each phase leaves the system working.

| Phase | What | Status |
|---|---|---|
| 1 | Keysym ABI: keysyms become Unicode, named keys move above it | done |
| 2 | ISO 8859-15 glyphs in the hardware fonts; drawing by codepoint | done -- see [text_encoding.md](text_encoding.md) |
| 3 | Layout engine, nine layouts, dead keys, Super+Space switching | done |
| 4 | UTF-8 in `text` | done -- see [text_editor.md](text_editor.md), "Encodings" |
| 5 | Second-wave layouts: `ch` `ch-fr` `se` `fi` `dk` `no` `pt` `be` `us-intl` `jp` | done |
| 6 | UTF-8 in `term`, `read` and `vi` | done -- see [terminal.md](terminal.md), "UTF-8" |
| 7 | Japanese: the font, the `jfont` service, wide glyphs in `zgfx`, `text` | done -- see [text_encoding.md](text_encoding.md), "Japanese" |
| 7b | Japanese in `term` (codepoints kept in cells and scrollback, drawn at 6x12); `read` spaced correctly | done -- see [terminal.md](terminal.md), "UTF-8" |
| 8 | Japanese input: romaji to hiragana and katakana (`ja`, `ja-kata`, `ja-us`) | done -- kanji conversion awaits a dictionary decision, see "Japanese input" |
| 9 | `keyboard`: an on-screen keyboard for any layout, pointer only | done -- see [keyboard_app.md](keyboard_app.md) |

Dead keys were planned for phase 5 and moved into phase 3: the German
layout most people use (xkb's default `de`, Ubuntu's included) has
them, and a `de` that behaved differently from the person's other
computer would be worse than none. The layouts that need them came with
them.

## Using it

Put the layouts to use in `/zeitlos.cfg`, the one to start in first:

```
system.keyboard.layouts: us,de
```

or use the **Keyboard layouts** row in `settings`. **Super+Space**
steps to the next one; its two-letter label shows over the right end of
the dock for a moment, and with speech on its name is spoken. Nothing
else changes: every app receives the characters the layout types.

| name | layout | notes |
|---|---|---|
| `us` | English (US) | the default; right Alt is Alt |
| `gb` | English (UK) | ISO keys: `#~` beside Enter, `\|` beside left Shift |
| `de` | German | QWERTZ; dead `^` `´` `` ` ``, as on every desktop |
| `de-nodeadkeys` | German (no dead keys) | `^` `´` `` ` `` type themselves |
| `it` | Italian | |
| `fr` | French | AZERTY; digits need Shift |
| `es` | Spanish | |
| `latam` | Spanish (Latin American) | |
| `br` | Portuguese (Brazil) | ABNT2, including the key beside right Shift |
| `ch` | German (Switzerland) | QWERTZ; u-umlaut plain, e-grave on Shift |
| `ch-fr` | French (Switzerland) | the same keyboard, e-grave plain |
| `se` | Swedish | |
| `fi` | Finnish | xkb's default Finnish (kotoistus) |
| `dk` | Danish | |
| `no` | Norwegian | |
| `pt` | Portuguese | |
| `be` | Belgian | AZERTY |
| `us-intl` | English (US, international, with dead keys) | `'` `"` `` ` `` `~` `^` are dead keys: `'` then `e` is e-acute, `'` then Space an apostrophe |
| `ja` | Japanese input, hiragana, on JIS keys | romaji becomes kana -- see "Japanese input" |
| `ja-kata` | Japanese input, katakana, on JIS keys | |
| `ja-us` | Japanese input, hiragana, on US keys | for a US keyboard |
| `jp` | Japanese (JIS 106/109) | romaji only -- kana input is phase 8. The yen key types backslash, as on every Linux desktop: Japanese fonts traditionally draw 0x5C as the yen sign |

Two xkb dead keys have no combining rule here and type nothing:
`dead_currency` and `dead_greek`, which only appear on AltGr levels of
a few layouts. The generator lists them when it runs.

A character outside ISO 8859-15 -- AltGr on these layouts reaches
arrows, fractions, Polish letters -- is still typed and delivered; it
draws as the missing-glyph box ([text_encoding.md](text_encoding.md)).
`text` takes every character a layout types (phase 4), and `term`
sends it to the shell or ssh session as UTF-8 (phase 6).

## Japanese input

Three layouts are **input methods**: `ja` and `ja-kata` on a JIS
keyboard's keys, `ja-us` on a US keyboard's. Put one in the cycle --
`system.keyboard.layouts: us,ja` -- and Super+Space switches between
English and Japanese, as on a Japanese desktop.

Type romaji and kana come out:

| typed | gives |
|---|---|
| `konnichiha` | こんにちは |
| `konna` (or `konnna`) | こんな -- `nn` before a vowel is ん plus the next syllable |
| `kanji`, `hon` | かんじ, ほん -- `n` before a consonant, or at the end, is ん |
| `hon'ya` | ほんや -- `n'` is always ん |
| `gakkou`, `chotto` | がっこう, ちょっと -- a doubled consonant is っ |
| `shi`/`si`, `tsu`/`tu`, `ja`/`zya` | Hepburn and kunrei-shiki both |
| `kyo`, `sha`, `cha`, `nya` ... | every ゃゅょ combination |
| `xtu`/`ltu`, `xa`, `lya` | small kana |
| `fa`, `vu`, `wi`, `thi` | the extended kana for loanwords |
| `-` `,` `.` `[` `]` `~` `/` | ー 、 。 「 」 〜 ・ |

`ja-kata` gives the same in katakana (`ko-hi-` is コーヒー). Case does
not matter. A letter that can still become a kana waits -- `k` shows
nothing until `ka` -- and the letters waiting are shown over the right
end of the dock (with the layout's label when none are waiting, so the
box stays put while you type -- [window_manager.md](window_manager.md)), since the app sees nothing until a kana is finished.
Backspace takes back a waiting letter and Escape drops them all; any
other key (Space, Enter, an arrow, a digit) finishes them first -- a
lone `n` becomes ん -- and then does what it always does. Shortcuts
with Ctrl, Alt or Super are never romaji.

The converter is `z_kbd_ime_feed()` in `zkbd.c` -- pure, no I/O, and
tested on the host with the spellings above (`test_zkbd.c`). Its table
of 190 spellings is generated, sorted, and searched by bisection. `wm`
forwards each finished kana as an ordinary key, a press and a release,
so it reaches `text`, `term` (and through it ssh) and anything else
exactly as a typed character would. The input method layouts point at
their base layout's key table rather than copying it.

**No kanji yet.** Kana-to-kanji conversion needs a dictionary, and the
usual small one (SKK-JISYO) is GPL while the larger alternatives carry
mixed licences. That choice is still to be made; until then Japanese
here is written in kana.

## Phase 5: the second wave

Ten more layouts, all from the same generator: `ch`, `ch-fr`, `se`,
`fi`, `dk`, `no`, `pt`, `be`, `us-intl` and `jp` -- nineteen in all, of
the 32 the event's five layout bits allow. The only new key is JIS's
yen key (`<AE13>`, usage 0x89). The generator now also follows includes
for a layout's display name (Swedish takes its name from `se(se)`) and
reports any dead key it has no rule for. Every character the new
layouts type on their first two levels is in ISO 8859-15, so all of it
draws in hardware.

## Phase 3: layouts

### Where the tables come from

`sw/common/zkbd_layouts.c` is generated from **xkeyboard-config** -- the
database every Linux desktop uses -- by `tools/gen_kbd_layouts.py`:

```
python3 tools/gen_kbd_layouts.py /usr/share/X11/xkb/symbols \
        /usr/include/X11/keysymdef.h
```

(Debian/Ubuntu packages `xkb-data` and `x11proto-dev`.) It is not run
by the build; run it after changing the layout list and commit the
result. Matching xkb is the whole specification: a layout transcribed
by hand is wrong in a dozen small places nobody notices until they need
that key. The generator:

- loads `pc` first and the layout on top, as a desktop does, following
  xkb's `include` chains (`latin(type4)`, `kpdl(comma)`, ...);
- takes the four shift levels of every key in the main block, the ISO
  key beside left Shift (usage 0x64), the Brazilian key beside right
  Shift (0x87), the JIS yen key (0x89) and the keypad decimal, mapping
  xkb key names to USB HID
  usages -- `<BKSL>` to both 0x31 and 0x32, as Linux does;
- turns keysym names into Unicode through `keysymdef.h`, including its
  deprecated aliases (`guillemotleft`), which have no `U+` comment and
  are resolved by value;
- notes whether the layout switches level 3 with right Alt (AltGr);
- computes what each dead key combines into from Unicode normalization
  -- dead acute + e is the precomposed e-acute -- rather than from X11's
  Compose file, which is tens of thousands of lines mostly for other
  scripts. 338 combinations for the dead keys these layouts have.

**Licence.** The layout data is xkeyboard-config's, which is under
X11/MIT-style licences: permissive, with the condition that their
copyright and permission notices go with copies. They are kept verbatim
in `sw/common/zkbd_layouts.LICENSE`, the generated file's header points
to it, and the README lists it with the repo's other exceptions. When
re-syncing to a newer xkeyboard-config, refresh that file from the
`xkb-data` package's `copyright` file. `keysymdef.h` is only read to
turn keysym names into codepoints -- nothing of it reaches the output
-- and the dead-key combinations come from Unicode normalization.

Layout ids are positions in `LAYOUTS` in the generator. They are what
the kernel stores, so the list is append-only.

### Translation (`sw/common/zkbd.c`)

`z_kbd_translate(layout, usage, modifiers, locks, &mods_out)`:

- **Levels:** plain, Shift, AltGr, Shift+AltGr. On a layout with AltGr,
  right Alt is AltGr. A key with nothing on its AltGr levels types its
  plain character, as a two-level key does under xkb.
- **Caps Lock** acts on a key whose two levels are a lower/upper case
  pair -- a-z, but also a-umlaut, e-acute, n-tilde -- and never on
  digits or punctuation. Before layouts, `wm` applied it to usages
  0x04-0x1D; that is wrong the moment a-umlaut is a letter.
- **Ctrl** with a key whose plain character is a letter gives its
  control code, following the layout: Ctrl+Z on German is the key marked
  Z, Ctrl+A on French the key marked A.
- **AltGr is not Alt.** When AltGr picked a character, `mods_out` comes
  back without the right-Alt bit, so an app does not mistake AltGr+Q
  (`@` on German) for an Alt shortcut.
- **The keypad** types digits and operators whatever the Num Lock
  state (Num Lock starts off: `zkbd.h`, "lock keys"). Its decimal key
  follows the layout -- a comma on German and Brazilian. `zkbd.h` had
  said the keypad worked this way since before layouts; nothing had
  implemented it, and the keypad typed nothing at all.

`z_kbd_event_to_keysym(ev, &mods_out)` does the same for a raw event,
taking the layout, modifiers and locks from the event itself.
`z_kbd_usage_to_keysym()` is unchanged: US, no locks. Checked by
translating every usage and modifier combination with the old and new
code: identical, except for keys that used to produce nothing (the
keypad, and the ISO keys 0x32 and 0x64).

The tables are `#include`d into `zkbd.c` rather than compiled on their
own, so every Makefile that already built `zkbd.o` got them unchanged.
`wm` is 12KB larger for them and for reading the config; the four core
apps come to about 481KB of the 576KB flash region.

### The active layout lives in the kernel

The kernel keeps the active layout and stamps it into bits 24:20 of
every key event (`Z_KBD_EV_LAYOUT`), the same way it stamps the lock
state -- keys from both USB ports and keys injected over the ESP32 link
alike. It still translates nothing; it only holds the number.

- A switch can never land between a key and its translation: the
  layout travels with the key.
- A program that reads raw events without `wm` -- `midi`, `play` and
  `track` in console mode -- follows the same layout as everything
  else. They use `z_kbd_event_to_keysym()` now.

One syscall, `Z_SYS_KBD_LAYOUT`, appended at the end of `syscalls.def`,
behind `z_kbd_active_layout()` and `z_kbd_set_active_layout()`
(declared in `zkbd.h`, in `zeitlos.c`). The request goes out typed
`Z_UINT32` and the kernel answers `Z_INT32`, so an old kernel -- which
bounds-checks the syscall number and answers nothing -- reads as
"layout 0, cannot change", and `wm` says so on the console.

### What `wm` does with a key

In `dispatch_keys()`, in order:

1. **Config:** if the config generation changed, re-read
   `system.keyboard.layouts` ([config.md](config.md)). Checked when a
   key arrives, not every pass of the main loop.
2. **Super+Space** (by usage, since Space is the same key everywhere):
   next layout, then the dock label and the spoken name.
3. **Translate** with `z_kbd_event_to_keysym()`.
4. **Releases** are delivered as whatever the press was delivered as.
   Otherwise a composed e-acute would be released as a plain e, and a
   key pressed in one layout and released in another would release a
   different character -- a stuck key to an app that tracks what is
   held.
5. **Dead keys** (next section).
6. wm's own shortcuts, then the dock, then the focused app. Alt for
   these means left Alt, or right Alt on a layout without AltGr --
   AltGr+Tab is not Alt+Tab. **Alt+[ and Alt+]** (dock pages) are
   matched by key, the two keys right of P: on German `[` and `]` need
   AltGr, and a character match would put the shortcut where nobody
   could press it.

### Dead keys

A dead key types nothing itself; `wm` holds it and looks at the next
key:

| then | types |
|---|---|
| a letter it combines with (`´` `e`) | the combined letter (`é`) |
| Space | the accent on its own (`´`) |
| the same dead key | the accent on its own |
| another dead key | the first accent, and waits on the second |
| a character it does not combine with (`´` `q`) | the accent, then the character |
| Escape | nothing: the accent is cancelled and Escape is spent doing it |
| anything else -- an arrow, Enter, a Ctrl or Alt shortcut | the key as usual; the accent is dropped |

This is what xkb and Windows do. Apps never receive a dead key: they
get the finished character, and a composed one counts as one key press
and one release.

### Compatibility

The syscall list changed. **Rebuild and reflash the kernel and every
app together** -- the rule `syscalls.def` states for any change to it.
An app built before this phase still works (it never asks for the
layout), but is part of the same rebuild anyway for phase 1's reasons.

The BIOS and the kernel's serial shell do not use the USB keyboard's
layout; they stay US.

### Adding a layout

1. Append it to `LAYOUTS` in `tools/gen_kbd_layouts.py`: config name,
   xkb file, xkb section (or `None` for the file's default) and a
   two-letter label. Append, never insert -- ids are positions.
2. Run the generator and commit `sw/common/zkbd_layouts.c`.
3. Add a characteristic key or two to `test_layouts()` in
   `sw/common/tests/test_zkbd.c`.
4. Add it to the table above and to the help text of
   `system.keyboard.layouts` in `sw/common/zcfg.c`.

Nothing else: `settings` and `wm` take the list from the table. At
most 32 layouts fit the five event bits.

### Tests

`sw/common/tests/test_zkbd.c` -- every layout's distinctive keys, AltGr
and its stripped modifier, Caps Lock, Ctrl, the keypad and its decimal,
the layout carried in an event, and every entry in the compose table:

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/test_zkbd \
   sw/common/tests/test_zkbd.c sw/common/zkbd.c
/tmp/test_zkbd
```

## Phase 1: keysyms are Unicode

A keysym (`sw/common/zkbd.h`) is now either:

- a **character**, as its Unicode codepoint (`0x000000`-`0x10FFFF`), or
- a **named key** -- arrows, F-keys, the navigation cluster -- at
  `0x110000` and up (`Z_KEY_NAMED_BASE`), the first value above the
  last Unicode codepoint.

Before this, characters were ASCII only and the named keys sat at
`0x100`-`0x11b`. That range is Latin Extended-A (A-macron, C-acute,
D-caron, ...): no problem while nothing could type those letters, a
collision as soon as a layout can. Every keysym comparison in the tree
already went through the `Z_KEY_*` names, so moving them changed no
behaviour. `sw/common/tests/test_zkbd.c` checks that every usage code
and modifier combination translates exactly as before, apart from the
new named-key values.

### `Z_WM_KEY` carries 23 bits of keysym

`wm` delivers keys to apps as one packed `uint32_t` (`zwm.h`). The
keysym field was 15 bits, bits 23:9 -- too narrow for either Unicode
or the new named keys. Bits 31:24 were unused, so the field now simply
extends to bit 31: 23 bits, up to `Z_KEY_MAX` (`0x7FFFFF`). The pressed
bit, the modifier byte and the low 15 bits of keysym stay exactly where
they were.

### Compatibility

**Rebuild every app.** (libz's ABI version went to 7 in phase 2 for the
same reason -- [libz.md](libz.md).) An app built against the old headers:

- still receives characters correctly (ASCII packs identically), but
- reads every named key as `Z_KEY_NONE`, because its unpack macro
  masks the keysym to 15 bits, and `0x110000 & 0x7FFF` is 0.

So a stale binary on the sdcard keeps typing but loses its arrow and
F-keys. Programs compiled on the machine with `zcc` link `zkbd.o` from
libz, so libz and anything built with it need rebuilding too.

### Raw events

`zkbd.h` also gained `Z_KBD_EV_PRESSED()`, `Z_KBD_EV_USAGE()` and
`Z_KBD_EV_MODS()`, next to the existing `Z_KBD_EV_LOCKS()`, for taking
apart the events `hid_read_key()` returns. `midi`, `play` and `track`
decode those events themselves in their console (window-less) modes,
and had the usage code at bits 7:0 instead of 8:1 -- every key arrived
as a different one -- and acted on releases as well as presses. They
use the macros now, and act on presses only.

### New helpers

| Macro | Meaning |
|---|---|
| `Z_KEY_IS_NAMED(k)` | an arrow, F-key or other named key |
| `Z_KEY_IS_TEXT(k)` | a character that belongs in text: not C0, DEL, C1, a surrogate or a named key |
| `Z_KEY_MAX` | the largest keysym `Z_WM_KEY` can carry |

`Z_KEY_IS_TEXT` is the Unicode-aware form of the `k >= 0x20 && k <
0x7f` test that ASCII-only apps use. Those apps are left as they are:
until they store more than ASCII, ignoring everything else is correct.
