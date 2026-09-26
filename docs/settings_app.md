# settings

System preferences, and the editor for `/zeitlos.cfg`. `sw/apps/settings`.

```
> run wm
> run settings
```

```
Display
[ White ][ Amber ][ Green ][ Paper ]

Preferences                  /zeitlos.cfg
Terminal connects to               [Edit]
  port repl0
Keyboard layouts (Super+Space)     [Edit]
  us,de
Time zone: Munich
+----------------------------------+-+
| Moscow         UTC+3             | |
| Mumbai         UTC+5:30          |#|
| Munich         UTC+1  summer +1  | |
+----------------------------------+-+
[ Reload file ]           [ Set time zone ]

Security                                flash
Password                      [Change][Remove]
  set
Screen lock                            [Edit]
  at start, after 5 min idle
Berlin selected -- Set time zone to use it
```

| control | setting (`docs/config.md`) | takes effect |
| --- | --- | --- |
| Display buttons | `system.video.mode` | immediately, and saved |
| Terminal connects to | `apps.term.auto_connect` | the next term window |
| Keyboard layouts | `system.keyboard.layouts` | the next key pressed |
| Japanese font | `system.font.japanese` | at once -- `jfont` is started or stopped |
| Time zone list + Set time zone | `system.rtc.timezone` | clock and cal, within a second |
| Reload file | -- | re-reads `/zeitlos.cfg` after editing it elsewhere |
| Password: Set / Change, Remove | the flash key/value store, not the file | at once |
| Screen lock: Edit | the same | at once: `wm` is told to re-read it |

Each value line shows what is in effect, and `(default)` when the file
does not set it.

## Choosing a time zone

A `z_tz_cities[]` city list (`zrtc.h`), in a list box (`z_listbox_t`,
`docs/widgets.md`):

- **Rows:** `UTC`, then every city with its standard offset (marked
  `summer +1` if it has daylight saving), then whole-hour offsets
  `UTC-12` .. `UTC+14`.
- **Selecting only moves the highlight; Set time zone saves.** The button
  is enabled only while the selected zone differs from the one in use,
  and the status line then says "*city* selected -- Set time zone to use
  it". Enter and double-click also save. Browsing with the arrow keys
  must not write the sdcard once per row, which is why selecting alone
  does not save.

  The button exists because a single click used to select, show nothing,
  and leave Enter and double-click -- mentioned nowhere on the panel --
  as the only ways to save. Someone reasonably concluded there were none.
- **Type to jump.** A letter jumps to the first city starting with it
  (**B** is Bangkok), and the same letter again walks on (Beijing,
  Berlin, ...). Letters typed within a second build a prefix (`mun` is
  Munich). This works wherever focus is on the panel -- nothing else
  there uses letters -- and moves focus to the list.
- **A value with no row** (`UTC+5:30` typed into the file) shows no
  selection, and the line above the list shows it.
- **Tab order.** Edit, the list, Set time zone (skipped while disabled),
  Reload. While the list has focus the arrow keys move its selection, and
  Tab moves on. After Set, focus returns to the list.

## Editing the terminal connection

**Edit** opens a text prompt pre-filled with the current value.

- **OK** saves it.
- **`default`** removes the line, so the setting goes back to its default.
  A word is needed for this because the system prompt dialog cannot tell
  an empty field from Cancel.
- **The value is checked before anything is written.** It must start
  with `port`, `serial`, `telnet`, `ssh` or `none`; otherwise a message
  explains, and the file is untouched.

## The Japanese font

Not a prompt but a switch: **Turn on** / **Turn off**. It acts at once.
Turning it on saves `system.font.japanese: yes` and starts `jfont`, so
Japanese draws at 6x12 in the next repaint; turning it off saves `no`
and stops `jfont`, giving back its ~190KB. (`wm` reads the setting only
at boot, so without that the change would wait for a reboot.) See
[text_encoding.md](text_encoding.md), "Japanese".

## Editing the keyboard layouts

**Edit** opens a prompt listing every layout `zkbd` has, taken from its
own table (`z_kbd_layouts[]`), so a newly added layout appears here
with no change to this app. Enter names separated by commas; the first
is the one the machine starts in, and Super+Space steps through them.
Every name is checked, and one that is not a layout is refused with
nothing written. `default` removes the line (US only). See
[keyboard_layouts.md](keyboard_layouts.md).

## It never clobbers the rest of the file

Every change goes through one function, `save()`:

