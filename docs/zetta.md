# zetta

**The Zeitlos Editor for TexT Applications**: a full-screen editor for
prose -- messages, notes, a letter -- in the manner of nano. Named for
the SI prefix 10^21: an editor smaller than pico and nano, with the
largest name. (`zepto`, the right size, is already someone's editor.)

`sw/common/zetta.c`, `zetta.h`. Not a code editor: `te` and `nextvi` are
those. Not a windowed app: `text` is that.

**Status**: the core is done and tested (below). **The BBS writes with
it** ([bbs.md](bbs.md), "Writing"): To and Subject as fields, Post and
Quote as the app's actions, the quote window as `zetta_pick()`. **And
`zetta FILE` in posix** (below). The REPL later.

## `zetta FILE`

A text file in posix: `zetta notes.txt`. `sw/apps/zetta`: the session
(`edit.c`, on zplat, tested on the host) and the terminal handoff
(`zetta_zeitlos.c`, as vi's: posix hands `term` over and takes it back
when zetta exits).

- A file's **lines are paragraphs**: shown wrapped, saved as they were --
  a long line stays one line, as nano's soft wrap keeps it. CRLF is read
  as LF (and saved so); a tab is kept, and shown as one space.
- **^O Save** (and ^Z); **Save as...** in the Esc menu -- and what Save
  does when there is no name yet. A save writes `FILE.~` and renames it
  over `FILE`: a failed one leaves the file as it was, and no `.~` behind.
- **^X** with changes asks "Save changes to FILE?" -- `y` saves and
  leaves, `n` leaves, Esc stays.
- A missing file is a new one. A file over 44 KB, or with a NUL byte in
  it (not text), is refused, saying why. The text is a static 48 KB, so
  zetta runs in the default memory tier.

`make -C sw/apps/zetta test`: 27 checks, the screen through the real
terminal emulator and real files -- a new file typed and saved exactly;
a 175-character line saved as one line, the tab kept, CRLF made LF;
leaving with changes (Esc stays, `n` leaves the file untouched, `y`
saves); no name yet (Ctrl-O asks, the title follows); Save as from the
menu; refused files; a save into a directory that is not there --
said, still editing, nothing left behind.

It found a bug in zetta itself: what an app said while handling an
event -- "Saved", "A subject, please." -- was drawn only at the next
key. `zetta_status()` and `zetta_error()` now draw at once, and
`zetta_refresh()` redraws after an app changes the title; zetta's
tests and the BBS's check it (the BBS's fails without it).

## What it does

- **Word-wrapped paragraphs that reflow as you type.** The text is
  paragraphs, with a line break only where Enter was pressed; the screen
  shows each wrapped at words, and a word put in mid-paragraph moves the
  rest along.
- **nano's layout**: a title bar; the app's header fields ("To",
  "Subject"); the text; a status line; two rows naming the keys.
- **UTF-8** inside, counted by characters. What reaches a terminal is
  the app's business: the BBS turns it into CP437 or ASCII per caller.
- **Cheap on a slow link**: each row is fingerprinted, and only the rows
  whose fingerprint changed are sent -- a keystroke costs one row.

## Keys

| key | |
|---|---|
| arrows, Home, End, PgUp, PgDn | move (the common escape sequences of each, decoded) |
| ^A, ^E | start and end of the line |
| Enter | a new paragraph |
| Backspace, Delete | |
| Tab | spaces to the next multiple of four |
| ^K, ^U | cut the paragraph (again: add the next to the cut), paste |
| ^W | search, case ignored |
| ^G | help |
| ^X | exit: the app decides what that means |
| **Esc** | **a menu of everything** -- for terminals where control keys are awkward |
| ^Z | the app's main action (post, save): Mystic's habit |

plus the app's own keys, listed in the help bar and the menu. Up from
the first row goes to the header fields; Enter, Tab or Down leaves one.
A lone Esc is told from an arrow key's first byte by a 150 ms pause.

## Embedding it

zetta never waits and never reads a terminal: the BBS runs one inside
each caller's session, `posix` wraps one in a loop.

