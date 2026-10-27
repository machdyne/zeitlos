# Finding files by name

```lesson
(objective "Find files by part of their name, or by a pattern, with files.")
(minutes 15)
(keywords "wildcard")
```

In `files`, press **F3**, **Ctrl+F** or the **Find** button, type what
to look for, and press Enter. `files` looks in the folder it is showing
and in every folder inside it.

The results take the place of the list while they are up. The line
above them says how many were found.

```check
(choice
  (ask "files shows /home/letters, and you search for plan. Is /home/plan.md looked at?")
  (right "No: only /home/letters and the folders inside it")
  (wrong "Yes: Find looks everywhere"
    "Find looks in the folder shown and below it. Go up to /home, or to /, to look wider.")
  (hint "Find starts in the folder it is showing."))
```

```sources
(source "Zeitlos docs: files (docs/file_browser.md), Find; sw/common/zwalk.c, zw_name_match()")
```

---

## Part of a name

A plain word finds every name with those letters in it, in capitals or
small letters: `report` finds `report.md`, `Old-Report.txt` and a
folder `reports`.

```check
(choice (several)
  (ask "You search for list. Which of these names are found?")
  (right "shopping-list.txt")
  (right "LIST.MD")
  (right "lists (a folder)")
  (wrong "lst.txt"
    "lst has no i: the letters l, i, s, t must all be there, one after another.")
  (hint "Is l-i-s-t in the name, in any case?"))
```

---

## Wildcards

A **wildcard** is a character that stands for others in a pattern:

- `*` stands for any run of characters, or none;
- `?` stands for exactly one character.

With a wildcard in it, the pattern must fit the whole name: `*.md`
finds the names that end in `.md`. Choose a pattern under the figure.

```figure
(define k (choice 'pat '("report" "*.md" "wk?.txt") "Find"))
(define names '("report.md" "Old-Report.txt" "reports" "wk1.txt" "wk2.txt" "wk10.txt" "notes.md"))
(define hits (list-ref '((#t #t #t #f #f #f #f) (#t #f #f #f #f #f #t) (#f #f #f #t #t #f #f)) k))
(wr-window 60 6 200 128 "files")
(let loop ((n names) (h hits) (y 36))
  (if (pair? n)
      (begin
        (if (car h) (wr-inverse 72 y (car n)) (text 72 y (car n) 'small))
        (loop (cdr n) (cdr h) (+ y 14)))))
(text 196 64 "(folder)" 'small)
(caption "Found names are shown light on dark.")
```

```check
(text
  (ask "Which pattern finds week1.txt and week2.txt, but not week10.txt?")
  (accept "week?.txt")
  (accept "week?.*")
  (wrong-if "week*.txt" "`*` stands for any run of characters, so it also fits the 10 of week10.txt. `?` stands for exactly one.")
  (wrong-if "week*" "`*` fits any run of characters, so week10.txt fits too. Use `?` for exactly one.")
  (hint "You want exactly one character after week.")
  (hint "week, then the wildcard for one character, then .txt"))
```

---

## The results

**Enter**, or a double-click, opens a result as the list would: a
folder is gone into, a file opens in its app. **Esc** stops a find that
is still running and keeps what it found; Esc again goes back to the
folder. F3 or Ctrl+F starts a new find.

```check
(choice
  (ask "A find over all of / is taking long, and the file you want is already in the results. What do you press?")
  (right "Esc, to stop the find and keep the results")
  (wrong "Esc twice, quickly"
    "The second Esc takes the results away and goes back to the folder. Press it once.")
  (wrong "F12"
    "F12 goes back to the folder files started in, putting the results away.")
  (hint "One key stops a find and keeps what it found."))
```

---

```try
(try (app "files")
  (task "Open files from the dock and go into /home. Press F3, type `*.md` and press Enter. Look at the results, press Esc twice, and come back here.")
  (check (choice
    (ask "Was plan.md, from the Markdown lessons, among the results?")
    (right "Yes")
    (wrong "No"
      "Check that files was showing /home when you pressed F3, and that the pattern was `*.md`. If you saved plan.md somewhere else, look there."))))
```

So: Find looks in the folder shown and below it, by part of a name or
by a pattern with `*` and `?`.