1. **Read** `/zeitlos.cfg` as text.
2. **Rewrite only the lines for that one key** with `z_cfg_text_set()`
   (`sw/common/zcfg.h`).
3. **Write** it back.
4. **Reload** -- ask the kernel to re-read the file.

Everything else is copied back byte for byte: comments, blank lines,
keys this app has never heard of, even lines that are not valid
settings, and CRLF line endings. The status line counts the other
settings in the file, so it is visible that they are being kept.

It deliberately keeps no model of the file to write out. A model only
holds what the app understands, and writing it back is how an editor
loses everything else.

`sw/apps/settings/tests/render.c` checks exactly this against an
in-memory file, through the real app code, and renders the panel.

**The buffers are static, not allocated.** Saving used to `malloc` 8KB
for the new file and read the old one with `fs_mallocfile()`, and on the
device it failed with "out of memory" and wrote nothing. An app's heap
and stack share one allowance (16KB for settings, `z_proc_stack_size_for()`
in `sw/os/kernel.h`), and `_sbrk()` (`sw/common/zeitlos.c`) refuses to
grow the heap past the stack pointer. So an allocation that is trivial
on a build machine does not fit. Two 4KB `.bss` buffers
(`Z_CFG_FILE_MAX`) and `fs_read_file()` (`zfsapp.h`) replace them. The
host test used to miss this because the host's `malloc` never fails; it
now counts every allocation `settings.c` makes and fails if saving makes
any.

A file that ends up with no settings keeps a one-line comment rather
than being written empty. `fs_write_file()` reports bytes written, so an
empty write could not be told apart from a failed one. With no sdcard,
the status line says the file could not be written.

## Security: the password and the screen lock

The one section that does not edit `/zeitlos.cfg`: the kernel keeps
the password and the lock policy in the flash key/value store, so they
hold with no card, and changes them only when given the current
password ([security.md](security.md)).

- **Password** shows *not set*, *set*, or *set -- too short for network
  use* (under 10 characters: fine for the screen lock, refused by
  network logins). **Set** / **Change** asks for the current password
  if there is one, then the new one twice; **Remove** asks to confirm,
  then for the current password.
- **Screen lock** shows what is on: *at start*, *after N min idle*,
  *console too*, or *only on Super+L*. **Edit** asks three questions --
  lock at start? how many idle minutes (0 for never, 1440 at most)? the
  serial console too? -- then for the current password.
- **Every password field shows `*`** and is wiped when its dialog
  closes (`z_dialog_prompt_secret()`); the app wipes its own copies
  after each change.
- **After a change, `wm` re-reads the policy** (`Z_WM_LOCK_RELOAD`), so
  a new idle timeout applies at once.
- The buttons are disabled, and the rows say why, on a bitstream that
  cannot write the flash or a kernel without `Z_SYS_AUTH`.

The Security buttons once came up disabled on the board -- empty
boxes, which is how zwidget draws a disabled button -- because
`widgets_init()` zeroed what `read_values()` had just set. Startup is
now one function, `startup()`, that the host test calls too, so it
starts exactly as the app does.

The host test (`tests/render.c`) scripts `Z_SYS_AUTH` and walks every
path: set, a wrong current password, two new passwords that differ,
cancelling, a short password, the policy with good and bad minute
counts and a wrong password, removal, and the read-only and old-kernel
cases -- and renders the section.

## Keyboard-only

Fully usable with no pointer.

- **Tab and Shift+Tab** move between every control, including the list
  and, after Reload, the Security buttons.
  Outside the list, the arrow keys do too.
- **Enter or Space** activates one.

That isn't an afterthought. A settings app reachable only with a mouse
is exactly the wrong thing to have on a machine whose pointer might be
the thing you're trying to fix.

## Display: applied immediately

Choosing a colour applies it there and then and saves it. There is no OK
button: for a setting whose entire effect is visible the instant it
changes, a confirmation step asks the user to commit to something they
can already see.

The selection is set from `z_video_get_mode()` at startup and **read
back after every write**, so the group shows what the display is
actually doing rather than what was asked for. It is only saved if the
readback agrees.

### When the bitstream can't do it

`z_video_mode_present()` (`sw/common/zsoc.h`) checks a signature in the
upper half of the register, not just `z_socctl_present()`. socctl
shipped before the video register existed, so a board can answer the
`ZCTR` magic correctly and still have nothing there.

If the register is absent, the title says so, and a click puts the
selection back. Changing this needs `make flash`, not `make dev-flash`
-- it's an RTL change.

## Not resizable

A preferences panel has no content to reveal.
