# Programs in files

```lesson
(objective "Write a program in a file with text, and run it in repl with load.")
(minutes 20)
(keywords "comment (in a program)" "load (a file)")
```

What you type at repl's prompt is gone when repl stops, and one prompt
takes at most 128 characters. A longer piece of work is better kept in
a file.

A Scheme **program** is a file of expressions, mostly definitions.
Write it in `text`, the editor in the dock, and save it with **Ctrl+S**:
for a new file the save dialog asks where. Name it with `.scm` at the
end, for Scheme, for example `/ram/collatz.scm`.

```sources
(source "Zeitlos docs: repl.md, Writing a program in a file; text_editor.md, Using it")
```

```check
(choice
  (ask "Why write a long procedure in a file rather than at repl's prompt?")
  (right "One prompt takes only 128 characters, and a file keeps the program to use again")
  (wrong "repl cannot work out procedures over one line long"
    "repl takes a definition over several lines. The limit is 128 characters at one prompt, and nothing typed there is kept.")
  (wrong "Programs in files run faster"
    "A loaded program runs at the same speed. The file is about length and keeping it.")
  (hint "Think of the limit on one prompt, and of what happens when repl stops."))
```

---

## Comments

In a file you can explain what the program does. A semicolon, `;`,
starts a **comment**: everything after it on its line is for people,
and Scheme passes over it.

```
; collatz.scm -- how many steps from n down to 1
; (an even number is halved, an odd one becomes 3n + 1)
(define (collatz n steps)
  (cond ((= n 1) steps)
        ((even? n) (collatz (/ n 2) (+ steps 1)))
        (else (collatz (+ (* 3 n) 1) (+ steps 1)))))
```

This definition alone is more than 128 characters: it could not be
typed at one prompt.

```sources
(source "Timeless Computing (docs/tc.md), Three Languages Side by Side: The Collatz Sequence")
```

```check
(choice
  (ask "Which line does Scheme pass over completely?")
  (right "`; an even number is halved`")
  (wrong "`(define (collatz n steps)`"
    "That line starts the definition. Only a line after a ; is passed over.")
  (wrong "`(else (collatz (+ (* 3 n) 1) (+ steps 1)))))`"
    "That is part of the cond, and Scheme reads it.")
  (hint "Which line starts with a semicolon?"))
```

---

## load

In repl, **load** runs a file: it reads the file and works out every
expression in it, in order. It answers `#t`, or `#f` if the file is
missing or empty.

```
> (load "/ram/collatz.scm")
#t
> (collatz 27 0)
111
```

The definitions now exist in repl as if you had typed them. Change
the file, save it, and load it again to get the new version.

```sources
(source "Zeitlos docs: zapi.md, Files: load")
```

```check
(choice
  (ask "You fixed a mistake in /ram/collatz.scm and saved it. What makes repl use the new version?")
  (right "Load the file again")
  (wrong "Nothing: repl sees the change at once"
    "repl has the definitions from when it loaded the file. Loading again replaces them.")
  (wrong "Start the computer again"
    "That would empty /ram as well. Loading the file again is enough.")
  (hint "repl took the definitions when the file was loaded."))
```

---

## When a file has a mistake

If an expression in the file stops with an error, `load` stops there
too, and repl says `repl: Scheme error`. The expressions before it have
already been worked out; the ones after it have not. The console says
what went wrong. (A misspelled name alone does not stop it: as at the
prompt, it gives `#f`, and load goes on.)

A file can do things as well as define them. A `(print ...)` in a file
shows its output in the terminal when the file has been loaded.

```check
(choice
  (ask "A file defines a, then has a mistake that stops with a Scheme error, then defines b. After load stops, which exist?")
  (right "a only")
  (wrong "Neither"
    "load works through the file in order, and a was defined before the mistake.")
  (wrong "Both"
    "load stops at the mistake, so the definition of b is never reached.")
  (hint "load works out the expressions in order, until one fails."))
```

---

## Try it

```try
(try (app "text")
  (task "Open text from the dock and type the collatz program above. Save it as /ram/collatz.scm with Ctrl+S. Then in repl, type (load \"/ram/collatz.scm\") and (collatz 6 0). Come back here.")
  (check (number (ask "What is (collatz 6 0)?") (answer 8)
    (hint "6, 3, 10, 5, 16, 8, 4, 2, 1: count the steps."))))
```

```check
(number
  (ask "How many steps does `(collatz 5 0)` count?")
  (answer 5)
  (wrong-if 6 "Count the steps, not the numbers: 5, 16, 8, 4, 2, 1 is six numbers and five steps.")
  (hint "5, 16, 8, 4, 2, 1."))
```

So: a program is a file of expressions, written in `text` with
comments, and `load` runs it in repl.
