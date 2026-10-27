# Code, quotations, rules and tables

```lesson
(objective "Write code, quotations, rules and tables in Markdown, and avoid the marks that surprise.")
(minutes 20)
(keywords "code block" "quotation (in Markdown)" "rule (across the page)" "table (in Markdown)")
```

Words between two backticks (the \` key) are code: a command, a name,
a key. `read` shows them light on dark, and takes no mark inside them
as Markdown, so two stars between backticks show as two stars.

A **code block** is a group of lines shown exactly as typed. Put a line
of three backticks (or three `~`) before it and after it. `read` shows
the lines inside as they are: not joined, not wrapped, with no `#` or
`*` taken as a mark.

```figure
(define r (wr-view))
(wr-window 20 4 280 206 "notes.md")
(if r
    (begin
      (line 26 28 26 40 'ink 1)
      (wr-lines 44 38 '("Less is more."))
      (text 26 62 "Type" 'small)
      (wr-inverse 61 62 "ls")
      (text 82 62 "first." 'small)
      (wr-lines 33 86 '("# not a heading"))
      (line 26 102 294 102 'ink 1)
      (wr-lines 33 124 '("| Day | Job |" "| Mon | Dig |")))
    (wr-lines 26 36 '("> Less is more." "" "Type `ls` first." "" "```" "# not a heading" "```" "" "---" "" "| Day | Job |" "|-----|-----|" "| Mon | Dig |")))
(caption (if r "read: what each mark becomes." "text: the marks as typed."))
```

```check
(choice
  (ask "Inside a code block you type a line # step 1. How does read show it?")
  (right "As # step 1, exactly as typed")
  (wrong "As a heading, step 1"
    "Nothing inside a code block is a mark. read shows the line as typed, # and all.")
  (hint "A code block is shown exactly as typed."))
```

```sources
(source "Zeitlos docs: read (docs/read_app.md), What renders; sw/apps/read/md.c")
```

---

## Four spaces

A line that starts with four spaces is a code block too. And in
`text`, **Tab** puts in four spaces. So do not start a paragraph with
Tab in a Markdown document.

```check
(choice
  (ask "You press Tab at the start of a long paragraph in a .md file. How does read show it?")
  (right "As typed: on one line, not wrapped, cut off at the window's edge")
  (wrong "As a paragraph with its first line indented"
    "Four spaces at the start make a code block, which read never wraps. Markdown has no indented first lines: leave an empty line between paragraphs.")
  (wrong "As usual: the spaces are ignored"
    "Four spaces at the start of a line are a mark: a code block.")
  (hint "Tab is four spaces. What do four spaces at the start mean?"))
```

---

## Quotations and rules

A line that starts with `>` and a space is a **quotation**. `read`
indents it, with a line down its left side.

A line of three or more `-` (or `*`, or `_`) is a **rule**: a line
across the page, to separate parts.

```check
(choice
  (ask "Which line makes a quotation?")
  (right "> Less is more.")
  (wrong "\"Less is more.\""
    "Quotation marks stay as they are, in a paragraph. A quotation block starts with > and a space.")
  (wrong "--- Less is more."
    "A rule is a line of dashes and nothing else. A quotation starts with >.")
  (hint "Look at the first line in the figure."))
```

---

## Leave a line above a rule

A line of `---` right under a line of text does not make a rule: it
turns that text into a level 2 heading. Leave an empty line above the
`---`.

```check
(choice
  (ask "You type Shopping, and on the next line ---. What does read show?")
  (right "Shopping as a level 2 heading, and no rule")
  (wrong "Shopping, then a rule under it"
    "A --- right under one line of text makes that line a heading. Put an empty line between them for a rule.")
  (hint "Is there an empty line between them?"))
```

```note
(note "One more: a file whose very first line is --- hides everything up to the next --- line. Start a document with its title, not a rule.")
```

---

## Tables

A **table** is rows of cells between `|` marks. The line of `|---|`
under the first row is left out by `read`. Every other row is shown
just as you typed it, so line the `|` marks up with spaces yourself:
the letters are all the same width.

```check
(choice
  (ask "Your table's columns look ragged in read. Why?")
  (right "read shows each row as typed: the | marks need lining up with spaces")
  (wrong "read cannot show tables"
    "It can, line by line, as typed. Line up the | marks in text and they line up in read.")
  (wrong "The |---| line is missing"
    "read leaves that line out anyway. The rows are shown as typed, so pad them with spaces.")
  (hint "Look at the rows in the figure: what makes them line up?"))
```

---

```try
(try (app "text")
  (task "In text, open plan.md. Add an empty line, then ---, then an empty line, then a quotation: > and a space and a saying you like. Save, open it in read from files, and come back here.")
  (check (choice
    (ask "What did read draw for the --- line?")
    (right "A line across the page")
    (wrong "It made the line above it a heading"
      "Then there was no empty line above the ---. Add one, save and look again.")
    (wrong "Three dashes"
      "A line of three dashes alone is a rule. Check there is nothing else on the line, not even a letter."))))
```

So: backticks for code, `>` for a quotation, `---` with an empty line
above for a rule, and `|` rows, lined up, for a table.
