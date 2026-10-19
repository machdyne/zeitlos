# text

A plain, fast, word-wrapping notepad. `sw/apps/text`. A core app: it
is in flash and works with no card ([flash_apps.md](flash_apps.md)).

```
> run wm
> run text
```

Deliberately not a programmer's editor. `sw/apps/repl` already embeds
`te` (`docs/editor.md`), which is modal, vi-flavoured and built around
lines as the unit of everything. This one is for notes, letters and
stories — so it wraps, it has no modes, and every key that isn't a
command inserts itself.

It is also the first app to use the file dialogs and the scrollbar
widget, so it doubles as the worked example for `docs/widgets.md`.

## Using it

**F1** shows a short help box; its **Read docs** button opens this page ([app_help.md](app_help.md)).

Titlebar buttons, left to right: **new**, **open**, **save**, **close**.
The title shows the current filename, prefixed with `*` when there are
unsaved changes, and followed by `(Latin-9)` or `(Windows-1252)` for a
file that will be saved in that encoding -- see "Encodings" below.

| key | does |
| --- | --- |
| any character the keyboard layout types | inserts it -- accented letters, the euro sign, anything ([keyboard_layouts.md](keyboard_layouts.md)) |
| Enter | new paragraph |
| Tab | four spaces — see "Tabs" below |
| Backspace / Delete | delete the character before / after the caret |
| arrows | move the caret; up and down keep the column |
| Home / End | start / end of the display line |
| PageUp / PageDown | a screen at a time |
| mouse wheel | three lines a notch; the view moves, the caret stays |
| Ctrl+N / Ctrl+O / Ctrl+S | new / open / save |
| Ctrl+Shift+S | save as -- a new name, or a new place |
| Ctrl+Q | close |
| Shift + any movement key | extend the selection |
| click and drag | select |
| Shift+click | extend the selection to here |
| Ctrl+A | select all |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Ctrl+Z | undo |
| Ctrl+Y or Ctrl+Shift+Z | redo |
| Ctrl+F | find |
| Ctrl+G or F3 | find the same text again |
| Ctrl+H | replace -- all at once, or one at a time |
| Ctrl+L | go to line; also says the line you are on, and the words and characters |
| right-click | copy the selection |

Clicking places the caret. Clicking past the end of a line, or below the
last one, clamps to the nearest real position rather than being ignored
— clicking in the empty space under a short document puts the caret at
the end of it, which is where you were pointing.

Anything that would discard unsaved work — new, open, close — asks
first, and offers to save. If that save is then cancelled or fails, the
whole operation is off; silently discarding after a failed save would be
the worst possible reading of "yes, save it".

The window is resizable. A resize rewraps the document rather than
scrolling it. The scrollbar stops short of the bottom-right corner so
the resize grip stays clickable — see `Z_WIN_GRIP_INSET` in
`docs/widgets.md`.

## The document

A **gap buffer**: the text is `doc[0..gs)` followed by
`doc[gs+gl..cap)`, with all the free space -- the gap -- between. Until
0.0.7 it was one flat array, and every keystroke moved everything after
the caret, which at 32KB was a few milliseconds and would have been a
third of a second per key in a megabyte.

The gap is always at the **end of a paragraph**: just after a newline,
or at the end of the document. So no display line, no word and no
character ever straddles it, and anything that reads a line, a word or a
character takes a plain pointer to it (`P()`, `B()`); only saving, Replace
All and a copy of a selection that spans paragraphs put the text in one
run first (`doc_flat()`). An edit moves the gap to the end of its own
paragraph -- nothing, if the last edit was in the same paragraph -- and
then moves only the rest of that paragraph. **Typing costs its own
paragraph, wherever in the document it is.**

**Where it lives.** A document of up to `TEXT_MAX` = 32KB is in `sbuf`,
in `.bss`, as it always was: a small file never touches anything else,
and a 1MB board is exactly as it was. Past 32KB the document moves to
the app's **arena** ([kernel.md](kernel.md#arenas)), one block of memory
beside the app sized while it runs, with the text at the bottom and the
line table above it. The arena is grown by doubling when an edit would
not fit, so a typed character almost never waits for it, and given back
when a small document is opened or a new one started. A grow may move
the arena, so the editor holds offsets into it, never pointers across a
call. There is **no fixed document cap**: the limit is the memory the
board can spare (the kernel keeps enough clear to start the largest core
app). Where there is none -- a 1MB board -- a file past 32KB is "too
large", as it always was.

