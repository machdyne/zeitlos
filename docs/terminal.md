# term

A VT100 terminal window. `sw/apps/term`.

Most of what it does is documented in the file itself; this covers
selection, the clipboard, and where line editing actually lives —
which is the thing most likely to be looked for in the wrong place.

## Selection and clipboard

Drag with the left button to select, release to keep it. Selection is
**reading order**, not a column rectangle: partial rows at each end,
full rows in between, which is what a terminal selection means.

| key | does |
| --- | --- |
| Ctrl+Shift+C | copy the selection |
| Ctrl+Shift+V | paste |
| right-click | copy the selection |

**Ctrl+Shift, not plain Ctrl, and that is deliberate.** Ctrl+C in a
terminal is `^C` to the far end — the single most-used key in a shell
— and rebinding it to copy would be indefensible. `sw/apps/text` uses
the plain Ctrl forms; the difference between the two apps is a
decision, not an inconsistency.

`z_kbd_usage_to_keysym()` folds Ctrl+letter to `0x01..0x1A` regardless
of Shift, so the shift bit is what distinguishes these from the
control codes they would otherwise be.

Right-click copies, acting on the press — there's no drag gesture on
that button, so waiting for the release adds nothing. It sits *after*
the titlebar guard, so a right-click on the titlebar doesn't copy.

Copying strips trailing blanks from each row — a terminal grid is
padded with spaces to the full width, and copying it verbatim gives
every line a tail of whitespace nothing wants pasted back. Rows other
than the last get a newline.

Selection covers the **visible grid only**. There is no scrollback in
`zvt100` — `vt.cells[VT_ROWS][VT_COLS]` is the whole of it — so there
is nothing above the top row to select.

Any key other than the clipboard pair drops the selection: the far end
is about to echo something back, and the selection would stop
describing what is on screen.

Selection redraws are explicit. `vt_row_dirty()` only tracks cells the
*emulator* changed, and a selection is drawn on top of unchanged
content — invisible to that tracking.

### Paste makes no assumption about lines

Pasted bytes go out the port exactly as typed keys do. Against `sh`
each newline submits a command, which is correct. Against a reader
that knows it is mid-form, the same bytes accumulate instead. Neither
behaviour belongs to `term`, so it does not have one.

## Line editing lives at the far end

`term` is a **dumb terminal**. `key_to_bytes()` already sends arrows,
Home/End, PageUp/Down and Delete as proper VT100 sequences. Nothing
about cursor movement or insert-in-the-middle is missing here.

What reads those bytes decides what they mean:

- **`readline()` (`sw/common/zeitlos.c`)** — the serial console's
  reader, used by `sh`. Deliberately minimal: Enter and backspace,
  nothing else. The console is the recovery path of last resort and
  complexity there is complexity in the one thing that has to work
  when everything else does not.

  It does now *swallow* escape sequences rather than appending them to
  the line. An arrow key sends `ESC [ D`, and those three bytes used
  to land in the buffer as literal characters — pressing Left to fix a
  typo silently corrupted the command. Discarding them means the keys
  do nothing, which is what a minimal reader should do rather than
  something actively wrong.

- **`zline` (`sw/common/zline.h`)** — the line discipline for port
  providers, used by `repl`. This is where richer editing lives:
  cursor movement, insert, delete, kill and history all work there
  now. See `docs/line_editing.md`. Multi-line continuation is still to
  come, and paren-balance logic layers on top rather than living
  inside.

Multi-line entry — typing a newline without submitting, then moving
back up to edit the first line — needs the reader to own a multi-row
buffer and repaint it across several terminal rows. That is a
different thing from single-line editing, and belongs with `zline`
and `repl`, not here.
