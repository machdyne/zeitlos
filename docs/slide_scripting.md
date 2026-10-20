# Scripting slide

A deck can do more than its controls' plain actions: it can keep
scores, count, compute, change its cards as it runs and talk to the rest
of Zeitlos. Its language is Scheme -- [Machdyne Scheme](scheme.md), the
same as `repl`'s, with the whole [Zeitlos API](zapi.md) -- plus a few
words of slide's own and short forms for the things a deck does most.

A deck that only uses buttons with `go`, `show`, `speak` and the other
plain verbs ([slide_app.md](slide_app.md#controls-and-actions)) never
starts Scheme at all. The first action or script that needs more starts
it, in a fraction of a second.

```scheme
; the deck's script
(define score 0)
(define (bump n)
  (set! score (+ score n))
  (set-text "score" score))
(when-clicked "plus" (bump 10))

; a button's action
(if (> score 50) (go "winner") (speak "Keep going"))

; a card's script: a clock on the card while it is shown
(every 1000 (set-text "time" (uptime)))
```

## Where scripts live

| | runs | edited with |
|---|---|---|
| **an action** | when its control is clicked (a check after it changes, a slider when let go, a list when an item is chosen, Enter in a one-line field); with nothing selected, the card's: on arriving at the card | `/action ...` (one line), or `/script` with the control selected (in `text`) |
| **a card's script** | on arriving at the card, after its action. Timers it starts stop when the card is left | `/script card` (or `/script` with nothing selected) |
| **the deck's script** | when the deck starts being used: the browse tool, a show, a deck run as an app (F7). Definitions, and `(when-clicked ...)` handlers for any card | `/script deck` |

`/script` writes the script to a file in `/tmp` and opens it in `text`.
Each time it is saved there (Ctrl+S), slide takes it -- after checking
that its parentheses close and its strings end, so a half-written script
never reaches the deck -- and says so in the status line. Undo takes it
back like any other change. A deck's script that changes while the deck
is in use is run again.

**The message box.** A line typed after `/` that starts with `(` is
Scheme, run at once on the card shown, and its value comes back in the
status line: `/(get-text "score")`, `/(go 3)`, `/(set-text "title"
(deck-title))`. What `(display ...)` writes shows there too.

## slide's procedures

| | |
|---|---|
| `(go 'next)` `(go 'prev)` `(go 'first)` `(go 'last)` `(go 'back)` | another card; `back` is the one before, as visited |
| `(go 3)` `(go "results")` | a card by number or by name |
| `(show "name")` `(hide "name")` `(toggle "name")` | an object on the card or its background |
| `(shown? "name")` | whether it is |
| `(get-text "name")` | an object's text: a field's, a label's, a list's items |
| `(set-text "name" value)` | any value as its text: `(set-text "sum" (+ 1 2))` |
| `(get-value "name")` | a check's or radio's `#t`/`#f`, a slider's number, a list's chosen item (`#f` for none) |
| `(set-value "name" n)` | a check (1 or 0), a slider (kept in its range), a list (the item's number from 0; -1 none) |
| `(card-name)` `(card-number)` `(card-count)` `(deck-title)` | where you are |
| `(speak "words")` `(speak-text "name")` | aloud, through the speech service |
| `(open "/data/slide/other.zsl")` | another deck |
| `(beep)` | |

Everything in the [Zeitlos API](zapi.md) is there too: files (`read-file`,
`write-file`, `ls`), the time (`current-date`, `uptime`), processes
(`run`, `(run "view" "/media/images/squirrel.jpg")`), messages, the
network (`tget`), GPIO, I2C and SPI, windows of its own (`win-create`,
`button`, `field`), random numbers -- `help` and `apropos` find them.

## Short forms

These are macros (`define-macro`), so a deck's author never writes a
`lambda`:

| | |
|---|---|
| `(after ms forms...)` | runs the forms once, ms milliseconds from now. Returns the timer's number |
| `(every ms forms...)` | runs them every ms milliseconds |
| `(cancel timer)` | stops one |
| `(when-clicked "name" forms...)` | runs the forms when that control is clicked, on whichever card -- before its own action |

A timer started by a card's script, or by an action on a card, belongs
to that card and stops when it is left; one started by the deck's script
runs as long as the deck is in use. Leaving a show, or going back to
editing, stops them all.

`(wait ms)` is for plain actions (`/action speak Hi; wait 500; go next`):
in Scheme, use `(after 500 (go 'next))`.

## When something goes wrong

An error is said in the status line, with where it came from: "Card
quiz's script: unbound symbol: scroe". The script stops; the deck goes
on. A timer whose forms fail stops.

**Esc stops a script** that runs too long -- a loop that never ends --
and slide goes on as before. Scheme asks every few thousand steps; while
a script runs, slide answers its window (a redraw) but keys and clicks
wait for it to finish. A script that takes a long time on purpose should
be cut into steps with `(after ...)` or `(every ...)`, so the deck stays
responsive ([responsive.md](responsive.md)).

## Examples

`/data/slide/examples` has decks made to be used (each opens as an app;
F7 shows how it is made):

| deck | shows |
|---|---|
| `quiz.zsl` | radios in a group, a score kept in the deck's script, `(after 1200 (go 'next))`, a card's script that shows the result, `(when-clicked "again" ...)` |
| `calculator.zsl` | a field as a display, sixteen buttons whose actions call the deck's procedures |
| `todo.zsl` | a field and a list: items added and removed with string procedures; Enter in the field adds |
| `clock.zsl` | `(every 1000 ...)` on a card, `current-date`; a stopwatch whose timer is kept in a variable and cancelled |

## Memory

Scheme has a heap of 8,000 cells in slide (192 KB, in the app's own
memory, not the deck's arena) and a stack for its strings within slide's
128 KB. That is room for a deck's script of several hundred lines and
its data; a deck that keeps large lists should keep them in a file.

## For the C side

`sw/apps/slide/script.c`: the interpreter (sw/ext/ms, with its
`define-macro`, log hook and step hook), `zapi_register()`
(sw/common/zapi), slide's procedures and the short forms (in Scheme, in
`slide_scm`). Actions that are only the plain verbs are run by `act.c`
without Scheme; anything else goes to `script_eval()`. One script runs at
a time: a script that leads to another (a `(go ...)` to a card with its
own script) has the second run when the first has finished. Host tests:
`make -C sw/apps/slide test` (`tests/test_script.c` drives the example
decks by clicking their buttons).
