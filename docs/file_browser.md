# files

A file browser. `sw/apps/files`.

```
> run wm
> run files
```

Most of this app is the `z_flist_t` widget (`sw/common/zflist.h`),
which was factored out of the file dialogs on exactly the assumption
that a browser would want the same thing. What's left is the window
around it, three buttons, and the decision about what double-clicking
something means.

## Using it

Double-click a folder to enter it, or the `..` row to go up. Arrow
keys, PageUp/PageDown, Home/End move the selection; Enter opens;
Backspace goes up a level.

**Tab** moves between the list and the three buttons, **Shift+Tab**
goes back, and **Enter** or **Space** presses the focused button. The
focused control draws a ring around it. This app is fully usable with
no mouse at all, which is a first-class case in this system — arrows
deliberately don't move between buttons, because they belong to the
list and Tab is the only way across.

| button | does |
| --- | --- |
| Open | opens the selection — same as Enter or a double-click |
| New Folder | prompts for a name and creates it in the current directory |
| Delete | asks first, naming the file, then removes it |

Buttons are laid out from the **left**, deliberately: right-aligning
them the way a dialog does would put the last one underneath the resize
grip.

Delete uses `fs_unlink()`, which removes an empty directory but not one
with anything in it — and can't report which failure it was, so the
message covers both rather than guessing.

## Opening a file

By extension, through the shared table in `sw/common/ztype.h`:

| extension | app |
| --- | --- |
| `TXT`, `ASC`, `MD` | `text` |
| `ZBM` | `draw` |
| *(none)* | run it, if it's really a program |
| anything else | "no application is associated" |

An extension nothing claims gets a dialog rather than silence — a
double-click that appears to do nothing reads as the app being broken.

**A file with no extension is only assumed to be a program, and the
assumption is checked.** `z_ftype_is_executable()` reads the first four
bytes and requires the `ZEXE` magic. That check is not optional: the
loader treats a file *without* the magic as the legacy raw executable
format and will load and jump into it regardless (see `zexec.h`,
"Backward compatibility"), so without it double-clicking a `README`
would execute it.

The filename reaches the launched app through wm's pending launch
argument — see "Launch arguments" in `docs/widgets.md`. The argument is
set *before* `z_proc_run()`, because the new process can reach its own
startup before the browser runs again.

`Z_PROC_RUN_NAME_MAX` was raised from 32 to 64 for this. The browser
can hand the loader a path several directories deep, and a longer name
is silently truncated, which surfaces as a confusing "no such file"
rather than an error about length. 64 matches `Z_FLIST_PATH_MAX`, which
is what the browser can produce.

## Adding a file type

One line in `z_ftypes[]` (`sw/common/ztype.c`):

```c
{ "WAV", "player", "Sound" },
```

`app` is what `z_proc_run()` expects — a bare program name, no path and
no extension. Nothing else needs changing; the browser, and anything
else that later asks "what opens this?", read the same table.

## Known limits

- **Each open starts a fresh process.** Double-clicking two `.TXT`
  files gives two independent `text` instances, each roughly 50KB of
  image plus 16KB of stack and heap. On a 1MB board that adds up.
  Handing the file to an already-running instance would need a way to
  find one and ask it, which the pid registry could support but nothing
  does yet.
- **The listing is bounded** at `Z_FLIST_MAX` (128) entries. A fuller
  directory is truncated; `z_flist_truncated()` reports it but this app
  doesn't yet surface it, so a file that is definitely on the card can
  simply not appear.
- **No rename, copy or move.** `fs_unlink()` and `fs_mkdir()` are what
  the filesystem API currently offers an app.

## Mounting

There is no card-detect line on this hardware (SPI only), so nothing
notices a card being inserted or swapped — the volume is mounted once
at boot. `mount` at the shell is the manual override, and also recovers
a card that failed to come up at boot because it was slow or inserted
late.

It **refuses while any file handle is open**. Remounting out from under
an open `FIL` leaves that handle describing cluster chains from the
previous mount, and the next write through it corrupts the card —
worse than making you close something first. `k_fs_open_count()`
(`sw/os/fsapi.h`) is what it checks.
