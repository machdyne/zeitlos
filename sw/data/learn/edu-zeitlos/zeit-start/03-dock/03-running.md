# Running and closing apps

```lesson
(objective "Read the running dots on the dock, find an app's window, and close apps you no longer need.")
(minutes 15)
(keywords "running dot")
```

This lesson is about knowing which apps are open, and closing them.

An app that is open is said to be **running**. Under each icon on the
dock is one small **running dot** for each copy of that app that is
running, up to six. Under a group's icon, the dots of all its apps are
added up.

Move the slider to open more copies of text.

```figure
(define n (slider 'copies 0 8 1 "Copies" 1))
(zs-dock-row 40 90 4 0 (list 0 0 n 0))
(text 103 150 "text" 'small 'centre)
(arrow 103 140 103 118 'ink 1)
(caption (string-append (fmt n 0) " running: " (fmt (min n 6) 0) (if (= (min n 6) 1) " dot" " dots")))
```

```sources
(source "The dock's files (docs/dock.md), Running apps")
```

```check
(number
  (ask "Seven copies of text are running. How many dots are under its icon?")
  (answer 6)
  (true "6 < 7")
  (wrong-if 7 "The dots stop at six, so seven copies still show six. Move the slider in the figure to see.")
  (hint "There is a most the dock shows."))
```

---

## Where is its window?

The dots count every copy running, on every workspace. So if there is
a dot under an icon but no window of that app on the screen, its window
is on another workspace. Press **Super+]** to step through the
workspaces until you find it.

```sources
(source "The dock's files (docs/dock.md), Running apps: a copy on another workspace")
```

```check
(choice
  (ask "There is a dot under clock's icon, but you see no clock on the screen. Where is it most likely to be?")
  (right "On another workspace")
  (wrong "Nowhere: the dot is wrong" "The dot counts a clock that is running. Its window is on another workspace; step through them to find it.")
  (wrong "Inside the dock" "Apps do not run inside the dock. The dot means clock is running somewhere, and that is another workspace.")
  (hint "The dots count copies on every workspace."))
```

---

## Another copy

Clicking the icon of an app that is already running opens another copy
of it, with a window of its own: two text windows, for two letters at
once. Some apps, such as the on-screen keyboard, run only once.

Every app that is running uses some of the computer's memory, and a
Zeitlos computer has only so much of it. So close the apps you have finished with.

```sources
(source "The dock's files (docs/dock.md), Running apps")
(source "files (docs/file_browser.md), Known limits: each open starts a fresh process")
(source "Zeitlos Window Manager (docs/window_manager.md), Keyboard layout key: Super+K, the on-screen keyboard is single-instance")
```

```check
(choice
  (ask "text is running. You click its icon on the dock again. What happens?")
  (right "A second copy of text opens, in a new window")
  (wrong "The text window you had comes to the front" "The dock starts the app again: a second copy, with its own window. To reach the first, click its window or use Alt+Tab.")
  (wrong "text closes" "The dock only opens apps. A second click on the icon opens another copy.")
  (hint "What does one click on a dock icon always do?"))
```

---

## Closing

Click a window's close box to close it, and its app stops running: its
dot goes from the dock.

```check
(number
  (ask "Three copies of text are running. You close one of their windows. How many dots are under text's icon now?")
  (answer 2) (from "3 - 1")
  (wrong-if 3 "Closing the window stops that copy, so its dot goes.")
  (hint "One copy fewer."))
```

---

```check
(choice
  (ask "Why close apps you have finished with?")
  (right "Every running app uses some of the computer's memory")
  (wrong "Open apps wear out the screen" "The screen does not wear out. Running apps use memory, which other apps may need.")
  (wrong "An app left open loses your work" "An app left open keeps its work. Closing it frees the memory it uses.")
  (hint "What does every running app use?"))
```

So a dot under an icon is a copy running, on any workspace; a click on
the icon starts another, and the close box stops one.
