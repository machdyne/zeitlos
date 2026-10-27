# Settings

```lesson
(objective "Open settings and change the screen's shade, the keyboard layouts and the time zone.")
(minutes 15)
(keywords "keyboard layout")
```

This lesson is about the settings app, where you change how the
computer works for you.

settings is in the **Tools** group on the dock. Its window is one panel
in parts:

- **Display**: the screen's shade -- White, Amber, Green or Paper;
- **Desktop colours**: the tints, on computers that have them;
- **Preferences**: what the terminal connects to, which letters the
  keys type, Japanese writing, and the time zone;
- **Security**: a password, and when the screen locks.

**Tab** goes from control to control (Shift+Tab goes back), and Enter
or Space presses the one with the ring. Each change is saved when you
make it -- most of them in the file `/sys/zeitlos.cfg`.

```sources
(source "settings (docs/settings_app.md)")
(source "sw/data/wm.cfg: settings is in the Tools group")
```

```check
(choice
  (ask "Where on the dock do you find settings?")
  (right "In the Tools group")
  (wrong "In the Desk group" "Desk holds calc, clock and cal. settings is in Tools.")
  (wrong "It is not on the dock" "It is, inside the Tools group: click the group, then settings.")
  (hint "settings is a tool for the computer."))
```

---

## The shade of the screen

Click White, Amber, Green or Paper under Display, and the whole screen
changes at once, and stays that way the next time the computer starts.

**Super+P** steps through the same four shades from anywhere, but only
until the computer starts again: it does not change the setting.

```sources
(source "settings (docs/settings_app.md), Display: applied immediately")
(source "Zeitlos Window Manager (docs/window_manager.md), Keyboard layout key: Super+P")
```

```check
(choice
  (ask "You press Super+P until the screen is amber, then restart the computer. What shade is it after the restart?")
  (right "The one chosen in settings")
  (wrong "Amber" "Super+P changes the shade only until the next start. To keep amber, choose it in settings.")
  (wrong "Always white" "It comes back as the shade chosen in settings, whatever that is.")
  (hint "Does Super+P change the setting?"))
```

---

## Keyboard layouts and the time zone

A **keyboard layout** says which letter each key types: a German
keyboard has Z where an American one has Y. Under Keyboard layouts,
**Edit** takes a list such as `us,de`; the first is used at the start,
and **Super+Space** steps to the next.

For the time zone, choose your city in the list (type its first
letters to jump to it), then press **Set time zone**. Choosing alone
does not change it.

```sources
(source "settings (docs/settings_app.md), Editing the keyboard layouts; Choosing a time zone")
(source "Keyboard layouts (docs/keyboard_layouts.md), Using it")
```

```check
(choice
  (ask "You chose Berlin in the time zone list, but the clock has not changed. Why?")
  (right "Set time zone has not been pressed yet")
  (wrong "Berlin cannot be chosen" "Every city in the list can be. Choosing only moves the highlight; Set time zone uses it.")
  (wrong "The computer must restart first" "The clock changes within a second of pressing Set time zone.")
  (hint "Choosing alone does not change it."))
```

---

```try
(try (app "settings")
  (task "Press Super+2. On the dock, open the Tools group and click settings. Under Display, click Amber, look at the screen, then click the shade you had before. Close settings with its close box, and press Super+1 to come back here.")
  (check (choice (ask "When did the screen change to amber?")
    (right "At once, with the click")
    (wrong "After an OK button" "There is no OK: the Display buttons change the screen at once.")
    (wrong "After a restart" "No restart is needed. The shade changes with the click, and is saved too.")
    (hint "Was there anything to confirm?"))))
```

---

```check
(text
  (ask "Which key combination steps to the next keyboard layout?")
  (accept "Super+Space") (accept "Super Space") (accept "Super + Space")
  (hint "Super, for the desktop as a whole, and the biggest key."))
```

So settings, in the Tools group, sets the shade, the keyboard layouts
and the time zone, and keeps each change at once.
