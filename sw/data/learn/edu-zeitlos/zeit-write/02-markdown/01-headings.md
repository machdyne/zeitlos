# Headings and paragraphs

```lesson
(objective "Write a Markdown document with headings and paragraphs, and see it in read.")
(minutes 20)
(keywords "Markdown" "heading (in Markdown)")
```

**Markdown** is plain text with a few marks in it, such as `#` and
`-`. A reader shows each mark as the look it stands for. You write
Markdown in `text`, in a file whose name ends in `.md`, and read it in
`read`.

Double-click a `.md` file in `files` and it opens in `read`, which
shows it with the marks turned into their look. To change it, open it
in `text` with Ctrl+O.

```check
(choice
  (ask "You double-click plan.md in files. Which app opens it?")
  (right "read")
  (wrong "text"
    "files opens .md files in read, since a Markdown file is read more often than changed. Open it from text with Ctrl+O to edit it.")
  (hint "The ending .md is for Markdown, to be read."))
```

```sources
(source "Zeitlos docs: read (docs/read_app.md), What renders; files (docs/file_browser.md), Opening a file")
```

---

## Headings

A **heading** is a title over a part of a document. In Markdown a line
that starts with `#` and a space is a heading. `#` is the top level,
`##` the next, and so on to `######`.

In `read`, levels 1 and 2 are in larger letters, and level 1 has a
line under it. Levels 3 to 6 are in the usual letters, with a little
space above them.

```figure
(define r (wr-view))
(wr-window 20 10 280 190 "plan.md")
(if r
    (begin
      (text 32 46 "Garden plan" 'body 'bold)
      (line 32 52 288 52 'ink 1)
      (wr-lines 32 72 '("Beans go in the sunny bed."))
      (text 32 102 "Week 1" 'body 'bold)
      (wr-lines 32 122 '("Dig the beds. Water them.")))
    (wr-lines 32 44 '("# Garden plan" "" "Beans go in the" "sunny bed." "" "## Week 1" "" "Dig the beds." "Water them.")))
(caption (if r "read: the marks are gone." "text: the marks as typed."))
```

```check
(text
  (ask "Write the line for a level 2 heading that says Plans.")
  (accept "## Plans")
  (wrong-if "##Plans" "There must be a space after the marks. Without it, read shows the line as ordinary text: ##Plans.")
  (wrong-if "# Plans" "One # is level 1. Level 2 has two.")
  (hint "Two #, then a space, then the words."))
```

---

## Levels

Use level 1 once, for the document's title, and level 2 for its main
parts. A part of a part is level 3.

```check
(number
  (ask "How many # marks start a heading one level below ## ?")
  (answer 3)
  (wrong-if 1 "One # is the top level, above ##. Each level down adds one #.")
  (hint "Each level down adds one #."))
```

---

## Paragraphs

In `read`, lines that come one after another join into one paragraph:
look again at "Beans go in the" and "sunny bed." in the figure above. A
new paragraph starts after an empty line.

So in `text`, press Enter twice between paragraphs: once to end the
line, once for the empty line.

```check
(choice
  (ask "In text you type Hello. then Enter, then How are you? How does read show it?")
  (right "One paragraph: Hello. How are you?")
  (wrong "Two paragraphs"
    "read joins lines that follow one another. Leave an empty line between them for two paragraphs.")
  (wrong "Two lines, one under the other"
    "Markdown joins them into one paragraph, wrapped to the window. An empty line starts a new one.")
  (hint "What separates paragraphs in Markdown?"))
```

---

## A heading needs its space

```example
(example
  (problem "Start a recipe with a title and a part called Steps.")
  (step "The title is level 1: # Pancakes")
  (step "An empty line, then a paragraph: what the recipe makes.")
  (step "An empty line, then the part's heading.")
  (step (choice (ask "Which line makes the part's heading?")
    (right "## Steps")
    (wrong "##Steps" "Without a space after ##, it is not a heading: read shows ##Steps as text.")
    (wrong "Steps" "Plain words are a paragraph. A heading starts with # marks and a space.")))
  (answer "# Pancakes, a paragraph, then ## Steps, each with an empty line between."))
```

---

```try
(try (app "text")
  (task "In text, write a line # My plan, an empty line, then two short paragraphs with an empty line between them. Save it in /home as plan.md. Then open files, go to /home and double-click plan.md. Come back here.")
  (check (choice
    (ask "How did read show the line # My plan?")
    (right "As a title in larger letters, with a line under it, and no #")
    (wrong "As # My plan, in ordinary letters"
      "Then read did not take it as a heading. Check that there is a space after the # and nothing before it.")
    (wrong "It did not show it"
      "Check that the line is the first line of the file and starts with # and a space."))))
```

So: `#` and a space make a heading, and an empty line separates
paragraphs.
