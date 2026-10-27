# Find and replace

```lesson
(objective "Find words in a document, and replace them all at once or one at a time.")
(minutes 20)
(keywords "match (of a search)")
```

**Ctrl+F** asks what to find. `text` selects the next **match**, the
next place where those characters are, after the caret. At the end of
the document it goes on from the top. If there is no match, it says so.

**F3**, or Ctrl+G, finds the same thing again. F3 before any search
asks, as Ctrl+F does.

```check
(choice
  (ask "You found the first Tuesday in a letter. Which key goes to the next one?")
  (right "F3")
  (wrong "Ctrl+F"
    "Ctrl+F asks again what to find. F3 (or Ctrl+G) goes straight to the next match.")
  (wrong "Enter"
    "Enter types a new line; it does not search. F3 finds the next match.")
  (hint "It is the find key in every app."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Find; Replace; Function keys (docs/keys.md)")
```

---

## Capitals and accents

The letters A to Z match in either case: `paris` finds `Paris` and
`PARIS`. Every other character must be just as you typed it, so an
accented letter only matches itself.

```check
(choice (several)
  (ask "You search for cafe. Which of these does text find?")
  (right "Cafe")
  (right "CAFE")
  (wrong "café"
    "é is not e: an accented letter must match exactly. Search for café to find it.")
  (hint "Case does not matter for A to Z. Accents do."))
```

---

## Replace

**Ctrl+H** replaces. It asks two things: what to find, and what to put
in its place. Leave the second empty to delete every match. Then it
says how many matches there are and asks:

- **Yes**: replace them all, at once;
- **No**: one at a time. Each match is shown and you answer: Yes
  replaces it, No leaves it, Cancel stops;
- **Cancel**: replace nothing.

```check
(choice
  (ask "Your notes have the mark ?? in many places. To remove them all, what do you give Ctrl+H?")
  (right "Find: ??, and Replace with: nothing")
  (wrong "Find: ??, and Replace with: a space"
    "That leaves a space where each ?? was. An empty Replace with leaves nothing.")
  (wrong "Nothing: you have to delete each one with Backspace"
    "Replace can delete: leave Replace with empty, and every match goes.")
  (hint "What should be left where each one was?"))
```

---

## Matches inside words

A match is any place the characters are, even inside a longer word.
Replacing `art` changes `start` too.

```check
(text
  (ask "You replace all of art with craft in: Start the art class. What is the line now?")
  (accept "Stcraft the craft class.")
  (accept "Stcraft the craft class")
  (wrong-if "Start the craft class." "start has art inside it, so it is a match too: it becomes Stcraft.")
  (wrong-if "Start the craft class" "start has art inside it, so it is a match too: it becomes Stcraft.")
  (hint "Look for art inside every word, not only the word art.")
  (hint "Start holds art: S, t, then art."))
```

To be safe, answer **No** and look at each match, or search for `art `
with its space.

---

## Undoing a replace

Replace All is one change: one Ctrl+Z puts every match back. A very
large Replace All warns that it cannot be undone before it starts.

```check
(choice
  (ask "Replace All changed 40 words, and some were wrong. How many times do you press Ctrl+Z to put them all back?")
  (right "Once")
  (wrong "40 times"
    "Replace All is one change, so one undo takes back all 40. (Replacing one at a time is undone one at a time.)")
  (hint "How many changes was Replace All?"))
```

---

```try
(try (app "text")
  (task "In text, type: the cat sat on the mat with a hat. Use Ctrl+H to replace at with og, and answer Yes. Read the line, then come back here.")
  (check (text
    (ask "What does the line say now?")
    (accept "the cog sog on the mog with a hog.")
    (accept "the cog sog on the mog with a hog")
    (wrong-if "the cog sat on the mat with a hat." "Yes replaces every match at once. Each of cat, sat, mat and hat has at in it.")
    (hint "Every at becomes og: c-at, s-at, m-at, h-at."))))
```

So: Ctrl+F and F3 find, Ctrl+H replaces, and a match can be part of a
longer word.
