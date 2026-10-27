# Help with F1

```lesson
(objective "Get an app's help with F1, close it, and open the full page of the documentation.")
(minutes 15)
(keywords "help box")
```

This lesson is about finding out what an app does without leaving it.

Press **F1** in an app and a small **help box** opens over it: a line
on what the app is for, its important keys, and how to do the main
thing. It is a screen or two at most.

**Close** puts it away: press **Enter**, **Esc**, or **F1** again. A
longer box scrolls with Up and Down or the wheel. While the box is up,
most apps behind it wait -- a game pauses, a clock's hands stop.

Try it now: press **F1**. learn's own help box opens over this lesson.
Read it, then press Esc.

```sources
(source "Help in apps: F1 (docs/app_help.md)")
(source "sw/apps/learn/learn.c: F1 opens \"learn help\"")
```

```check
(choice
  (ask "A help box is open over an app. Which key does NOT close it?")
  (right "F11")
  (wrong "Esc" "Esc closes the help box, as do Enter and F1.")
  (wrong "Enter" "Enter presses Close, which is chosen when the box opens.")
  (hint "Enter, Esc and F1 all close it."))
```

---

## Read docs

When the reader app and the documentation are on the card, the help
box has a second button beside Close: **Read docs**. It opens the app's
full page of the Zeitlos documentation in read. **Tab** goes between
the two buttons.

The pages linked from these lessons are the same documentation.

```sources
(source "Help in apps: F1 (docs/app_help.md), The box")
```

```check
(choice
  (ask "The help box is too short to answer your question. Where do you look next?")
  (right "Read docs, which opens the app's full page")
  (wrong "Press F1 again for more" "F1 again closes the box. The full page is behind Read docs.")
  (wrong "Nowhere: that is all there is" "The box is the short version. Read docs opens the long one, when it is on the card.")
  (hint "The other button in the box."))
```

---

## Where F1 does something else

F1 is help in almost every app. Two places differ:

- **term**, the terminal, sends F1 on to the program running in it;
  its first panel shows what there is to know.
- Some **games**, while they fill the screen, ignore F1; press F11 to
  put the game back in its window, and F1 works there.

```sources
(source "Help in apps: F1 (docs/app_help.md)")
(source "Function keys (docs/keys.md), term")
```

```check
(choice
  (ask "You press F1 in a game that fills the screen, and nothing happens. What do you try?")
  (right "F11, to put the game back in its window, then F1")
  (wrong "Ctrl+Alt+Del" "That asks to restart the computer. The game is fine: F1 works once it is back in its window.")
  (wrong "Give up: that game has no help" "Games have help too. Some show it only in their window, so press F11 first.")
  (hint "Full screen and the window are toggled by one key."))
```

---

```try
(try (app "files")
  (task "Press Super+2 and open files from the dock. Press F1 and read its help box. Close it with Esc, close files, and press Super+1 to come back here.")
  (check (choice (ask "Which key did files' help give for going up a folder?")
    (right "Backspace")
    (wrong "Esc" "In files Esc stops a find, copy or delete. Up a folder is Backspace.")
    (wrong "F12" "F12 goes back to the folder files started in. Up one folder is Backspace.")
    (hint "The same key that goes back a step here in learn."))))
```

So F1 opens a short help box in almost any app, Enter, Esc or F1
closes it, and Read docs opens the full page.
