# Events

```lesson
(objective "Give a window a handler that answers clicks, keys and redraws.")
(minutes 25)
(keywords "event (in an app)" "handler (of events)")
```

An app waits for things to happen: a click, a key, the window needing
to be drawn again. Each of these is an **event**. `win-on` gives a
window a procedure, its **handler**, which repl calls with each event:

```
(win-on w (lambda (e) (print e)))
```

The event is a list. Its car says what happened:

| Event | When |
|---|---|
| (redraw) | the window must be drawn again |
| (click x y) | a click at x, y in the drawing area |
| (key k) | a key: a character such as #\a, or a name such as up or enter |
| (close) | the window's close box: the window then goes |

With that handler, each event is printed in the terminal that made the
window.

```sources
(source "Zeitlos docs: repl.md, Apps: events, buttons and fields; zapi.md, Windows and apps: win-on")
```

```check
(choice
  (ask "You click the window at x 30, y 12. What does the handler get?")
  (right "(click 30 12)")
  (wrong "(30 12)"
    "The event says what happened as well as where: its car is the symbol click.")
  (wrong "(key click)"
    "A click is its own kind of event, not a key.")
  (hint "Look at the table: the car names the event."))
```

---

## Choosing by the event

`case` picks a branch by a value: the branch whose list holds it. With
`(car e)` it picks by the kind of event:

```
; dots.scm -- click to draw a dot
(define w (win-create "Dots" 160 100))
(win-on w
  (lambda (e)
    (case (car e)
      ((click)
       (let ((x (cadr e)) (y (caddr e)))
         (box w x y (+ x 3) (+ y 3) 1))))))
```

`(cadr e)` is the event's second item, x, and `(caddr e)` its third,
y. Load the file in repl, then click in the window: each click leaves
a dot.

```sources
(source "Zeitlos docs: repl.md, the counter app")
```

```check
(choice
  (ask "In that handler, what happens when you press a key in the window?")
  (right "Nothing: case finds no branch for key")
  (wrong "A dot is drawn where the pointer is"
    "Only the click branch draws, and a key event is not a click.")
  (wrong "A Scheme error"
    "A case with no matching branch simply does nothing.")
  (hint "Which branches does that case have?"))
```

---

## The rule for drawing

Cover the Dots window with another and uncover it: the dots are gone.
Once a window has a handler, the system clears it whenever it must be
drawn again, and sends `(redraw)`. The handler must then draw
everything again, from what the program knows.

So a program keeps what it shows in variables, and draws from them:

```
(define dots '())
(define (show)
  (let loop ((l dots))
    (if (pair? l)
        (let ((x (caar l)) (y (cdar l)))
          (box w x y (+ x 3) (+ y 3) 1)
          (loop (cdr l))))))
```

dots is a list of pairs, x and y. `(caar l)` is the car of the first
pair, its x, and `(cdar l)` its cdr, y. show goes through the list
with a loop rather than `for-each`, so that it works for any number of
dots.

A click does `(set! dots (cons (cons x y) dots))` and draws; a
`(redraw)` calls `(show)`.

```check
(choice
  (ask "Why must a handler answer (redraw)?")
  (right "The window has been cleared, and only the program knows what was in it")
  (wrong "To make the window open"
    "win-create opens it. (redraw) comes later too, whenever the window was covered or moved.")
  (wrong "It need not: the system keeps the drawing"
    "That is true of a window with no handler. A window with one is cleared before (redraw).")
  (hint "What has the system just done to the window?"))
```

---

## Keys

A key event holds a character for a letter or digit, written `#\a`,
`#\7`, and a symbol for the other keys: `up`, `down`, `left`, `right`,
`enter`, `escape`, `backspace`, `f1` to `f12`. Compare a character
with `eqv?`:

```
((key)
 (if (eqv? (cadr e) #\c)
     (begin (set! dots '()) (win-clear w))))
```

This branch, added to the case, makes the c key clear the dots.

```check
(choice
  (ask "Which test is true when the up arrow is pressed?")
  (right "`(eq? (cadr e) 'up)`")
  (wrong "`(eqv? (cadr e) #\\u)`"
    "#\\u is the letter u. The arrow keys come as symbols: up.")
  (wrong "`(eq? (car e) 'up)`"
    "(car e) is the kind of event, key. The key itself is the second item.")
  (hint "The key is (cadr e), and arrows are symbols."))
```

---

## Try it

```try
(try (app "text")
  (task "Write dots.scm from this lesson in text, with the dots list, show, and a case with click, redraw and key branches. Save it as /ram/dots.scm, load it in repl, click in the window, then cover and uncover it. Come back here.")
  (check (choice (ask "After covering and uncovering the window, are the dots there?")
    (right "Yes, because (redraw) calls show")
    (wrong "No, they are gone" "If show is called for (redraw), the dots come back. Check the redraw branch.")
    (hint "What does the redraw branch do?"))))
```

```check
(choice
  (ask "When the close box is clicked, what does the handler get?")
  (right "(close), and then the window goes")
  (wrong "Nothing: the window just goes"
    "A window with a handler gets (close) first, so the program can tidy up.")
  (wrong "(click) on the close box"
    "Clicks on the title bar are not the app's. The close box sends (close).")
  (hint "Look back at the table of events."))
```

So: a handler gets each event as a list, picks by its car with `case`,
and draws everything again on `(redraw)`.
