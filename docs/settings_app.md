# settings

System preferences. `sw/apps/settings`.

```
> run wm
> run settings
```

One setting so far — the display phosphor mode — chosen from a radio
group of White / Amber / Green / Paper. The app exists as much for the
shape as for the content: somewhere for the next preference to go that
isn't a shell command.

## Keyboard-only

Fully usable with no pointer. Tab and Shift+Tab move between the
buttons, the arrow keys move within the group, Enter or Space picks
one.

That isn't an afterthought. A settings app reachable only with a mouse
is exactly the wrong thing to have on a machine whose pointer might be
the thing you're trying to fix.

Tab is the consistent binding across the system. Arrows work here
*as well*, because a radio group is the one case where they have an
unambiguous meaning — there's nothing else on the window for them to
belong to — but Tab is what you can rely on everywhere.

## Applied immediately

Choosing a mode applies it there and then. No OK button, nothing to
cancel.

For a setting whose entire effect is visible the instant it changes, a
confirmation step asks the user to commit to something they can
already see, and undo is just picking the other one again.

The selection is set from `z_video_get_mode()` at startup and **read
back after every write**, so the group shows what the display is
actually doing rather than what was asked for. `z_video_set_mode()`
can refuse.

## When the bitstream can't do it

`z_video_mode_present()` (`sw/common/zsoc.h`) checks a signature in
the upper half of the register, not just `z_socctl_present()` — socctl
shipped before the video register existed, so a board can answer the
`ZCTR` magic correctly and still have nothing there. On such a board
the read returns 0, which is bit-for-bit identical to a working block
reporting White.

If the register is absent the title says so, and a click puts the
selection back rather than leaving the group showing a mode the
display isn't in. Changing this needs `make flash`, not
`make dev-flash` — it's an RTL change.

## Not resizable

Deliberately. A preferences panel has no content to reveal, and screen
space is scarce. Small windows are the house default.
