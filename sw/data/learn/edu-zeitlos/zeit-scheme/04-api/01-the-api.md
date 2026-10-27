# The Zeitlos procedures

```lesson
(objective "Find a procedure and how to call it with help and apropos, and call procedures as bare commands.")
(minutes 15)
(keywords "API (application programming interface)" "bare command")
```

Besides the language, repl has procedures that reach the machine:
files, windows, the time, other apps, the network, the hardware. A set
of procedures that programs use to reach a system is an **API**, an
application programming interface. Zeitlos's has more than 80.

Every procedure repl has, Scheme's own and Zeitlos's, is in its
reference. `help` with a name shows one entry: how to call it, what it
does, and an example.

```
> help substring
(substring "text" start end)
  The characters from start up to end (not included), from 0.
  > (substring "Zeitlos" 0 4)  =>  "Zeit"
#t
```

In the reference, `=>` points to what an example gives.

```sources
(source "Zeitlos docs: repl.md, Starting: Looking things up; scheme_api.md, help and apropos")
```

```check
(choice
  (ask "What does `help string-length` show?")
  (right "How to call string-length, what it does, and an example")
  (wrong "The length of the word string-length"
    "help is a command of its own here: it looks the name up in the reference.")
  (wrong "Every procedure repl has"
    "help with a name shows that one entry. To search, use apropos.")
  (hint "help with a name looks that name up."))
```

---

## apropos

When you do not know the name, `apropos` finds every procedure whose
name or description has a word in it, one line each, and answers with
how many it found:

```
> apropos window
win-create       Opens a window and returns its number, or #f if it...
...
```

`page /docs/zapi.txt` reads the whole reference, a screen at a time;
`q` leaves it. The same list is on the docs page [zapi](docs:zapi).

```check
(choice
  (ask "You want a procedure that deletes a file, but do not know its name. What do you type?")
  (right "apropos delete")
  (wrong "help delete"
    "help needs the exact name of a procedure. apropos searches for a word.")
  (wrong "(delete)"
    "There is no procedure called delete; repl would answer #f. Search first.")
  (hint "Which of the two searches by a word?"))
```

---

## Bare commands

`help substring` has no brackets and no quotes, yet it works. When the
first word of a line is the name of a procedure, repl turns the line
into a call for you: `help substring` becomes `(help "substring")`.
Such a line is a **bare command**.

Each word after the first becomes a number if it is one, `#t` or `#f`
if it is one of those, and a string otherwise. A word that starts with
`(` or `"` is kept as it is.

```
> random 6
4
```

is `(random 6)`: a whole number from 0 to 5, chosen afresh each
time.

```sources
(source "Zeitlos docs: scheme_api.md, How a line reaches Scheme")
(source "Zeitlos docs: zapi.md, Random numbers")
```

```check
(choice
  (ask "Which call does the bare command `ls /ram` become?")
  (right "`(ls \"/ram\")`")
  (wrong "`(ls /ram)`"
    "A bare word that is not a number becomes a string, in quotes. /ram alone would be taken as the name of a variable, which has no value.")
  (wrong "`ls /ram` stays as it is and gives #f"
    "ls is the name of a procedure, so repl turns the line into a call.")
  (hint "Words that are not numbers become strings."))
```

---

## When to write brackets

A bare word is always a number, a truth value or a string, never a
variable. With `(define n 6)`, the bare command `random n` passes the
string `"n"`, not 6. Write the real call: `(random n)`.

Bare commands are for quick commands at the prompt; programs use
brackets.

```check
(choice
  (ask "You have `(define f \"/ram/a.txt\")`. Which shows the size of that file?")
  (right "`(file-size f)`")
  (wrong "`file-size f`"
    "As a bare command, f becomes the string \"f\": the size of a file called f.")
  (wrong "`file-size \"f\"`"
    "That is the file named f too. The variable's value is only used in a real call.")
  (hint "A bare word never stands for a variable."))
```

---

## Try it

```try
(try (app "repl")
  (task "In repl, type help random and read its entry, then type random 6 a few times. Come back here.")
  (check (choice (ask "Which number did random 6 never give?")
    (right "6")
    (wrong "0" "0 can come up: (random n) gives a whole number from 0 to n - 1.")
    (wrong "5" "5 can come up: it is n - 1.")
    (hint "From 0 up to n - 1."))))
```

```check
(number
  (ask "How many different values can `(random 10)` give?")
  (answer 10)
  (wrong-if 9 "From 0 to 9 is ten values: count 0 as well.")
  (wrong-if 11 "From 0 to 9: 10 itself is never given.")
  (hint "0 up to 9."))
```

So: `help` and `apropos` find the procedures of the Zeitlos API, and
a line whose first word is a procedure is a bare command.
