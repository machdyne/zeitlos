# Text

```lesson
(objective "Join, measure and cut strings, and show text with display and print.")
(minutes 15)
(keywords "string (of text)" "output (of a program)")
```

Text in double quotes is a **string**. A string is a value, like a
number, and repl prints it with its quotes:

```
> "Zeitlos"
"Zeitlos"
> (string-append "Zeit" "los")
"Zeitlos"
```

`string-append` joins strings into one. Nothing is added between
them: to get a space, put it inside one of the strings, as in
`(string-append "Hello, " "Ada")`.

```sources
(source "Zeitlos docs: repl.md, Scheme in ten minutes: Text; zapi.md, Strings")
```

```check
(choice
  (ask "What is the value of `(string-append \"good\" \"bye\")`?")
  (right "\"goodbye\"")
  (wrong "\"good bye\""
    "string-append adds nothing between the strings. A space would have to be inside one of them.")
  (wrong "\"byegood\""
    "The strings are joined in the order they are given.")
  (hint "Put the second string straight after the first."))
```

---

## Measuring and cutting

`string-length` counts the characters of a string, spaces included.
`substring` cuts a piece out: its arguments are the string, where to
start and where to stop. The characters are counted from 0, and the
one at the stop is not included:

```
> (string-length "Zeitlos")
7
> (substring "Zeitlos" 0 4)
"Zeit"
```

Z is character 0, e is 1, i is 2, t is 3: four characters, 0 up to 4.

```check
(text
  (ask "What is the value of `(substring \"Zeitlos\" 4 7)`? Type it with its quotes.")
  (accept "\"los\"") (accept "los")
  (wrong-if "tlos" "Counting starts at 0, so character 4 is the l: Z 0, e 1, i 2, t 3, l 4.")
  (wrong-if "los\"" "Type the quotes on both sides, or neither.")
  (hint "Number the characters from 0: Z e i t l o s.")
  (hint "Characters 4, 5 and 6 are l, o and s."))
```

---

## Numbers are not strings

`"12"` is a string of two characters; `12` is a number. A procedure
that wants strings stops at a number:

```
> (string-append "Total: " 12)
repl: Scheme error (see the serial console for details)
> (string-append "Total: " (number->string 12))
"Total: 12"
```

`number->string` turns a number into a string, and `string->number`
turns a string back into a number.

```check
(choice
  (ask "Which expression gives the string \"Age 42\"?")
  (right "`(string-append \"Age \" (number->string 42))`")
  (wrong "`(string-append \"Age \" 42)`"
    "42 is a number, and string-append joins only strings. Turn it into one first with number->string.")
  (wrong "`(+ \"Age \" 42)`"
    "+ adds numbers. It cannot add a string, and it would not join text if it could.")
  (hint "Which one turns 42 into a string first?"))
```

---

## Showing text

What a program shows is its **output**. `display` shows a value as it
is, a string without its quotes. `print` does the same, then starts a
new line.

Both are worked out like any expression, so repl then prints their
value as well. Their value is `#f`, and it comes straight after the
output:

```
> (display "Hello")
Hello#f
> (print "Hello")
Hello
#f
```

The output is the `Hello`; the `#f` is only repl answering, as it does
for every expression.

```sources
(source "Zeitlos docs: zapi.md, Output")
(source "Zeitlos source: sw/apps/repl/repl.c, captured output then the value")
```

```check
(choice
  (ask "What appears after you type `(display \"Hi\")`?")
  (right "Hi#f")
  (wrong "\"Hi\""
    "That is what repl prints for the string itself. display shows the text without quotes, then repl prints display's value, #f.")
  (wrong "Hi, then #f on a line of its own"
    "That is print, which starts a new line after the text. display does not.")
  (hint "display adds no quotes and no new line."))
```

---

## Practice

```check
(number
  (ask "What is the value of `(string-length (string-append \"ab\" \"cde\"))`?")
  (answer 5) (from "2 + 3")
  (wrong-if 2 "string-length measures the joined string, \"abcde\", not the first part.")
  (hint "Join the strings first, then count."))
```

```check
(number
  (ask "What is the value of `(string-length \"a b c\")`?")
  (answer 5)
  (wrong-if 3 "Spaces are characters too: a, space, b, space, c.")
  (hint "Count the spaces as well as the letters."))
```

```check
(choice
  (ask "You want repl to show the words Good morning on a line of their own. Which do you type?")
  (right "`(print \"Good morning\")`")
  (wrong "`(print Good morning)`"
    "Without quotes, Good and morning are taken as names of variables. Text must be in double quotes.")
  (wrong "`(string-length \"Good morning\")`"
    "That counts the characters: repl would print 12.")
  (hint "Text goes in double quotes, and print shows it."))
```

So: strings are text in double quotes, and `display` and `print` show
them as output.
