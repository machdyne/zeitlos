# The keyboard

```lesson
(objective "Press key combinations, and say what Alt, Super and the function keys are for.")
(minutes 15)
(keywords "key combination" "Super key" "function key")
```

This lesson is about the keys that do more than type a letter.

Some keys are meant to be held down while you press another: **Shift**,
**Ctrl** (control), **Alt** and **Super**. On most keyboards the
**Super key** is the one with a Windows logo on it, between Ctrl and
Alt.

Holding one of these and pressing another key is a
**key combination**, written with a plus: **Alt+Tab** means hold Alt
down, press Tab, then let go of both. **Ctrl+Alt+Del** means hold Ctrl
and Alt, then press Del.

```sources
(source "Welcome to Zeitlos (docs/welcome.md), Using the keyboard")
(source "Text-to-speech (docs/tts.md): the Windows/Super key")
```

```check
(choice
  (ask "How do you press Super+2?")
  (right "Hold Super down, press 2, then let go of both")
  (wrong "Press Super, let go, then press 2" "The keys of a combination are pressed together: Super stays down while you press 2.")
  (wrong "Press Super, then +, then 2" "The plus only joins the names. You never press the + key for a combination.")
  (hint "The first key is held while the second is pressed."))
```

---

## Alt, Super, Ctrl+Alt

The key combinations of the desktop follow one rule of thumb:

| Held | Acts on |
|---|---|
| **Alt** | one window |
| **Super** | the desktop as a whole |
| **Ctrl+Alt** | the machine itself |

So Alt+Tab goes from one window to the next, Super+2 changes the whole
desktop (you will see how), and Ctrl+Alt+Del restarts the computer --
after asking first, with **Reboot** and **Cancel**. Esc or Cancel
leaves everything as it was.

```sources
(source "Welcome to Zeitlos (docs/welcome.md), Using the keyboard")
(source "Zeitlos Window Manager (docs/window_manager.md), Rebooting")
```

```check
(choice
  (ask "A key combination changes the whole desktop, not one window. Which key is it most likely to start with?")
  (right "Super")
  (wrong "Alt" "Alt combinations act on one window, such as moving it.")
  (wrong "Ctrl+Alt" "Ctrl+Alt is kept for the machine itself, such as restarting it.")
  (hint "Look at the table: which key acts on the desktop as a whole?"))
```

---

## Ctrl+Alt+Del

```check
(choice
  (ask "You pressed Ctrl+Alt+Del by mistake, and a box asks: Reboot or Cancel. What do you do to go on as before?")
  (right "Press Esc, or choose Cancel")
  (wrong "Press Enter" "Enter takes the button that is lit, and at first that is Reboot: the computer would restart. Esc or Cancel keeps everything as it was.")
  (wrong "Nothing: it is too late" "The computer only restarts if you choose Reboot. Cancel leaves the screen exactly as it was.")
  (hint "Which key means \"no, go back\" in most boxes?"))
```

---

## The function keys

Along the top of the keyboard are the **function keys**, F1 to F12.
On Zeitlos each one means the same thing in every app. F1 is help:
press it in almost any app to see what the app does and what its keys
are. The last lesson of this course goes through the rest.

In learn, F5 gives a hint, F6 reads the step aloud, and F7 reports a
mistake in a lesson.

```sources
(source "Function keys (docs/keys.md)")
```

```check
(text
  (ask "Which key shows help in almost any app? (Type its name.)")
  (accept "F1")
  (wrong-if "F5" "F5 is the hint in a lesson. Help for the whole app is on the first function key.")
  (hint "The first of the function keys."))
```

---

```check
(text
  (ask "Alt+Tab is a key combination. Which key do you hold down while you press the other? (Type its name.)")
  (accept "Alt")
  (wrong-if "Tab" "Tab is the key you press and let go. The one written first, Alt, is held down.")
  (hint "The key written first is the one held."))
```

---

```check
(choice
  (ask "What does Ctrl+Alt+Del do on Zeitlos?")
  (right "Asks whether to restart the computer")
  (wrong "Closes the window you are using" "Ctrl+Alt combinations act on the machine as a whole. This one asks whether to restart it.")
  (wrong "Shows help" "Help is F1. Ctrl+Alt+Del asks whether to restart the computer.")
  (hint "Ctrl+Alt acts on the machine itself."))
```

So a key combination is keys held together: Alt for a window, Super for
the desktop, Ctrl+Alt for the machine, and F1 for help everywhere.
