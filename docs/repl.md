# Programming in Scheme

Zeitlos comes with a programming language, Scheme, in the `repl` app.
It is in flash, so it works with no SD card. This page is enough to
start writing programs, and then apps with windows and buttons.

## Starting

Open a terminal (`term`, in the dock) and press **REPL**. You get a
prompt:

```
> (+ 1 2)
3
```

Type an expression and press Enter; repl works it out and prints the
answer. `help` lists the commands repl has besides Scheme.

## Scheme in ten minutes

Everything is an expression in brackets: the first word is what to do,
the rest is what to do it to.

```
> (* 6 7)
42
> (+ 1 (* 2 3))
7
> (/ 1 3)
0.333333
```

**Names.** `define` gives a value a name:

```
> (define price 12)
> (* price 3)
36
```

**Text** is in double quotes. `display` shows it; `print` shows it and
starts a new line:

```
> (print "Hello")
Hello
> (string-append "Zeit" "los")
"Zeitlos"
```

**Functions.** `define` with a name and arguments in brackets:

```
> (define (square x) (* x x))
> (square 9)
81
```

**Choices.** `if` takes a test, a value for true and one for false.
`#t` is true and `#f` is false:

```
> (if (> 5 3) "bigger" "smaller")
"bigger"
```

`cond` checks several tests in turn:

```
(define (size n)
  (cond ((< n 10) "small")
        ((< n 100) "medium")
        (else "large")))
```

**Lists** hold several values. `car` is the first, `cdr` the rest:

```
> (define days (list "mon" "tue" "wed"))
> (car days)
"mon"
> (length days)
3
```

**Repeating.** A function can call itself. This counts down:

```
(define (countdown n)
  (if (> n 0)
      (begin (print n) (countdown (- n 1)))))
```

`for-each` runs a function on every item of a list:

```
> (for-each print days)
```

**Local names.** `let` names values for one expression:

```
(let ((w 10) (h 4)) (* w h))
```

A semicolon starts a comment, to the end of the line.

## Writing a program in a file

Long programs are easier to write in the text editor (`text`, in the
dock). Save the file, then load it in repl, which runs everything in
it:

```
> (load "/ram/hello.scm")
```

Where to save: `/ram` is memory, there with or without a card but
gone when the power is; `/usb` is a USB stick; with an SD card,
`/home` is yours.

## Windows and drawing

`win-create` opens a window and returns its number:

```
(define w (win-create "Drawing" 200 120))
(line w 0 0 199 119 1)
(box w 20 20 60 50 1)
(text w 70 30 "Hello" 1)
```

Coordinates count from the top left of the window, in pixels. The last
number is the colour: 1 white, 0 black. `(box w x0 y0 x1 y1 c)` fills
a rectangle; `(win-clear w)` blanks the window.

## Apps: events, buttons and fields

An app tells the window what to do when something happens. `win-on`
gives the window a function, the **handler**, that is called with each
event:

| Event | When |
|---|---|
| `(redraw)` | the window needs drawing again (it was covered, moved, opened) |
| `(key k)` | a key: a character like `#\a`, or `up` `down` `left` `right` `enter` `escape` `tab` `backspace` `delete` `home` `end` `page-up` `page-down` `f1` ... `f12` |
| `(click x y)` | the mouse was clicked at `x`, `y` |
| `(button n)` | button `n` was pressed |
| `(enter n)` | Enter was pressed in text field `n` |
| `(close)` | the window's close box; the window goes after this |

Buttons and text fields are drawn and worked by the system:

```
(button w x y width height "Label")   ; returns the button's number
(field w x y width)                   ; returns the field's number
(field-text w n)                      ; the text in field n
(field-set! w n "text")               ; change it
```

Tab moves between them; Enter or Space presses a button. Fields take
plain ASCII text.

**The rule for drawing:** draw your window's content in the `(redraw)`
event, from what your program knows. The system clears the window
first and draws the buttons and fields after.

A counter app:

```
(define w (win-create "Counter" 120 60))
(define count 0)
(define plus (button w 4 30 50 16 "+1"))

(define (show)
  (box w 0 0 119 25 0)
  (text w 4 4 (number->string count) 1))

(win-on w
  (lambda (e)
    (case (car e)
      ((redraw) (show))
      ((button) (set! count (+ count 1)) (show)))))
```

`case` picks the branch for the kind of event, `(car e)`; `(cadr e)`
is the event's first value, such as the button's number.

## The to-do example

A complete app, in flash:

```
> (load "/data/repl/examples/todo.scm")
```

Type a task, press Enter, click tasks to tick them, and press Clear.
Open `/data/repl/examples/todo.scm` in `text` to read how it works, or copy it to
`/ram` and change it.

## More to try

| | |
|---|---|
| `(ls "/ram")` | the files in a directory |
| `(read-file "/ram/a.txt")` | a file's text |
| `(write-file "/ram/a.txt" "hi")` | write one |
| `(current-date)` | year, month, day, hour, minute, second |
| `(random 6)` | a random number from 0 to 5 |
| `(delay-ms 500)` | wait half a second |
| `(led #t)` | the board's LED on |
| `(ps)`, `(run "text")` | processes; start an app |

The full list is [scheme_api.md](scheme_api.md); how repl is built is
[scheme.md](scheme.md).