```c
zetta_cfg_t cfg = {
	.rows = 25, .cols = 80, .title = "New message",
	.fields = { { "To", to, sizeof(to), check_user }, { "Subject", subj, sizeof(subj) } },
	.nfields = 2,
	.actions = { { ZK_CTRL('S'), "Post", POST }, { ZK_CTRL('Q'), "Quote", QUOTE } },
	.nactions = 2, .main_action = POST,
	.clip = clip, .clip_cap = sizeof(clip),
	.write = send_to_caller, .ctx = caller,
};
zetta_init(&ed, &cfg, body, sizeof(body));
...
switch (zetta_key(&ed, bytes, n, now_ms)) {	// and zetta_tick() when idle
case ZE_ACTION: ...ed.action...                // POST: post it; QUOTE: zetta_pick()
case ZE_EXIT:   ...                           // confirm, save, discard: the app's call
case ZE_ANSWER: ...ed.answer, ed.answer_tag... // a dialog the app opened
}
```

- **The app says what the text is for.** Its actions -- key, label, id
  -- are shown in the help bar and the Esc menu and come back as
  `ZE_ACTION`; Exit comes back as `ZE_EXIT`. zetta never posts, sends or
  saves anything.
- **Header fields** are the app's buffers, each with an optional check
  run when the field is left ("There is no user called nobody.").
- **Dialogs the app opens**: `zetta_confirm()` (y/n), `zetta_prompt()`
  (a line), `zetta_pick()` (choose lines and put them in after a prefix:
  the BBS's quote window). Each is answered by `ZE_ANSWER` with its tag.
- `zetta_status()` / `zetta_error()` for the status line, and
  `zetta_text()` for the text: as typed, or as lines of at most N
  columns (the BBS stores 79).
- **No heap.** `zetta_t` (~1 KB) is the caller's, and so are the text,
  the fields and the cut buffer. Input that arrives after an event is
  held in the editor itself -- the BBS has one a caller -- and taken
  first next time: nothing typed is lost.

On Zeitlos it is 12 KB of code and 512 bytes of static data.

## Wrapping, exactly

One function decides every wrap, for the screen and for `zetta_text()`,
so what is saved is what was shown. The text is one column narrower than
the screen, so writing its last column never leaves a terminal about to
wrap. A row is as much of the paragraph as fits, ending:

- after the **last space that fits** (the space shown at the row's end);
- or, when the character just past the edge is a space, **there** --
  that space taken into the row, not shown; the cursor after it is on
  the next row, which exists even at the very end of the text;
- or, for a word wider than the whole row, **mid-word**.

## Testing

`make -f tests/Makefile.zetta` (in `sw/common`), 68 checks. zetta's
output goes into the real terminal emulator (`zvt100.c`), and the checks
are on what a terminal shows -- its cells and its cursor:

- the screen: title, fields, help bar; the cursor starting in an empty
  field; Enter leaving it; wrapping at words, a word put in reflowing
  the rest; `zetta_text()` at 79 columns and as typed;
- Enter, Backspace and Delete across paragraphs, cut and paste, search;
- the app's keys and Ctrl-Z; an arrow's Esc not taken for the menu, a
  lone one after a pause opening it; the menu's choices; Ctrl-X; bytes
  typed after an action kept, not lost; two editors at once keeping
  their held input apart;
- the dialogs: a confirm with its tag, the quote window putting in the
  chosen lines after their prefix; a field's check refusing, then
  accepting;
- UTF-8 wrapped by characters, not bytes; Latin characters shown as
  themselves; one keystroke costing one row (under 250 bytes);
- **60,000 random keys** -- typing, Enter, Backspace, Delete, every
  movement, cut and paste, UTF-8 -- over five seeds at 80 columns and at
  40: after every key, the text area and the cursor must match a
  **reference wrapper written apart from zetta's** (on characters, not
  byte offsets), and the text saved must keep every line within the
  width.

The random keys found one real case: text ending in a space exactly one
column past the edge put the cursor on a row that was never drawn (now:
that row exists). Breaking that fix, the space-past-the-edge rule, the
row fingerprints, or the held input each fails the tests.

## Limits

- Characters are one cell wide: double-width ones (CJK, emoji) are
  counted as one, as `term` shows them.
- A tab already in a text shows as a single space; the Tab key types
  spaces.
- The quote window chooses among its first 256 lines.