The buffer is not NUL-terminated as a rule; `len` is the length and the
text may contain any byte. Anything handing a string to a drawing call
copies out a bounded piece first.

Files are read with `fs_open_read()` and `fs_read_chunk()` straight into
that buffer, **not** with `fs_mallocfile()`. The latter would allocate a
second copy of the whole file out of a heap that is 16KB shared with the
stack (`Z_PROC_STACK_SIZE_DEFAULT`, `sw/os/kernel.h`), so a 20KB
document would fail to open for no reason the user could ever guess. In
the arena, reads and writes go through a 4KB buffer of the app's own:
`fs` writes into a client through the MPU's grant window, which covers
the app's block and not its arena, and a message carries only memory in
the sender's block. CRLF is normalized to LF on the way in.

## Encodings

**The buffer holds UTF-8.** Every offset in `text.c` is still a byte
offset; what changed is that a character can be more than one byte, so
the three things that care walk characters instead of adding one:

- **the caret** -- Left, Right, Backspace and Delete step over a whole
  character (`next_off()` / `prev_off()`);
- **columns** -- wrapping, Up/Down, End, the caret's x position and
  mouse clicks count display columns (`width_at()`, `chars_between()`,
  `line_pos()`): a CJK character takes two, a combining mark none. A
  wide character never straddles the right edge; a line of Japanese,
  which has no spaces, breaks between any two characters;
- **drawing** -- rows go through `z_win_draw_utf8()`, so every
  character in ISO 8859-15 is a hardware glyph, Japanese is drawn from
  the `jfont` service at 6x12, and anything else is the missing-glyph
  box ([text_encoding.md](text_encoding.md), "Japanese").

A byte that is not valid UTF-8 is **one character**, drawn as the box,
and is kept exactly as it is: the caret steps over it, Delete removes
it, and a save writes it back unchanged. So a file this editor cannot
fully decode is never corrupted by opening and saving it.

### Reading a file

| the file | read as | saved as |
|---|---|---|
| ASCII only | UTF-8 | UTF-8 |
| starts with a UTF-8 byte order mark | UTF-8; the mark is taken off | UTF-8, with the mark put back |
| more valid UTF-8 sequences than invalid bytes | UTF-8; stray bad bytes kept as they are | UTF-8 |
| otherwise, with a byte in 0x80-0x9f that Windows-1252 defines | Windows-1252, converted to UTF-8 in the buffer | Windows-1252 |
| anything else | ISO 8859-15 (Latin-9), converted to UTF-8 in the buffer | Latin-9 |

The test is `detect_enc()`: text written as UTF-8 is all valid multi-
byte sequences; text written in a single-byte encoding is nearly all
bytes that cannot start one (ä in Latin-1 is the lone byte 0xE4). The
byte order mark is taken off because it is not text -- it would sit in
front of the first character as a box.

