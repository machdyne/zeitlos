# Line editing

`sw/common/zline.c` — the line discipline used by port providers.
`repl` is its only consumer today; any other provider wanting a
command line uses the same code.

## Where it lives, and why not elsewhere

`term` is a **dumb terminal**. It already sends arrows, Home/End and
Delete as proper VT100 sequences (`key_to_bytes()`); whatever reads
those bytes decides what they mean. So editing belongs at the reading
end, not in the terminal — which also means it works unchanged if
`term` ever talks to something remote.

There are three readers, and only one of them got this:

- **`readline()` (`sw/common/zeitlos.c`)** — the APP-side minimal
  reader. Enter and backspace, plus swallowing escape sequences so
  arrows can't corrupt the line.
- **`readline()` (`sw/os/kruntime.c`)** — a SECOND, separate copy,
  and the one the kernel shell (`sw/os/sh.c`) actually calls. The
  kernel does not link `sw/common/zeitlos.c`.
- **`zline`** — this. Full editing.

**The first two are independent copies of the same function and have
drifted.** That is worth stating plainly, because it hid two real bugs
in the kernel copy for as long as it existed:

  - Backspace cleared `buf[pl]` without decrementing `pl`, so it
    erased the character on screen and left it in the buffer. Typing
    `ls`, backspace, `d` displayed "ld" and ran "lsd". The app-side
    copy always had the decrement.
  - Backspace on an empty line matched no branch (the emptiness test
    was in the condition, not the body) and fell through to the
    printable case, which echoed it and stored it — walking the
    terminal cursor back over the prompt and putting `0x08` in the
    command. This one was present in BOTH copies.

Both are fixed. Neither would have survived a doc that said there were
two copies to keep in step; this file said there was one. If a third
behaviour is ever added to either, add it to both or delete one.

The minimalism is deliberate in both: the console is the recovery path
of last resort, and complexity there is complexity in the one thing
that has to work when nothing else does. Richer editing belongs in
`zline`.

## What it does

| key | does |
| --- | --- |
| left / right, Ctrl+B / Ctrl+F | move the cursor |
| Home / End, Ctrl+A / Ctrl+E | start / end of line |
| Backspace, Delete, Ctrl+D | delete |
| Ctrl+K / Ctrl+U | kill to end of line / whole line |
| up / down | move between rows, or history |
| Ctrl+C | abandon the input |

Typing inserts at the cursor. `pos` is separate from `len` — they were
one variable when this could only append, which is what made it not an
editor.

History is a `z_line_hist_t` the **caller supplies**, not something
embedded in `z_line_t`. One is wanted per user, not per connection:
`repl` serves up to four terminals and sharing one history between
them is both what a single-user machine wants and 3KB cheaper.
Recalling in one window something typed in another is the behaviour,
not a compromise.

`z_line_history_add()` is called by the caller too, because only the
caller knows whether a line was worth remembering — one consumed by a
pager or an editor session rather than executed is not. Consecutive
duplicates are dropped.

An in-progress line is stashed when you first press up, and returned
when you come back down.

## Everything is relative

No absolute cursor positioning is ever emitted, and no assumption is
made about the prompt. `zline` has no idea how wide the caller's
prompt is or which column the input starts in, so every redraw is
"write these characters, erase to end of line, move back N". That
keeps it correct behind a prompt of any width, including one that
changes — which a continuation prompt would.

Every mid-line edit reduces to one operation: rewrite from the edit
point, `ESC[K`, move back.

`Z_LINE_ECHO_MAX` grew from 4 bytes to `Z_LINE_MAX + 32`, because
redrawing after an insert means echoing the rest of the line.

## Echo is batched, not per byte

`repl` accumulates the echo for a whole incoming `Z_PORT_DATA` and
sends it as **one** message.

Per-byte was fine while input arrived at typing speed and broke the
moment paste existed. `z_port_send()` refuses once
`Z_PORT_MAX_PENDING_SENDS` (8) messages are unacked, and the feed loop
never returns to read acks — so a paste of more than eight characters
had its **tail silently dropped from the display** while landing
correctly in the buffer. Paste nine characters, exactly one goes
missing; the measured case is 43 characters arriving as 8.

The batch is flushed in three places, and all three are needed:

- when a line completes, **before** the command runs — its output goes
  down the same port and would otherwise overtake the echo;
- at the end of the byte loop, which is the paste case itself;
- when the buffer would overflow, with a full line's headroom left so
  a batch is never split mid-escape-sequence. Half a sequence arriving
  alone would be drawn as text.

## Testing

```
cd sw/apps/repl && make test
```

The interesting property isn't what ends up in the buffer — that's
easy to get right — but whether the **echo puts the same thing on the
screen**. An editor whose buffer and display disagree is far worse
than one that can't edit at all, because the user is then typing blind
into something that looks correct.

So the suite drives a small simulated terminal (printable characters,
backspace, CR, `ESC[nC`, `ESC[nD`, `ESC[K`) with the echo bytes, and
after every keystroke asserts that the terminal's visible line and
cursor column match the buffer and position exactly. 95 checks,
covering movement, mid-line insert, delete, kill, history including
the stash, escape sequences that must never reach the line, the
full-buffer limit, and all of the multi-line behaviour above.

The echo batching has its own harness (`/tmp` scratch, not checked in)
that models the port's send window and confirms both that a paste now
arrives whole in one message and that the per-byte scheme really did
drop it — a fix whose failure mode you can't reproduce isn't verified.

## Multi-line

Pressing Enter asks the **caller** whether the input is finished
(`z_line_complete_fn`). If it says no, a newline is inserted and
editing continues on a fresh row, with the cursor free to move back up
into what was already typed.

The decision stays with the caller because "finished" depends entirely
on what the text means. `repl`'s rule is balanced parentheses, ignoring
anything inside a string or after a `;` comment — otherwise a paren in
`"a (b"` would keep the reader waiting for one the user never intends
to type. An *excess* of closing parens counts as complete: the form is
broken either way, and letting it through means the reader reports the
problem instead of the terminal silently refusing the line.

Up and down move between **rows** while there is more than one, and
only reach history at the edges. That ordering is what makes
multi-line usable — pressing up to fix a typo two rows above must not
replace the whole form with the previous command. History recall is
refused outright mid-form, since it would discard work that took
several rows to type.

Home, End and Ctrl+K act on the current row; Ctrl+U clears the whole
input.

**Ctrl+C abandons the input** and is not optional: a stray `(` or an
unclosed quote leaves a form permanently incomplete, and without it
there is no way to stop the terminal asking for more.

### The one thing that needs the prompt

Multi-line is the only mode that breaks the everything-is-relative
rule, because repainting across rows means knowing which column the
input starts in. `z_line_set_multiline()` takes the prompt width, and
the continuation prompt **must be the same width** — every row then
starts at the same column. `repl` uses `"> "` and `". "`.

`screen_row` tracks which row the cursor is *physically* on, which is
not the same as the row `pos` falls on. An edit that removes a newline
changes the cursor's logical row before anything is redrawn, while the
terminal's cursor hasn't moved at all — a repaint navigating by the
logical row starts one row too low and leaves the original behind.
That was a real bug, caught by the test suite.

Single-row edits still use the cheap path (rewrite the rest of this
row); a full repaint happens only when the row structure changes.
