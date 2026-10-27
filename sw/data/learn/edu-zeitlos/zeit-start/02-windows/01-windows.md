# Windows and focus

```lesson
(objective "Know the parts of a window, close one, and give a window the focus by clicking or with Alt+Tab.")
(minutes 20)
(keywords "titlebar" "close box" "focus (of the keyboard)")
```

This lesson is about the parts of a window, and which window your
typing goes to.

The bar along the top of a window is its **titlebar**. It shows the
window's name -- usually the app's name -- on the left.

At the right end of the titlebar is the **close box**: a small square.
Click it and the window closes, and the app with it.

```figure
(zs-win 40 50 240 140 "clock" #f #f)
(text 60 30 "the titlebar" 'small)
(arrow 100 34 110 52 'ink 1)
(text 214 30 "the close box" 'small)
(arrow 262 34 268 51 'ink 1)
(text 160 125 "what the app shows" 'small 'centre)
```

[Using the mouse](docs:welcome) and the
[desktop's guide](docs:window_manager) have more about windows.

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Titlebar icons; Window titlebar icons")
(source "sw/apps/wm/win_icons.c: the close icon, a square")
```

```check
(choice
  (ask "You want to close the clock's window. Where do you click?")
  (right "The small square at the right end of its titlebar")
  (wrong "Anywhere on the titlebar" "A click on the titlebar only chooses the window. The close box is the small square at its right end.")
  (wrong "The clock's icon on the dock" "The dock is for opening apps, not closing them. The close box is on the window itself.")
  (hint "Look at the right end of the bar along the top."))
```

---

## Before it closes

Closing a window ends its app. Some apps, such as text, ask before they
close while they hold work you have not yet kept -- a letter you are
writing, say -- so nothing is lost by a slip of the mouse.

```sources
(source "sw/apps/text/text.c: do_close() asks before discarding changes")
```

```check
(choice
  (ask "You click the close box of text while it holds a letter you have not kept. What happens?")
  (right "text asks you first, before it closes")
  (wrong "The letter is lost at once" "text asks before closing while it holds work not yet kept, so a slip of the mouse loses nothing.")
  (wrong "Nothing: text cannot be closed" "text closes from its close box like any app; it only asks first while there is work not yet kept.")
  (hint "The lesson said what an app does when it holds work you have not kept."))
```

---

## Focus

You can have many windows open, but your typing goes to one of them:
the window with the **focus**. Its titlebar is filled in, with its name
cut out of the filled bar, so you can tell it from the others at a
glance.

Click anywhere on a window to give it the focus. It also comes to the
front, on top of the others.

Choose which window to click on in the figure below.

```figure
(define c (choice 'clicked '("text" "clock") "Click on"))
(if (= c 0)
    (begin (zs-win 110 70 180 120 "clock" #f #f) (zs-win 30 30 180 120 "text" #t #f))
    (begin (zs-win 30 30 180 120 "text" #f #f) (zs-win 110 70 180 120 "clock" #t #f)))
(text 290 204 "clock's window" 'small 'right)
(caption "The window with the focus: in front, its titlebar filled.")
```

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Input")
(source "sw/apps/wm/wm.c, draw_titlebar_content(): a focused titlebar is a filled bar")
```

```check
(choice
  (ask "Two windows are open: text and clock. You type a word. Which window gets it?")
  (right "The one whose titlebar is filled in")
  (wrong "The one the pointer is over" "Typing does not follow the pointer. It goes to the window with the focus, the one with the filled titlebar.")
  (wrong "Both windows" "Your typing goes to one window only: the one with the focus.")
  (hint "Which window does the filled titlebar mark?"))
```

---

## A new window

When you open an app, its window opens on top and has the focus at
once, so you can start typing in it straight away. The window you were
using keeps its place, under the new one.

```sources
(source "sw/apps/wm/wm.c, Z_WM_CREATE_WINDOW: a process's first window takes the focus")
```

```check
(choice
  (ask "You open text while clock has the focus. Where does your typing go now?")
  (right "To text: a new app's window takes the focus")
  (wrong "Still to clock" "A newly opened app's window takes the focus, so the next thing you type goes to it, not to clock.")
  (wrong "Nowhere until you click" "No click is needed: the new window has the focus as soon as it opens.")
  (hint "Opening an app is like clicking its window."))
```

---

## Alt+Tab

With the keyboard, **Alt+Tab** gives the focus to the next window and
brings it to the front. Press it again for the one after, round and
round. The windows come in a fixed order, not the order you last used
them.

When the dock is showing, it is one of the stops. While the dock has
the focus, Left and Right choose an icon, and Enter opens its app.

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Keyboard-only operation")
```

```check
(choice
  (ask "Three windows are open. You press Alt+Tab once. What happens?")
  (right "The next window gets the focus and comes to the front")
  (wrong "The window with the focus closes" "Alt+Tab never closes anything. It moves the focus on to the next window.")
  (wrong "All three come to the front together" "Only one window can be in front with the focus. Alt+Tab moves it to the next one.")
  (hint "Tab moves on to the next thing."))
```

---

```check
(choice
  (ask "With no mouse, how do you give the focus to a window under another one?")
  (right "Press Alt+Tab until it has the focus")
  (wrong "Press Tab" "Tab alone goes to the app with the focus, which may use it to move between its own buttons. Alt+Tab is the desktop's key for the next window.")
  (wrong "It cannot be done without a mouse" "Everything the mouse does to windows has a key as well. Alt+Tab moves the focus from window to window.")
  (hint "A key combination that starts with Alt, the window key."))
```

---

```check
(text
  (ask "What is the bar along the top of a window called?")
  (accept "titlebar") (accept "title bar") (accept "the titlebar") (accept "the title bar")
  (hint "It shows the window's name, its title."))
```

So a window has a titlebar with its name and a close box, and the one
with the focus -- clicked, newly opened, or reached with Alt+Tab -- gets
your typing.
