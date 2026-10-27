# Moving and sizing windows

```lesson
(objective "Move a window, change its size, make it fill the screen, and put it back.")
(minutes 20)
(keywords "resize grip" "maximize" "full screen (F11)")
```

This lesson is about putting windows where you want them, at the size
you want.

To move a window, drag its titlebar. While you drag, only an outline
of the window follows the pointer; the window itself moves there when
you let go. A window is always kept wholly on the screen: you cannot
push part of it off an edge.

Drag the ring on this window's titlebar, and try to push the window off
the screen.

```figure
(zs-screen)
(define p (drag 'bar 120 47))
(define wx (clamp (- (car p) 50) 0 180))
(define wy (clamp (- (cadr p) 7) 0 160))
(zs-win wx wy 140 80 "clock" #t #f)
(caption "The window stops at the edges.")
```

```sources
(source "Welcome to Zeitlos (docs/welcome.md), Using the mouse")
(source "Zeitlos Window Manager (docs/window_manager.md), Moving a window")
```

```check
(choice
  (ask "You drag a window by its titlebar towards the right edge of the screen and keep going. What happens?")
  (right "It stops when its right side reaches the edge")
  (wrong "It slides off the screen" "Zeitlos keeps every window wholly on the screen, so it stops at the edge.")
  (wrong "It closes" "Moving never closes a window. It just stops at the edge of the screen.")
  (hint "Try it in the figure."))
```

---

## With the keys

**Alt+Left**, **Alt+Right**, **Alt+Up** and **Alt+Down** -- Alt with an
arrow key -- move the window that has the focus a short step that way.
Hold Alt and press an arrow again and again to move it further.

This is how you move a window with no mouse: Alt+Tab to give it the
focus, then Alt and the arrows.

```sources
(source "Welcome to Zeitlos (docs/welcome.md): Alt+Arrow moves the focused window")
(source "Zeitlos Window Manager (docs/window_manager.md), Keyboard-only operation")
```

```check
(choice
  (ask "With no mouse, how do you move the window that has the focus up the screen?")
  (right "Alt+Up, as many times as you need")
  (wrong "Up on its own" "The arrow keys on their own go to the app, which may use them to move in a list. Alt makes it the window's key.")
  (wrong "Super+Up" "Super combinations act on the desktop as a whole. A window is moved with Alt and an arrow.")
  (hint "Alt is the key for one window."))
```

---

## Changing the size

Many windows can change size. Such a window has a **resize grip** in
its lower-right corner: a few short slanting lines. Drag the grip to
make the window bigger or smaller; the top-left corner stays where it
is.

A window with no grip, such as settings, has one size only.

```figure
(zs-win 30 30 200 140 "text" #t #t)
(text 236 206 "the resize grip" 'small 'right)
(arrow 236 196 228 172 'ink 1)
(caption "Drag the grip: the lower-right corner follows.")
```

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Resizing")
(source "settings (docs/settings_app.md), Not resizable")
```

```check
(choice
  (ask "You want a window to be wider. What do you drag?")
  (right "The resize grip in its lower-right corner")
  (wrong "The titlebar" "Dragging the titlebar moves the window without changing its size.")
  (wrong "The close box" "The close box closes the window; it is clicked, not dragged.")
  (hint "Which corner has the slanting lines?"))
```

---

## Filling the screen

To **maximize** a window is to make it fill the screen. Double-click
its titlebar, or press **Alt+Equal** (Alt and the key just left of
Backspace). The dock hides while a window is maximized. Do the same
again to put the window back as it was.

learn is maximized now. Press Alt+Equal: learn's window gets smaller
and the dock comes back. Press Alt+Equal again to fill the screen once
more.

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Maximize and shade")
(source "learn (docs/learn.md), Windows and the dock")
```

```check
(choice
  (ask "A window fills the screen and the dock is hidden. How do you get the dock back?")
  (right "Double-click the window's titlebar, or press Alt+Equal")
  (wrong "Close every window" "There is no need to close anything. Putting the maximized window back to its size brings the dock back.")
  (wrong "Press F1" "F1 shows help. The window is put back with a double-click on its titlebar or Alt+Equal.")
  (hint "Do again what made the window fill the screen."))
```

---

## Full screen

**F11** puts the window with the focus in **full screen**, if it is a
window that can change size: it fills the whole screen with no titlebar
and no dock, so the app has every dot of the screen. Press **F11**
again, or **Esc**, to come back.

Esc is there too so that anyone who pressed F11 by mistake can get out.
In a game that has a full-screen mode of its own, F11 goes to the game
instead.

```sources
(source "Function keys (docs/keys.md), Full screen (F11)")
```

```check
(choice
  (ask "You pressed F11 by mistake and the titlebar has gone. How do you come back?")
  (right "Press F11 again, or Esc")
  (wrong "Double-click the titlebar" "In full screen the window has no titlebar to double-click. F11 or Esc brings it back.")
  (wrong "Restart the computer" "Nothing is wrong. F11 again, or Esc, puts the window back as it was.")
  (hint "The same key again, or the key for getting out."))
```

---

```check
(text
  (ask "Which key puts a window in full screen? (Type its name.)")
  (accept "F11")
  (wrong-if "F1" "F1 is help, in every app. Full screen is the eleventh function key.")
  (hint "One of the last function keys."))
```

---

```check
(choice
  (ask "What is the difference between maximized and full screen?")
  (right "Full screen hides the titlebar as well; maximized keeps it")
  (wrong "There is none" "Both fill the screen and hide the dock, but only full screen hides the window's titlebar too.")
  (wrong "Maximized hides the dock; full screen keeps it" "Both hide the dock. The difference is the titlebar, which only full screen hides.")
  (hint "Which of the two leaves you no titlebar to double-click?"))
```

So you move a window by its titlebar or with Alt and the arrows, size it
with its grip, maximize it with a double-click or Alt+Equal, and F11
gives it the whole screen.
