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
Time zone: Munich
+----------------------------------+-+
| Moscow         UTC+3             | |
| Mumbai         UTC+5:30          |#|
| Munich         UTC+1  summer +1  | |
+----------------------------------+-+
[ Reload file ]
1 other setting in the file, kept as is
```

| control | setting (`docs/config.md`) | takes effect |
| --- | --- | --- |
| Display buttons | `system.video.mode` | immediately, and saved |
| Terminal connects to | `apps.term.auto_connect` | the next term window |
| Time zone list | `system.rtc.timezone` | clock and cal, within a second |
| Reload file | -- | re-reads `/zeitlos.cfg` after editing it elsewhere |

Each value line shows what is in effect, and `(default)` when the file
does not set it.

## Choosing a time zone

A `z_tz_cities[]` city list (`zrtc.h`), in a list box (`z_listbox_t`,
`docs/widgets.md`):

- **Rows:** `UTC`, then every city with its standard offset (marked
  `summer +1` if it has daylight saving), then whole-hour offsets
  `UTC-12` .. `UTC+14`.
- **Selecting only moves the highlight.** Enter or a double-click saves.
  Browsing with the arrow keys must not write the sdcard once per row.
- **Type to jump.** A letter jumps to the first city starting with it
  (**B** is Bangkok), and the same letter again walks on (Beijing,
  Berlin, ...). Letters typed within a second build a prefix (`mun` is
  Munich). This works wherever focus is on the panel -- nothing else
  there uses letters -- and moves focus to the list.
- **A value with no row** (`UTC+5:30` typed into the file) shows no
  selection, and the line above the list shows it.
- **Tab order.** The list sits between Edit and Reload. While it has
  focus the arrow keys move its selection, and Tab moves on.

## Editing the terminal connection

**Edit** opens a text prompt pre-filled with the current value.

- **OK** saves it.
- **`default`** removes the line, so the setting goes back to its default.
  A word is needed for this because the system prompt dialog cannot tell
  an empty field from Cancel.
- **The value is checked before anything is written.** It must start
  with `port`, `serial`, `telnet`, `ssh` or `none`; otherwise a message
  explains, and the file is untouched.

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

## Keyboard-only

Fully usable with no pointer.

- **Tab and Shift+Tab** move between every control, including the list.
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
