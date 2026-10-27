# Lists and emphasis

```lesson
(objective "Write bulleted and numbered lists, and mark words for emphasis, in Markdown.")
(minutes 20)
(keywords "list (in Markdown)" "emphasis (in Markdown)")
```

A **list** is a run of items, one under another. In Markdown, a line
that starts with `-` and a space is an item of a bulleted list (`*` or
`+` and a space work too). `read` draws a dash in front of each item,
and wraps a long item under its own words.

A line right under an item, with no empty line between, joins that
item.

```figure
(define r (wr-view))
(wr-window 20 10 280 190 "jobs.md")
(if r
    (begin
      (text 32 46 "Jobs" 'body 'bold)
      (wr-lines 46 70 '("- buy fresh bread" "- post the letter to Anna" "1. wash" "1. dry")))
    (wr-lines 32 44 '("## Jobs" "" "- buy *fresh* bread" "- post the letter" "to Anna" "" "1. wash" "1. dry")))
(caption (if r "read: dashes, and numbers as typed." "text: the marks as typed."))
```

```check
(choice
  (ask "Which line does read show as a list item?")
  (right "- milk")
  (wrong "-milk"
    "The dash needs a space after it. Without one, the line is a paragraph that starts with a dash.")
  (wrong "milk -"
    "The mark goes at the start of the line, then a space.")
  (hint "Mark first, then a space, then the words."))
```

```sources
(source "Zeitlos docs: read (docs/read_app.md), What renders; sw/apps/read/md.c, lists and emphasis")
```

---

## Numbered lists

A line that starts with a number, a full stop (or `)`) and a space is a
numbered item: `1. wash`. `read` shows the number you typed. It does
not count for you, as some other readers do.

```check
(choice
  (ask "You type three items, each starting 1. and a space. How does read number them?")
  (right "1, 1, 1")
  (wrong "1, 2, 3"
    "Some Markdown readers count for you, but read shows each number as typed. Type 1., 2., 3. for 1, 2, 3.")
  (hint "Look at the figure: wash and dry."))
```

---

## Items under items

Put two spaces before the mark to make an item part of the one above:
`read` indents it one step more. Keep to one step: deeper lists are hard
to read in a window this size.

```check
(choice
  (ask "How do you put the item - salt under the item - shopping?")
  (right "Two spaces, then - salt")
  (wrong "A dash, then two spaces, then salt"
    "Spaces after the dash do not move the item. The two spaces go before the dash.")
  (wrong "Type -- salt"
    "Two dashes are not a deeper item. Indent the item with two spaces.")
  (hint "The step in is made with spaces."))
```

---

## Emphasis

**Emphasis** marks words as stressed: `*word*` or `_word_` for
italic, `**word**` for bold.

`read` has no bold or italic letters. It takes the marks away and shows
the plain words. The marks still count: when you save a Markdown
document in another system's format, such as a Word file, they become
bold and italic there.

```check
(choice
  (ask "How does read show the line `**Do not** forget the keys.`")
  (right "Do not forget the keys.")
  (wrong "`**Do not** forget the keys.`"
    "read takes the marks away, so only the words are left.")
  (wrong "Do not forget the keys., with Do not in bold"
    "read has one weight of letter, so it cannot show bold. It shows the words plainly.")
  (hint "read takes the marks away."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Other systems' formats; convert (docs/convert.md), What a document keeps")
```

---

## Stars that are not marks

A `*` or `_` with a space on both sides is just a character. An `_`
inside a word, as in `file_name`, stays too. But a `*` next to other
characters is taken as a mark, and disappears in `read`.

To show a mark as itself, put `\` in front of it: `2\*3`.

```check
(text
  (ask "How does read show `2*3=6` ?")
  (accept "23=6")
  (wrong-if "2*3=6" "The `*` has characters right after it, so read takes it as the start of emphasis, and takes it away.")
  (hint "Is the `*` next to other characters?")
  (hint "read takes the `*` away."))
```

---

```try
(try (app "text")
  (task "In text, open plan.md (Ctrl+O). Add a heading ## Jobs, an empty line, and three items, each a dash, a space and a job. Save, then open plan.md in read from files, and come back here.")
  (check (choice
    (ask "What did read show in front of each job?")
    (right "A dash, with the job's words lined up after it")
    (wrong "Nothing: the jobs ran together as one paragraph"
      "Then the lines were not taken as items. Check each starts with a dash and a space, and that the empty line is under the heading.")
    (wrong "A round dot"
      "read draws every bullet as a dash, whether you typed `-`, `*` or `+`."))))
```

So: `-` and a space for a bullet, `1.` and a space for a number, and
stars or underscores for emphasis.
