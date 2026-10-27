# What is on the screen

```lesson
(objective "Name the parts of the Zeitlos screen: the desktop, windows, apps, the dock and its icons.")
(minutes 15)
(keywords "desktop (on the screen)" "window (on the screen)" "app (a program)" "dock" "icon (on the screen)")
```

This lesson names the parts of the screen, so that the rest of the
course can talk about them.

Everything Zeitlos shows on the screen together is the **desktop**.

A program you use -- to write, to draw, to learn -- is an **app**. Each
app shows its work in a **window**: a box on the desktop with the app's
name along its top. This lesson is in the window of the app called
learn.

Windows can overlap, like sheets of paper on a desk. The one on top
hides part of the ones under it.

```figure
(zs-screen)
(zs-win 16 20 150 100 "text" #f #t)
(zs-win 96 50 150 100 "clock" #f #t)
(zs-dock 4 210 6 0)
(text 254 34 "a window" 'small)
(arrow 270 40 236 58 'ink 1)
(text 132 226 "the dock" 'small)
(arrow 128 222 114 222 'ink 1)
(text 190 182 "the desktop:" 'small)
(text 190 196 "all of it" 'small)
```

```sources
(source "Welcome to Zeitlos (docs/welcome.md), The desktop")
```

```check
(choice
  (ask "Two windows overlap. What happens to the one underneath?")
  (right "Part of it is hidden by the one on top")
  (wrong "It closes" "Nothing closes when windows overlap. The window underneath is still there, with part of it covered, like a sheet of paper under another.")
  (wrong "The two join into one window" "Each app keeps its own window. Overlapping windows stay separate; the top one simply covers part of the other.")
  (hint "Think of two sheets of paper on a desk, one partly on top of the other."))
```

---

## The dock

Along the bottom of the desktop is the **dock**: a row of small
pictures, one for each app you can open. A small picture on the screen
that stands for something is an **icon**.

The dock is how you open apps. Its first icon is term, the terminal,
for typing commands; each of the last few opens a set of other apps.

```figure
(zs-screen)
(zs-dock 4 210 12 0)
(arrow 120 180 30 208 'ink 1)
(text 124 178 "one icon for each app" 'small)
```

[The dock](docs:dock) has more about it.

```sources
(source "Welcome to Zeitlos (docs/welcome.md), The desktop")
(source "The dock's files (docs/dock.md); sw/data/wm.cfg, the built-in dock")
```

```check
(choice
  (ask "What is the dock for?")
  (right "Opening apps")
  (wrong "Keeping your saved work" "Saved work is a later unit. The dock holds an icon for each app, to open it.")
  (wrong "Showing the time" "An app can show the time (clock does), but the dock itself is a row of icons for opening apps.")
  (hint "What does each icon on the dock stand for?"))
```

---

## Where is the dock now?

learn has made its window as big as the screen, so that a lesson has
room. **While a window fills the screen, the dock hides.** It is still
there, and it comes back as soon as no window fills the screen.

You will learn two ways to reach the dock without leaving this lesson:
make learn's window smaller again, or go to another desktop where the
dock shows. Both come in the next unit.

```sources
(source "learn (docs/learn.md), Windows and the dock")
(source "Zeitlos Window Manager (docs/window_manager.md), Maximize and shade")
```

```check
(choice
  (ask "You cannot see the dock at the moment. Why not?")
  (right "learn's window fills the screen, and the dock hides while one does")
  (wrong "This computer has no dock" "Every Zeitlos desktop has a dock. It hides while a window fills the whole screen, as learn's does now.")
  (wrong "The dock only appears after learn is closed" "Closing learn is not needed. The dock hides only while a window fills the screen; make the window smaller and it is back.")
  (hint "How much of the screen does learn's window take up?"))
```

---

## Light and dark

The desktop is drawn in a lit shade on a dark one. The lit shade can be
white, amber or green on black, or the desktop can look like dark ink on
paper. You choose which in the settings app, later in this course.

On many Zeitlos computers a few parts -- the frame of the window you
are using, the dock's outline, the background -- are also tinted, in
colours you can choose there too. Some apps, such as games, can show
colours of their own.

```sources
(source "Colour (docs/color.md), Phosphor modes; Desktop colour")
(source "settings (docs/settings_app.md)")
```

```check
(choice
  (ask "A friend's Zeitlos desktop is green on black, and yours is white on black. What is different?")
  (right "Only a setting: each person can choose the shade")
  (wrong "Their computer is broken" "Green on black is one of the shades you can choose, along with white, amber and dark on paper. Nothing is wrong.")
  (wrong "They have a different system" "It is the same Zeitlos. The lit shade is a setting.")
  (hint "The lesson listed the shades you can choose from."))
```

---

```check
(text
  (ask "What is the row of icons along the bottom of the desktop called?")
  (accept "dock") (accept "the dock")
  (hint "It is where apps are opened from.")
  (hint "A short word: d, o, c, k."))
```

```check
(text
  (ask "Each app shows its work in its own ...?")
  (accept "window") (accept "a window")
  (hint "It is a box on the desktop with the app's name along its top."))
```

So the screen is the desktop: windows, each showing an app, over a dock
of icons that open them.