Of the two single-byte encodings, **Windows-1252** is what Notepad's
"ANSI", Word's "plain text" and most mail clients wrote for decades:
Latin-1 from 0xA0 up, and in 0x80-0x9F the characters a word processor
puts everywhere -- curly quotes, en and em dashes, the euro, the
ellipsis, Œ, Š, Ž, ™. Latin-9 has control codes there that nobody types
into a text file, so **one such byte is the tell**. Without one the two
read the same apart from eight letters, and Latin-9 is kept. (Before
0.0.7 a Windows file opened as Latin-9 and every quote was a control
code, drawn as the box.) The quotes and dashes have no glyph in the
hardware fonts, so they are drawn as the straight quote and hyphen they
look like ([text_encoding.md](text_encoding.md#look-alikes)); the
document holds the real characters, and saves them.

A single-byte file is converted **in place**, backwards so the growing
output never overtakes the input, because there is no room for a second
copy (see "The document" above). Every accented letter doubles, so a
file under 32KB can be too large once converted; that is refused with a
message and nothing loaded. **Every byte survives**, 0x80-0x9F
included: ISO 8859-15 leaves those as control codes, and they are kept
as the codepoints of the same value so that any file read as Latin-9 is
written back byte for byte. Windows-1252 does the same with the five
bytes it leaves undefined (0x81, 0x8D, 0x8F, 0x90, 0x9D).

### Saving

A file is written back in the encoding it was read in, a piece at a
time through `fs_open_write()` / `fs_write_chunk()` -- again, no second
copy. New documents are UTF-8.

A Latin-9 or Windows-1252 file that has gained a character its encoding
cannot hold -- a Japanese character, a Polish letter, an em dash in a
Latin-9 file -- asks before saving:

- **Yes** saves it as UTF-8 from now on, which holds everything;
- **No** saves the old encoding anyway, with `?` in place of each such
  character;
- **Cancel** saves nothing.

### Paste and speech

The clipboard is UTF-8 (the rule in [text_encoding.md](text_encoding.md)),
so a copy is the selected bytes and a paste inserts them. A paste that
does not all fit is cut at a character boundary, never half-way through
one. The speech echo sends whole characters.

### Tests

`sw/apps/text/tests/utf8.c` runs the real `text.c` against a scripted
kernel with an in-memory filesystem: typing non-ASCII, Backspace and
Delete over multi-byte characters, caret columns, wrapping and Up/Down
by characters, a UTF-8 file with a byte order mark, a Latin-9 file with
all 128 high bytes saved back byte for byte, an edit that fits Latin-9
and one that does not, a UTF-8 file with a malformed byte, and the
too-large refusal. Given an argument it also writes a picture:

```
cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/text_utf8 \
   sw/apps/text/tests/utf8.c sw/common/zwin.c sw/common/zwidget.c \
   sw/common/zflist.c sw/common/zedit.c sw/common/zfsapp.c \
   sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
   sw/common/zspeak.c sw/common/zsayall.c sw/common/zdialog.c
/tmp/text_utf8 /tmp/text          # also writes /tmp/text-utf8.pbm
```

Needs `-no-pie` and `vm.mmap_min_addr=0`, like the other app-level host
tests (`sw/common/tests/ztramp.h`); exits 77 without them.

## Wrapping

The line table holds where each display line starts. It is a **gap
buffer too**: lines before its gap are stored as their offset, lines
after it as their **distance from the end of the text**. An edit is made
with the table's gap just after the edit's own line (`lines_at_edit()`),
so every line below the caret is held as a distance from the end, and
that does not change when text before it grows or shrinks. A keystroke
therefore rewrites only its own paragraph's entries and moves the
table's gap by about one line; before 0.0.7 it rewrapped **everything
after the caret** on every key.

In `.bss` the table is 16 bits wide and `MAX_LINES` = 4096 long, which
holds both kinds of entry for a 32KB buffer and is unreachable for 32KB
of prose; a file of nothing but short lines that needs more moves to the
arena, where the table is 32 bits wide and grows with the document. On a
board with no arena, at the cap the document stops being *displayed*
past that point; it is not truncated on disk and editing above the cap
still works.

The property everything rests on is that **wrapping is
paragraph-local**: where a line breaks depends only on the text since
the last `\n`. So an edit can never change how anything before its own
paragraph is laid out, and after an edit `layout_sync()` wraps from the
start of the paragraph to the first paragraph start past the change,
where the old table takes over again. Only a resize or a font change
rewraps everything, because a column-count change genuinely can move
every line; so do loading and Replace All.

**That whole-document wrap runs in the background.** It wraps as far as
the caret and one slice (`WRAP_SLICE`, 8KB) more, which is enough to
draw the window, and the main loop wraps the rest a slice at a time
between messages, the scrollbar growing as it goes. While it runs the
table ends at the **frontier**, the start of the next line still to wrap,
held -- like the lines after the table's gap -- as its distance from the
end, so typing before it leaves it right. Anything that needs a line
past it wraps up to there first: finding a line (`line_at()`), moving
down, scrolling, the scrollbar, reading aloud, and an edit, which first
wraps past the end of its own paragraph so that `layout_sync()` can stop
at the frontier. A 1.1MB file is drawn after 151 lines are wrapped; the
other 19,690 take 133 slices.

Lines include their terminating `\n`, and a wrapped line includes the
space it broke after, so every byte of the document belongs to exactly
one display line and nothing has to special-case "is the character
before me a newline".

A wrapped line also **absorbs the whole run of spaces at its break**, so
a line can never begin with one. Without that, two ordinary cases
produce a line holding nothing but spaces, which renders as a blank line
in the middle of a paragraph: a word that exactly fills the width
followed by a space, and a double space after a full stop that happens
to straddle the break. The absorbed spaces make a line longer in bytes
than the column count, which is why `line_col_max()` exists — trailing
spaces are invisible, `draw_row()` clamps what it paints, and the caret
clamps to the same limit so it can't follow them out over the scrollbar.

A word longer than the whole line is broken mid-word. Letting it run off
the edge would be prettier and less useful: a URL or a line of pasted
code should still be readable.

### What a keystroke costs, measured

`tests/utf8.c` section 18 loads a 1.1MB file of 19,841 display lines and
counts, per key typed, the bytes moved, the bytes wrapped and the line
entries changed:

| typing at | bytes moved | bytes wrapped | line entries changed |
|---|---:|---:|---:|
| the start | 108 | 162 | 1 |
| the middle | 46 | 192 | 0 |
| the end | 92 | 192 | 1 |

-- the size of one paragraph, wherever it is. With a flat buffer and a
plain table the same keystroke at the start would have moved 1.1MB and
rewrapped 1.1MB.

## Drawing

Every glyph goes through the hardware glyph blitter, via
`z_win_draw_text()` (`-DZ_GFX_HW_BLIT`). Text is drawn a display line at
a time, and an edit repaints only from the edited line down — reflowing
a paragraph can move every line after it, but never one before. A cursor
move repaints just the caret's old row and its new one.

`repaint()` also blanks the strip between the text and the scrollbar and
any partial row at the bottom, because `wm` clears before most redraws
but **not** after a move (`repair_drag()` in `wm.c` deliberately
excludes the window's own final footprint), so anything not actively
rewritten keeps its pre-move contents. Same reasoning as `draw`'s
`clear_panels()`.

The caret is a 1px vertical rule between characters, not a filled block
over one. A block caret hides the character it sits on and is also just
wrong about where the text will go, for something that lives *between*
two characters.

## Tabs

Tab inserts four spaces rather than a tab character. `wrap_one()` counts
columns, and a real tab has no fixed width in that count, so honouring
one would mean teaching both the wrapper and the renderer about tab
stops. A tab can therefore only appear in text loaded from disk, and is
drawn as a single space so it at least occupies its one column.

## Font

Two fonts, toggled by the titlebar font button: `z_font_5x8` and
`z_font_6x12`. Both are resident in glyph memory at fixed offsets and
loaded once by `wm`, so both draw through the hardware blitter — see
`docs/window_manager.md`, "Fonts in glyph memory".

Switching changes the column count, so the document rewraps from the
top. That is the same case a resize is, and the one place
`wrap_from()`'s paragraph-local shortcut cannot help, since every line
can move.

The caret stays on the same **character**, not the same screen
position: `cursor` is a buffer offset and survives the rewrap
untouched. That is precisely why the buffer and the line table are
separate things.

## Launched with a file

If the file browser (or anything else) starts `text` with a pending
launch argument, it is claimed at startup and opened — see "Launch
arguments" in `docs/widgets.md`. The claim happens even when the
editor doesn't want it, so a stale argument can't be left for whatever
the user opens next.

## Selection

An **anchor** plus the cursor. The selected range is whichever order
they happen to be in, so extending backwards needs no special case and
the caret stays the moving end — which is what makes shift+arrow and
shift+drag feel right.

`-1` means no selection, deliberately rather than `anchor == cursor`:
a selection can collapse to zero width mid-drag without ceasing to
exist, and treating that as "none" makes the next shift+arrow start
from the wrong place.

Selected text is drawn by splitting each line into up to three runs —
before, inside, after — and drawing the middle one through
`z_fb_draw_text2()` with the colours swapped. That's one hardware
glyph blit per character, exactly like the normal path; inverting
afterwards with a fill would be a second pass over the same pixels and
would fight the no-flash rule. A selection continuing onto the next
line also highlights the rest of the row past the text, or a
multi-line selection reads as a stack of ragged fragments.

Movement redraws the **union of the old and new selected ranges**, not
just the two rows the caret touched — with a selection those ranges
are what changed appearance. Repainting the whole text area on every
shift+arrow would be hundreds of glyph blits per keystroke.

`delete_selection()` is shared by every path that replaces a selection
— typing over it, Backspace, Delete, Cut and Paste — so one place
knows how a selection becomes an edit. Paste does the delete and the
insert as a single edit rather than calling both, which would rewrap
and repaint twice for one user action.

Shift+click needs the modifier state, and `Z_WM_MOUSE` carries buttons
but no modifiers, so the last-seen modifier byte from `Z_WM_KEY` is
correlated with the click.

### Three bugs worth remembering

All three presented as *"text isn't displayed"*, which is nowhere near
where any of them lived.

**Half-clamped range.** `draw_row()` clamped `a` only against 0 and
`b` only against `n` — each end in the direction that matters for a
selection *overlapping* the line. For a line entirely outside it, the
other direction bites: a line past the selection's end gets a negative
`b`, and the trailing run then draws `tmp + b`, reading before the
buffer, at a negative x that clips away entirely. The line vanished.
Which lines vanished depended on where the selection was, so it looked
like an unrelated rendering fault.

**Stale collapsed anchor.** A click sets `anchor == cursor`, which is
not a selection. Nothing cleared it, so the moment an edit moved the
cursor, `anchor != cursor` and a selection sprang into existence that
the user never made — which, with the bug above, hid everything after
it. `after_edit()` now clears the anchor, and a release that selected
nothing drops it.

**Titlebar clicks.** wm forwards pointer samples to the focused window
whenever the cursor is over it, and its hit test is the whole window
rect — titlebar included. A drag is suppressed while wm owns it, but a
click on a titlebar *icon* is not: wm handles the icon and forwards
the same press, with negative content y. Clamping that to row 0 meant
clicking Save also placed the caret at the top and started a selection
there. `handle_mouse()` now ignores samples outside the content area
unless a drag is already in progress — that exemption matters, because
a drag legitimately leaves the window and must keep extending.

## Undo

**Ctrl+Z** undoes, **Ctrl+Y** (or Ctrl+Shift+Z) redoes. One undo takes
back a *group* of edits:

- a run of typing, up to and including an Enter -- so undo goes back a
  line at a time, and a sentence typed without Enter is one step;
- a run of Backspace, or of Delete;
- anything that replaces a selection -- typing over it, Paste -- and
  every Cut, with the selection coming back as it was.

Moving the caret, or switching between typing and deleting, starts a new
group. After an undo the caret is where it was before the edit.

**Memory.** An app has 16KB for heap and stack together, so the document
is never copied: what is kept is the edits -- the last 256, and 6KB of the
text they inserted or deleted -- in fixed rings (`UNDO_RECORDS`,
`UNDO_BYTES`). When they fill, the oldest edits are forgotten; undo goes
back as far as they reach. A single edit larger than 6KB (a big paste)
cannot be undone, and clears the history before it. Opening or starting
a document clears it too.

**The modified mark follows it.** Saving marks the point; undoing or
redoing back to exactly the saved text clears the `*` from the title,
and undo stops at that point even in the middle of a group, so the saved
text can always be reached.

All of this is `sw/common/zundo.h` -- generic over any byte buffer, with
no app types in it. `text.c` tells it every edit through `edit_insert()`
and `edit_delete()`, the only two places the buffer changes (loading
aside, which starts the history over), and decides the groups.

**Undo goes through the gap.** The log puts its edits back through two
operations the editor supplies (`z_undo_undo_ops()`, `z_undo_ops_t`),
which `text` implements as the same gap edits typing uses, line table
and all. So undoing a word costs its paragraph, as typing it did, and
undoing a Replace All walks the document once rather than moving it once
per match. The plain-buffer `z_undo_undo()` is the same loop over a
`memmove` pair.

## Find

**Ctrl+F** asks for the text (the dialog's field takes any character,
German and Japanese included) and selects the next match after the
caret, wrapping round to the top; **Ctrl+G** or **F3** finds the same
text again. ASCII letters match either case, as the posix shell's
wildcards do; everything else matches exactly, so accented and Japanese
text is found as typed. A search with no match says so.

## Replace

**Ctrl+H** asks two things: what to find (the selection, if it is a
short piece of one line, else the last search), and what to put in its
place -- which may be **nothing**, the way to delete every match
(`z_dialog_prompt_empty_ok()`, `zdialog.h`). Matching is find's. Then it
says how many matches there are and asks:

- **Yes**: replace them all, at once;
- **No**: one at a time -- each match is selected and shown, and asked
  about: Yes replaces it, No leaves it, Cancel stops. It goes round the
  document at most once, so saying No to every match ends;
- **Cancel**: nothing.

**Replace All is one step of undo**, however many matches: one Ctrl+Z
puts every one back, case and all. It is also **one pass over the
document**, not an edit per match, which would move the rest of the
document once per match -- a thousand replacements in 32KB would be 32MB
of copying, several seconds. When the replacement is longer than what it
replaces, the document is first moved to the top of the buffer, so the
output -- which grows -- never overtakes the text it is read from; the
same trick as the Latin-9 conversion, the other way round. A Replace All
that would not fit in 32KB is refused and changes nothing.

The undo log holds 256 edits and 6KB of text (below), and a replacement
is two edits. When a Replace All is bigger than that it **says so and
asks first**, and if you go ahead the history is cleared rather than
left holding the last part of the replace -- an undo that put back only
some of the matches would be worse than none.

One-at-a-time replacements are an edit each, undone one by one.

`replace_all_core()`, `replace_selection()` and `count_matches()` are
the parts with no dialog in them, and what `tests/utf8.c` section 14
drives.

## Go to line

**Ctrl+L** says where you are and how much there is -- line 12 of 340,
1,204 words, 7,301 characters -- and asks for a line to go to, the
current one filled in. Lines are the **file's** lines, what a compiler
or another editor numbers, not the wrapped display lines; words are runs
of anything but spaces, tabs and newlines, and characters are
characters, not bytes. A number past the end goes to the last line. The
line arrived at is spoken, like any other move.

## Save As

**Ctrl+Shift+S** is Save As: the save dialog, starting in the directory
of the last dialog, with the file's own name filled in. Saving onto
**another** file that already exists asks first ("Replace it?"); the
save dialog leaves that to its caller (`zdialog.h`), and before 0.0.7 a
new document's first save overwrote a file of the same name without a
word. The document then belongs to the new name, in the same encoding.


### Other systems' formats

Saving as **`.docx`, `.odt`, `.rtf` or `.html`** exports: the document
is written to `/tmp` as it is -- Markdown if it is a `.md`, plain text
otherwise -- and handed to `convert` ([convert.md](convert.md)), whose
own window shows the progress, **Cancel** and any error. An export is a
copy: the document keeps its own name and its unsaved state. A `.md`
exports with its headings, lists and emphasis as Word or LibreOffice
headings, lists and bold/italic.

Opening a `.docx`, `.odt`, `.rtf` or `.epub` (from the Open dialog, or
by double-clicking one in `files`) hands it to `convert`, which turns
it into Markdown in `/tmp` and opens that in a new `text` window -- or
in `read`, when it is larger than `text` can take on this machine.

**A file in `/tmp` saves with Save As**, starting in `/home`: `/tmp` is
the RAM disk and is gone at the next boot, and the usual reason a
document is there is that `convert` put it there. Plain **Ctrl+S** on
such a file asks where, every time, until it is saved elsewhere.

## Size

`text` is a core app, in flash, and its RAM is the binding budget: the
apps that start at boot plus the two largest others have to fit a 1MB
board ([flash_apps.md](flash_apps.md), "Budgets"). Replace, Go to line,
Save As and Windows-1252 were paid for by building with `ZFMT_LITE`
(stdout without newlib's stdio, [build.md](build.md)): `text` prints
only whole lines to the console log and never calls `fflush()`. Measured
with the same compiler before and after, `text` went from 57,544 to
55,319 bytes in flash and from 156,976 to 152,808 in RAM -- smaller than
before, with the features. The gap buffer, the line table's gap and the
arena then took it to 57,620 and 156,812; background wrapping and
autosave took 2.6KB more, which was paid back by `ZFMT_NO64` (no 64-bit
division, 2.3KB -- `text` prints no 64-bit number) and by the scrollbar
doing its arithmetic in 32 bits (`sb_muldiv()`, `zwidget.c`, 1.3KB in
every app with a scrollbar). With everything in 0.0.7, `text` is 155,396
bytes in RAM against 156,976 in 0.0.6.

## Autosave

Two seconds after the last edit, a copy of the document goes to the
**RAM disk** (`/ram`) -- never to the card. Writing to the card at a
moment the person did not choose is how a hard power-off corrupts it, so
on a board with no RAM disk there is no autosave at all. The copy is
written **4KB per pass of the main loop**, so typing is never held up by
it, and an edit in the middle abandons it; it starts again at the next
pause. It is written as `T<hash>.NEW` and renamed over `T<hash>.SAV`
only when complete, so a `.SAV` is always a whole document; `T<hash>.INF`
beside it records which file it is a copy of and the encoding that file
is saved in. The hash is of the file's path (an 8.3 name,
`/ram/T1A2B3C4.SAV`), so finding the copy of a file is one open, not a
search.

**Saving deletes the copy**, and so does answering No to "Save changes
before continuing?". If `text` ends any other way -- a crash, a kill --
the copy stays, and the next time that file is opened `text` says
unsaved changes from a session that did not end were kept, and offers
them back: Yes puts them in the document, still unsaved and still
belonging to the file and its encoding; No deletes the copy. A document
that was never saved has one slot of its own, offered when `text` starts
with no file.

What it does not cover is the power going off: the RAM disk does not
survive a reboot, by design. It protects the work from `text` itself,
not from the machine.

## Not implemented

Nothing planned for 0.0.7 is left out. Two things that are not done:
the undo log is fixed at 256 edits and 6KB of text (above), and a
document that was never saved has a single autosave slot, so two
untitled windows share it.

## Tests

Undo and find run through the real `text.c` in `tests/utf8.c` (sections
11 and 12): typing, Enter, Backspace runs, typing over a selection,
multibyte text, the saved point, and find with case, wrapping and a
German word. Section 13 is Windows-1252 (detection, all 256 bytes back
byte for byte, an em dash and Œ saved to their bytes, hiragana counted
as unsavable, and a Latin-9 file with no tell staying Latin-9); 14 is
replace (shorter, longer, empty and non-overlapping replacements, caret
placement, one undo and redo for all, too many to undo, too large to
fit, and one-at-a-time); 15 is go to line and the counts. 16 makes 4,000 random inserts and
deletes and checks the text against a model and the line table against
a full rewrap after every one; 16b undoes and redoes 60 random edits,
group by group, checking the table at each step; 17 is a 400KB file in
the arena: typing in three places and two hundred, undo, pastes that
grow the arena through moves, Replace All, going back to `.bss`, and
refusal where no arena can be had; 16c edits a document while it is
still being wrapped in the background -- before the frontier, paging
past it, beyond it, Replace All restarting it, undo; 18 is the megabyte
measurement above, the background wrap counted, and random edits on it;
19 is autosave: nothing before the pause, pieces after it, an edit
abandoning a copy, the copy byte for byte with its note, saving deleting
it, and a copy taken from the arena across the gap. `sw/common/tests/test_undo.c` covers the log on
its own, including what happens when it fills.

## Speech

### Echo while editing

With speech on, the editor says what you are doing, which is what
makes writing without the screen possible:

| action | says |
|---|---|
| Left / Right | the character stepped over ("semicolon", "new line") |
| Up / Down / PageUp / PageDown / Home / End | the whole line, or "blank" |
| finishing a word (space or punctuation) | the word just typed |
| Backspace / Delete | the character removed, said before it goes |

Characters are not echoed as they are typed -- at typing speed that is
noise -- but a word is, when the thing that ends it arrives. Everything
interrupts, because an echo that queues arrives after you have moved
on. A blank line says "blank" rather than nothing, since silence is
indistinguishable from the key not working.

### Reading aloud

**Super+A** reads the document aloud, a display line at a time, with
the caret following the voice ([tts.md](tts.md)). Super+A again, any
key, a click or a Ctrl tap stops it, and the caret stays on the line
you last heard -- so moving somewhere and pressing Super+A reads from
there. With the caret at the very end of the document (where it is
after typing), it reads from the top. An empty document says so.

**Super+C** says the highlighted text, once -- "copy to audio" -- and
leaves the clipboard as it was; with nothing highlighted it says
"Nothing selected". **Super+V** says what is on the clipboard.

The window is created with `Z_WIN_FLAG_READABLE`, and the pacing is
`sw/common/zsayall.c`: `read_get()` hands it a line (flagged
`Z_TTS_F_CONTINUES` when the line wrapped mid-paragraph rather than
ended), `read_at()` moves the caret, and every key and click calls
`z_sayall_stop()` before doing anything else.

