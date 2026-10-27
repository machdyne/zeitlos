# Scheme inside slide

```lesson
(objective "Name an object on a card and change it from slide's message box with Scheme.")
(minutes 20)
(keywords "deck (of cards)" "card (of a deck)" "object (on a card)" "status line (in slide)" "message box (in slide)" "text box (on a card)" "script (in slide)")
```

`slide` makes **decks**: stacks of **cards**, each card holding
**objects** such as shapes, buttons and **text boxes**, boxes of words.
A deck can also hold **scripts**: Scheme kept in the deck -- the same
language as repl's, with the same Zeitlos procedures, and some of
slide's own.

Open slide from the dock (or type `(run "slide")` in repl). Along its
foot is the **status line**, where slide says what it has to say.

```sources
(source "Zeitlos docs: slide_app.md, Using it; slide_scripting.md")
```

```check
(choice
  (ask "Which language do slide's scripts use?")
  (right "Scheme, the same as repl's, with the Zeitlos procedures too")
  (wrong "A language of slide's own"
    "slide's scripts are Machdyne Scheme, as in repl. slide adds a few procedures of its own.")
  (wrong "Only plain words like go next"
    "Plain words like go next do simple jobs. Anything more is written in Scheme.")
  (hint "Everything in this course so far works in slide too."))
```

---

## The message box

Press `/` (or F4) to open slide's command line in the status line.
A line that starts with `(` is Scheme: slide works it out at once, on
the card shown, and puts its value in the status line after `=>`.
This is the **message box**.

```
/(+ 1 2)
```

shows `=> 3`. A mistake shows `Error:` and the reason, such as
`unbound symbol: scroe` -- in slide you see why straight away.

```sources
(source "Zeitlos docs: slide_scripting.md, Where scripts live: The message box")
(source "Zeitlos source: sw/apps/slide/slide.c, run_command(): => and Error:")
```

```check
(choice
  (ask "You type `/(* 6 7)` and press Enter in slide. What does the status line show?")
  (right "=> 42")
  (wrong "42#f"
    "That mix of output and value is repl's way. slide's message box shows the value after =>.")
  (wrong "Nothing: slide has no Scheme until a deck has a script"
    "The message box starts Scheme when it is first needed, in a fraction of a second.")
  (hint "slide puts the value after =>."))
```

---

## Naming an object

Scheme finds an object on the card by its name. Draw a text box with
the text tool (press `t`, then drag on the card), type a word into it,
and press Escape. With the box still chosen, give it a name with the
command line: `/name score`.

Now the message box can reach it:

```
/(set-text "score" 10)
/(get-text "score")
```

The first puts 10 in the box; `set-text` takes any value and shows it
as text. The second shows `=> "10"`: the text is a string.

```sources
(source "Zeitlos docs: slide_app.md, Using it (name NAME); slide_scripting.md, slide's procedures")
```

```check
(choice
  (ask "What does `/(get-text \"score\")` give when the box shows 10?")
  (right "The string \"10\"")
  (wrong "The number 10"
    "An object's text is a string. string->number turns it into a number.")
  (wrong "#f"
    "get-text gives #f only when no object on the card has that name. Here one has.")
  (hint "Text on a card is text."))
```

---

## Adding one

To add 1 to the score, read the text, turn it into a number, add, and
set it again:

```
/(set-text "score" (+ 1 (string->number (get-text "score"))))
```

Each time you run that line, the box counts up. With a name that no
object on the card has, `get-text` and `set-text` answer `#f`.

```check
(number
  (ask "The box shows 10. You run that line three times. What does it show?")
  (answer 13) (from "10 + 3")
  (wrong-if 11 "Each run adds 1 to what the box shows then: 11, 12, 13.")
  (hint "One more each time."))
```

---

## Practice

```check
(choice
  (ask "Which shows the deck's title in a text box named `t`?")
  (right "`/(set-text \"t\" (deck-title))`")
  (wrong "`/(set-text t (deck-title))`"
    "The object's name is a string, in quotes. t without quotes is a variable.")
  (wrong "`/(get-text \"t\")`"
    "get-text reads the box. set-text changes it.")
  (hint "Which procedure changes an object's text?"))
```

```check
(choice
  (ask "You type `/(set-text \"Score\" 5)` but the box is named score. What happens?")
  (right "Nothing changes: no object is called Score, so set-text answers #f")
  (wrong "The box shows 5"
    "Names count capitals: Score and score are different names.")
  (wrong "slide makes a new box called Score"
    "set-text changes an object that is there. It does not make one.")
  (hint "Do capitals count in a name?"))
```

So: slide runs the same Scheme, the message box shows a value at once,
and named objects are reached with `get-text` and `set-text`.
