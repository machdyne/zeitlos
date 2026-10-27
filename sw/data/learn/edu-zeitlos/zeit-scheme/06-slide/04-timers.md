# Timers

```lesson
(objective "Run Scheme later or again and again with after and every, and stop it with cancel.")
(minutes 20)
(keywords "timer (in slide)")
```

`(after ms forms...)` runs its forms once, ms milliseconds from now;
1000 milliseconds are a second. `(every ms forms...)` runs them again
and again, every ms milliseconds. Each makes a **timer** and answers
with its number.

```
(after 1200 (go 'next))
```

goes to the next card after 1.2 seconds. Meanwhile the deck goes on
answering clicks and keys.

```sources
(source "Zeitlos docs: slide_scripting.md, Short forms")
```

```check
(number
  (ask "How many seconds after the click does `(after 3000 (go 'next))` go on?")
  (answer 3) (from "3000 / 1000")
  (wrong-if 3000 "after counts in milliseconds: 1000 of them are a second.")
  (hint "1000 milliseconds are a second."))
```

---

## Every second

A card's script with `every` keeps something up to date while the card
is shown:

```
(define n 0)
(every 1000
  (set! n (+ n 1))
  (set-text "n" n))
```

```figure
(define n (floor (seconds)))
(rect 70 60 180 80 'paper)
(frame 70 60 180 80 'ink 1.5)
(text 160 116 (fmt n 0) 'centre 40)
(text 160 52 "the text box n" 'centre 'small)
(caption "(every 1000 ...) adds 1 to n each second")
```

The figure counts as that card would. A timer started by a card's
script belongs to the card: leaving the card stops it.

```check
(number
  (ask "A card with that script has been shown for 10 seconds. About what does the box show?")
  (answer 10)
  (wrong-if 1 "every runs its forms again each second, not once: after 10 seconds, about 10 times.")
  (wrong-if 10000 "n goes up by 1 each time, and the timer fires once a second.")
  (hint "One more each second."))
```

---

## cancel

`(cancel timer)` stops a timer. Keep its number in a variable to stop
it later. A stopwatch:

```
; tick, defined elsewhere, adds a tenth of a second and shows it
(define ticking #f)
(when-clicked "start"
  (if (not ticking)
      (set! ticking (every 100 (tick)))))
(when-clicked "stop"
  (if ticking
      (begin (cancel ticking) (set! ticking #f))))
```

ticking is `#f` while the watch is stopped, and the timer's number
while it runs; so start cannot make a second timer, and stop knows
which one to cancel.

```sources
(source "Zeitlos example deck: /data/slide/examples/clock.zsl, the stopwatch")
```

```check
(choice
  (ask "Why does start test `(not ticking)` first?")
  (right "So that a second click on start does not make a second timer")
  (wrong "Because every needs ticking to be #f"
    "every works either way. The test keeps two timers from running at once.")
  (wrong "To stop the watch"
    "Stopping is stop's job, with cancel. start only starts a timer when none runs.")
  (hint "What would two clicks on start do without the test?"))
```

---

## A script that runs too long

A loop that never ends would hold slide up. **Esc** stops a script
that runs too long, and slide goes on as before. A script meant to take
a long time should be cut into steps with `after` or `every`, so the
deck keeps answering.

This is a difference from repl: repl cannot stop a long calculation,
so there, try loops with small numbers first.

```check
(choice
  (ask "A script in slide is stuck in a loop. What do you press?")
  (right "Esc")
  (wrong "F5"
    "F5 starts a show. Esc stops a script that runs too long.")
  (wrong "Nothing: the deck is lost"
    "slide asks every few thousand steps whether to stop, and Esc stops the script.")
  (hint "The key that stops things in most apps."))
```

---

## Practice

```check
(choice
  (ask "Which shows the next card by itself 5 seconds after a card is shown?")
  (right "A card script `(after 5000 (go 'next))`")
  (wrong "The deck's script `(after 5000 (go 'next))`"
    "The deck's script runs once, when the deck starts being used, so this would go on only once. A card's script runs each time its card is shown.")
  (wrong "A card script `(after 5 (go 'next))`"
    "after counts in milliseconds: 5 is a two-hundredth of a second.")
  (hint "Once, after 5000 milliseconds."))
```

So: `after` and `every` run Scheme later without holding the deck up,
`cancel` stops them, and Esc stops a script that runs too long.
