# Buttons and fields

```lesson
(objective "Put buttons and text fields in a window and answer them in its handler.")
(minutes 25)
(keywords "control (in a window)" "button (in a window)" "text field")
```

The system can draw and work two kinds of **control**, a part of a
window that you work with the mouse or the keys. A
**button** is pressed with a click, or with Enter or Space when it has
the focus. A **text field** is a box to type a line of text into.

```
(button w x y width height "Label")  ; its number
(field w x y width)                   ; its number
```

Each answers with a number, which events use to say which one it was:

| Event | When |
|---|---|
| (button n) | button n was pressed |
| (enter n) | Enter was pressed in field n |

Tab moves from one control to the next. The system draws the controls
again itself after each `(redraw)`.

```sources
(source "Zeitlos docs: repl.md, Apps: events, buttons and fields; zapi.md, Windows and apps")
```

```check
(choice
  (ask "A window has a button whose number is kept in `plus`. Which test is true when it is pressed?")
  (right "`(and (eq? (car e) 'button) (= (cadr e) plus))`")
  (wrong "`(eq? (car e) 'plus)`"
    "The event is (button n): its car is the symbol button, and n is the number kept in plus.")
  (wrong "`(= (car e) plus)`"
    "(car e) is the kind of event, a symbol. The button's number is the second item.")
  (hint "The event is a list: button, then the number."))
```

---

## A counter

This app counts presses of its button:

```
(define w (win-create "Counter" 120 64))
(define count 0)
(define plus (button w 4 30 50 16 "+1"))

(define (show)
  (box w 0 0 115 25 0)
  (text w 4 4 (number->string count) 1))

(win-on w
  (lambda (e)
    (case (car e)
      ((redraw) (show))
      ((button) (set! count (+ count 1)) (show)))))
```

show first rubs out the top of the window with colour 0, then writes
the count. A press adds 1 to count and shows it.

```check
(choice
  (ask "Why does show draw a box in colour 0 before the text?")
  (right "To rub out the old number, so the new one is not written over it")
  (wrong "To draw a frame round the number"
    "Colour 0 is the background colour: the box rubs out what was there.")
  (wrong "The system needs a box before text"
    "text can be drawn anywhere. The box is there to clear the old count.")
  (hint "What colour is 0?"))
```

---

## Fields

`(field-text w n)` is the text in field n, as a string, and
`(field-set! w n "text")` changes it. In a new window w of the same
size, this handler greets whoever types a name and presses Enter. It is
longer than one prompt takes, so put it in a file and load it:

```
(define name-box (field w 4 30 100))
(win-on w
  (lambda (e)
    (case (car e)
      ((enter)
       (box w 0 0 115 25 0)
       (text w 4 4 (string-append "Hello, "
                     (field-text w name-box)) 1)
       (field-set! w name-box "")))))
```

The field is emptied after each name, ready for the next. Fields take
plain text only: letters without accents, digits and the usual signs.

```check
(choice
  (ask "A field holds a number someone typed, \"42\". How do you get the number 42 to add to?")
  (right "`(string->number (field-text w n))`")
  (wrong "`(field-text w n)`"
    "That is the string \"42\". + needs a number, so turn it into one first.")
  (wrong "`(number->string (field-text w n))`"
    "That turns a number into a string. Here it is the other way round.")
  (hint "field-text gives a string."))
```

---

## A whole app

Zeitlos has a complete app in Scheme to read and change: a to-do
list. Load it in repl:

```
> (load "/data/repl/examples/todo.scm")
#t
```

Type a task and press Enter, click tasks to tick them, and press Clear
to take away the ticked ones. Open the file in `text` to read how it
works; to change it, save a copy in `/ram` and load that.

```try
(try (app "repl")
  (task "In repl, load /data/repl/examples/todo.scm. Add three tasks, tick one, and press Clear. Then open the file in text and find the case in its handler. Come back here.")
  (check (choice (ask "Which events does the to-do app's handler answer?")
    (right "redraw, enter, button, click and close")
    (wrong "Only button" "It answers Enter in its field and clicks on the list as well, and draws on redraw.")
    (wrong "Only click and key" "It has no key branch: typing goes into the field, which the system works. Look at the case again.")
    (hint "Look at the branches of the case in todo.scm."))))
```

```check
(choice
  (ask "In the to-do app, how is a click on a task turned into which task it was?")
  (right "From the click's y: each task is 12 pixels tall, starting at y 26")
  (wrong "Each task is a button with its own number"
    "The tasks are drawn with text, not made as buttons. The app works out which from where you clicked.")
  (wrong "The system tells the handler the task's text"
    "A click event holds only x and y. The app works out the rest.")
  (hint "The click branch uses (caddr e), the y."))
```

So: `button` and `field` make controls, and the handler answers their
`(button n)` and `(enter n)` events.
