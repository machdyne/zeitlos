# Groups on the dock

```lesson
(objective "Open a group on the dock, start an app from it, and go back to the row.")
(minutes 15)
(keywords "group (on the dock)")
```

This lesson is about the last icons on the dock.

The dock is one row and has room for only so many icons, so some apps
are kept in **groups**. A group is one icon on the dock -- a box with a
thick bar across its top, so it never looks like an app -- that holds a
set of apps.

```figure
(zs-dock-row 10 100 6 4 '())
(text 196 160 "4 groups" 'small 'centre)
(arrow 196 150 196 128 'ink 1)
(text 60 160 "apps" 'small 'centre)
(arrow 60 150 60 128 'ink 1)
(caption "A group's icon has a thick bar across its top.")
```

The groups on a new computer are:

| Group | Holds |
|---|---|
| Desk | calc, clock, cal (a calendar) |
| Sound | apps that play music and sound |
| Tools | settings, backup, info and others |
| Games | chess, snake, blocks and more |

A computer with no sound has no Sound group.

```sources
(source "The dock's files (docs/dock.md), Groups")
(source "sw/data/wm.cfg: the built-in groups")
```

```check
(choice
  (ask "How can you tell a group's icon from an app's icon on the dock?")
  (right "A group's icon has a thick bar across its top")
  (wrong "A group's icon is bigger" "Every icon on the dock is the same size. A group's has a thick bar across its top.")
  (wrong "You cannot: they look the same" "They are drawn differently on purpose: a group's icon is a box with a thick bar across its top.")
  (hint "Look at the top of the icons in the figure."))
```

---

## Opening a group

Click a group's icon (or press Enter on it) and the dock shows the
group's apps **in place of the row**, with **BACK** first. Click an app
to open it; once its window is up, the dock goes back to the row by
itself.

To go back without opening anything, click BACK -- or, while the dock
has the focus, press **Esc** or **Backspace**.

```figure
(define open (toggle 'open "Desk opened" #f))
(if open
    (begin (zs-dock-row 10 100 4 0 '() 10)
           (text 21 150 "BACK" 'small 'centre) (arrow 21 140 21 124 'ink 1)
           (text 110 150 "calc  clock  cal" 'small 'centre))
    (begin (zs-dock-row 10 100 6 4 '())
           (text 128 150 "Desk" 'small 'centre) (arrow 128 140 128 124 'ink 1)))
(caption "A group opens in place of the row, in the same frame.")
```

```sources
(source "The dock's files (docs/dock.md), Groups")
```

```check
(choice
  (ask "You opened the Games group by mistake. How do you get the row of the dock back?")
  (right "Click BACK, at the start of the dock")
  (wrong "Close a window" "No window has opened, so there is nothing to close. BACK brings the row back.")
  (wrong "Click the Games icon again" "While the group is open, its icon is not on the dock: the group's apps are there instead, with BACK first.")
  (hint "The first icon of an open group."))
```

---

```try
(try (app "clock")
  (task "Use the dock, not the Open button. Press Super+2. On the dock, click the Desk group: its icon is the first of the icons with a thick bar. Click clock to open it, and see the dock go back to the row. Then press Super+1 to come back here.")
  (check (choice (ask "After clock opened, what did the dock show?")
    (right "The row again, with the groups at its end")
    (wrong "Still the Desk group" "Once an app from a group opens, the dock goes back to the row by itself.")
    (wrong "Nothing: it hid" "The dock hides only for a window that fills the screen. clock's window is small, so the dock showed the row again.")
    (hint "What does the dock do once an app from a group has opened?"))))
```

---

```check
(choice
  (ask "In which group is the settings app?")
  (right "Tools")
  (wrong "Desk" "Desk holds calc, clock and cal. settings is with the other tools for looking after the computer.")
  (wrong "Games" "Games holds the games. settings is in Tools.")
  (hint "settings is a tool for looking after the computer."))
```

```check
(choice
  (ask "The dock has the focus and shows an open group. Which key goes back to the row?")
  (right "Esc (or Backspace)")
  (wrong "Enter" "Enter opens the app the ring is on. Esc or Backspace goes back to the row.")
  (wrong "F1" "F1 is help. Esc or Backspace takes the dock back to its row.")
  (hint "The key for going back out of something."))
```

So a group is an icon with a thick bar that opens a set of apps in place
of the row, with BACK, Esc or Backspace to come back.
