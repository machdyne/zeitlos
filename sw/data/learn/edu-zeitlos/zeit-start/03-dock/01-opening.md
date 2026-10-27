# Opening apps

```lesson
(objective "Open an app from the dock with the mouse or with the keys, and know what the dock shows while it starts.")
(minutes 15)
```

This lesson is about opening apps from the dock.

**One click** on an icon opens its app; no double-click is needed.
While the app starts, its icon is drawn the other way round -- a lit
square with the icon's shape cut out of it -- and clicking it again then
does nothing, so an app cannot be opened twice by an impatient second
click.

When the app's window appears, the icon goes back to normal. The new
window opens on top, on the workspace you are on, and has the focus.

```figure
(zs-dock-row 20 100 5 0 '())
(rect 59 103 16 16 'ink)
(line 63 107 71 115 'paper 2)
(line 71 107 63 115 'paper 2)
(text 67 150 "starting" 'small 'centre)
(arrow 67 140 67 124 'ink 1)
(caption "A starting app's icon is drawn the other way round.")
```

```sources
(source "Welcome to Zeitlos (docs/welcome.md): click a dock icon to launch that app")
(source "Zeitlos Window Manager (docs/window_manager.md), Keyboard-only operation: dock launch feedback")
```

```check
(choice
  (ask "You click text's icon on the dock once. Nothing seems to happen yet, but the icon looks reversed. What should you do?")
  (right "Wait: text is starting, and its window will appear")
  (wrong "Double-click it" "One click is enough on the dock. The reversed icon shows the app is already starting.")
  (wrong "Click it a few more times" "While the icon is reversed, more clicks do nothing. Wait for the window.")
  (hint "What does a reversed icon mean?"))
```

---

## What is on the dock

From the left, the dock starts with term (the terminal), then the app
for finding your saved work (a later unit), then the apps for your
work: text for writing, read for reading documents, learn, web
for the web, sheet for tables of numbers, draw for pictures, and more.
The last icons on the dock each hold a set of other apps; the next
lesson is about them.

An app that is not installed on this computer, or that needs a part the
computer does not have, is left off the dock, so what you see can work.

```sources
(source "The dock's files (docs/dock.md); sw/data/wm.cfg, the built-in dock")
```

```check
(choice
  (ask "A friend's dock has more icons than yours. Why might that be?")
  (right "Their computer has more apps installed, or parts yours does not")
  (wrong "Your dock is broken" "The dock shows only apps that are installed and can run on the computer. Fewer icons means fewer such apps.")
  (wrong "Icons fall off the dock when you use them" "Using an app does not change the dock. It shows the apps that are installed and can run.")
  (hint "What does the dock leave out?"))
```

---

## With the keys

Press **Alt+Tab** until the dock has the focus: a thin ring then shows
around one of its icons. **Left** and **Right** move the ring, and
**Enter** opens the app it is on.

With speech on (**Super+S**), the computer says each app's name as the
ring reaches it.

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Keyboard-only operation")
(source "The dock's files (docs/dock.md), dock.zic: spoken names")
```

```try
(try (app "text")
  (task "Press Super+2. Press Alt+Tab until a ring shows around an icon on the dock. Press Right or Left to move the ring to the third icon, text, and press Enter. Then press Super+1 to come back here.")
  (check (choice (ask "Which key opened the app the ring was on?")
    (right "Enter")
    (wrong "Space" "On the dock it is Enter that opens the app the ring is on.")
    (wrong "Tab" "Tab with Alt moves the focus between windows. Enter opens the app.")
    (hint "The key that says \"do it\"."))))
```

---

```check
(choice
  (ask "How many clicks open an app from the dock?")
  (right "One")
  (wrong "Two, a double-click" "Elsewhere a double-click opens things, but on the dock a single click is enough.")
  (wrong "None: point at it and wait" "Pointing does nothing by itself. Click the icon once.")
  (hint "The dock is quicker than a double-click."))
```

```check
(choice
  (ask "You open an app from the dock while on workspace 4. Where does its window open?")
  (right "On workspace 4, on top, with the focus")
  (wrong "On workspace 1" "New windows open on the workspace you are on, not on the first.")
  (wrong "Behind the other windows" "A newly opened app's window comes on top and has the focus.")
  (hint "Where do new windows open?"))
```

So the dock opens an app with one click, or with Alt+Tab, the arrows
and Enter, and its new window comes on top with the focus.
