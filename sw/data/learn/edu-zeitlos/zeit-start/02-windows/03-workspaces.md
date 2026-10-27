# Workspaces

```lesson
(objective "Go between the ten workspaces, and take a window from one to another.")
(minutes 20)
(keywords "workspace")
```

This lesson is about having more than one desktop.

Zeitlos has ten desktops, called **workspaces**, numbered 1 to 10. Each
has windows of its own, and one is on the screen at a time. The dock is
on every workspace.

The computer starts on workspace 1. To go to another, hold Super and
press its number: **Super+2** for workspace 2, up to **Super+9**, and
**Super+0** for workspace 10. Its name, such as "Workspace 2", shows in
the middle of the screen for a second, so you know where you are even
when it is empty.

```figure
(define w (choice 'ws '("1" "2" "3") "Workspace"))
(zs-screen)
(cond ((= w 0) (zs-win 0 0 320 240 "learn" #t #f) (text 160 120 "(the dock hidden)" 'small 'centre))
      ((= w 1) (zs-win 30 30 160 110 "text" #t #t) (zs-dock 4 210 6 0))
      (else (zs-dock 4 210 6 0) (text 160 100 "Workspace 3" 'big 'centre)
            (text 160 122 "(shown for a second)" 'small 'centre)))
```

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Workspaces")
(source "sw/apps/wm/workspace.c, ws_for_digit_usage(): Super+0 is the tenth")
```

```check
(text
  (ask "Which key combination goes to workspace 10?")
  (accept "Super+0") (accept "Super 0") (accept "Super + 0")
  (wrong-if "Super+10" "There is no 10 key: one key per workspace. The tenth is on the last digit of the row, 0.")
  (hint "The digit at the end of the number row."))
```

---

## Next and previous

**Super+]** goes to the next workspace and **Super+[** to the one
before -- the two keys just right of P. After workspace 10 comes 1
again, and before 1 comes 10.

When you come back to a workspace, the window that had the focus there
has it again.

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Workspaces")
(source "sw/apps/wm/wm.c, ws_switch(): each workspace remembers its focused window")
```

```check
(number
  (ask "You are on workspace 10 and press Super+] once. Which workspace are you on now?")
  (answer 1)
  (wrong-if 11 "There are only ten. After the tenth, Super+] goes round to the first.")
  (wrong-if 9 "Super+] goes to the next workspace; Super+[ is the one before.")
  (hint "The workspaces go round in a ring of ten."))
```

---

## New windows and the dock

A new window opens on the workspace you are on.

The dock hides only for a maximized window on the workspace on the
screen. So with learn maximized here on workspace 1, workspace 2 has
the dock showing: a good place to open other apps while you keep your
lesson.

```try
(try (app "text")
  (task "Use the dock, not the Open button, which would open text here. Press Super+2 to go to workspace 2, where the dock shows. Click the third icon on the dock to open text. Then press Super+1 to come back to this lesson.")
  (check (choice (ask "When you came back to workspace 1, where was text's window?")
    (right "Still on workspace 2: it was not on workspace 1")
    (wrong "On top of this lesson" "text was opened on workspace 2, so it stays there. Super+2 shows it again.")
    (wrong "It had closed" "Changing workspace closes nothing. text is still open on workspace 2.")
    (hint "A window stays on the workspace where it was opened."))))
```

```sources
(source "Zeitlos Window Manager (docs/window_manager.md), Workspaces: the dock")
(source "learn (docs/learn.md), Windows and the dock")
```

---

## Taking a window along

**Alt+Super+]** takes the window that has the focus to the next
workspace, and you go with it. **Alt+Super+[** takes it to the one
before.

```example
(example
  (problem "text is open on workspace 2, and you want it on workspace 3.")
  (step "Press Super+2 to go to workspace 2.")
  (step "Click text's window to give it the focus.")
  (step (choice (ask "Which key combination takes it to workspace 3?")
          (right "Alt+Super+]")
          (wrong "Super+3" "Super+3 takes you to workspace 3, but leaves text behind on 2.")
          (wrong "Alt+Tab" "Alt+Tab gives the focus to another window on this workspace; it does not move anything.")))
  (answer "Alt+Super+]: text and you arrive on workspace 3 together."))
```

```sources
(source "Welcome to Zeitlos (docs/welcome.md), Workspaces")
```

---

```check
(choice
  (ask "You press Super+5 and the screen shows only the dock. What has happened to your windows?")
  (right "They are still open, on the workspaces where you left them")
  (wrong "They were closed" "Changing workspace closes nothing. Your windows are on the other workspaces; go back to see them.")
  (wrong "They are hidden under the dock" "Each workspace has its own windows. Workspace 5 has none yet, so only the dock shows.")
  (hint "Each workspace has windows of its own."))
```

```check
(choice
  (ask "Alt+Tab goes through the windows of which workspace?")
  (right "Only the one on the screen")
  (wrong "All ten" "Windows on the other workspaces are passed by. Alt+Tab goes round the windows you can see.")
  (wrong "Only workspace 1" "Alt+Tab works on whichever workspace is on the screen.")
  (hint "Can you give the focus to a window you cannot see?"))
```

So there are ten workspaces: Super and a digit to go to one, Super+] and
Super+[ to step, and Alt+Super+] to take a window along.
