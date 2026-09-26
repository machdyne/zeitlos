# Configuration: `/zeitlos.cfg`

A plain text file of `key: value` settings at the root of the sdcard.
The kernel reads it at boot, **before any app starts**, and keeps the
settings in memory; apps ask the kernel. No card or no file means every
setting has its default.

**A card that comes up late** still gets its config read, once. With
the core apps in flash, boot tries the card once rather than waiting, so
that a board with no card starts at once -- and the first access to a
freshly powered card can fail (issue #7). Boot then says `cfg: no sdcard
yet -- init will try again`, and the init script, after loading the
shells from the card (which is what brings a late card up), tries once
more: `cfg: the sdcard came up during init -- reading /zeitlos.cfg`,
exactly as `cfg reload` would, and before anything that reads a setting
(the speech service, `system.tts.enabled`). If the card is still not up,
boot gives up for this boot and says so; `cfg reload` reads it by hand.
Apps already running see the new generation; the video mode is applied
at once. `sw/os/cfg.c`, `k_cfg_retry()`, called from `init()` only.

```
# /zeitlos.cfg
apps.term.auto_connect: port repl0
system.rtc.timezone: Berlin
system.video.mode: amber
```

Settings that must hold with **no** card -- the password, the screen
lock -- are not here but in the flash key/value store
([kvstore.md](kvstore.md)), which the kernel keeps and which does not
travel with the card. `settings` edits them; see
[security.md](security.md).

## Settings

| key | default | read by | effect |
| --- | --- | --- | --- |
| `apps.netserve.allow` | `subnet` | `netserve` | accept connections from this subnet only, or `any` ([netserve.md](netserve.md)) |
| `apps.netserve.echo` | `off` | `netserve`, `init` | an echo service on this port, for testing |
| `apps.netserve.http` | `off` | `netserve`, `init` | HTTP: a port and a directory to serve, `80 /www` |
| `apps.netserve.ssh` | `off` | `netserve`, `init` | SSH: a port and a port name, `22 posix0`; needs a 10+ character password and a seeded TRNG |
| `apps.netserve.ssh_auth` | `both` | `netserve` | SSH logins by `key` (`/user/authkeys`), `password`, or `both` |
| `apps.netserve.telnet` | `off` | `netserve`, `init` | telnet: a port and a port name, `23 repl0`; needs a password of 10+ characters |
| `apps.term.auto_connect` | *(none)* | `term` | what a new term window connects to by itself |
| `system.font.japanese` | `no` | `wm`, `settings` | start `jfont` at boot, so Japanese draws at 6x12 -- about 190KB of RAM ([text_encoding.md](text_encoding.md)); `settings` switches it on and off at once |
| `system.keyboard.layouts` | `us` | `wm` | keyboard layouts, comma-separated; the first is used at start, Super+Space cycles |
| `system.rtc.timezone` | `UTC` | `clock`, `cal` | local time shown; the RTC itself stays UTC |
| `system.tts.enabled` | `no` | kernel | speech: start `tts` at boot, for a machine set up for someone who cannot see it |
| `system.tts.voice` | `recorded` | `tts` | speech: `recorded`, `male` or `female`. `recorded` is a real person's voice, built from the speech pack's diphones; without a pack that has them, the male synthesised voice speaks. `male` and `female` choose the synthesised voice; a female one is a higher pitch (200Hz) AND a shorter vocal tract (formants 17% higher), because pitch alone only makes a squeaky male voice |
| `system.tts.pitch` | `110` | `tts` | speech: base pitch in Hz, 50-300 (200 for the recorded and female voices) |
| `system.tts.formants` | `100` | `tts` | speech: vocal tract size, % of the default, 85-120 (the range measured not to clip at any pitch). Higher is a smaller, younger-sounding voice; `female` is 117 |
| `system.tts.expression` | `100` | `tts` | speech: how much the pitch moves, % -- 0 is a monotone, 200 twice as lively. Many people turn this down at high speed |
| `system.tts.rate` | `180` | `tts` | speech: words per minute, 80-450 |
| `system.tts.volume` | `200` | `tts` | speech: volume, 0-255 |
| `system.video.mode` | `white` | kernel | display colour, applied at boot and on reload |

The same table, with one-line help, is `z_cfg_known[]` in
`sw/common/zcfg.c` -- `cfg` at the console prints it with the values in
effect. Keep the two together when adding a key.

### `apps.term.auto_connect`

The same text the term Open bar (F11) takes:

```
apps.term.auto_connect: port repl0
apps.term.auto_connect: port posix0
apps.term.auto_connect: telnet bbs.machdyne.com
apps.term.auto_connect: ssh me@10.0.0.5
apps.term.auto_connect: serial 9600
```

Absent, empty or `none`: the start panel, as without a config file.

**It waits for the provider.** A window opened at boot can be on screen
before `repl0` has registered -- init loads the shells off the card, and
the dock enables once init has *started* them, not once they are
listening. So term waits up to 15 seconds for the name to appear (the
port's own name; `serial0` for serial and usbserial;
`net0` for telnet and ssh), with
the panel saying so, then connects **once**. Esc stops the wait. A
refusal or timeout lands on the panel with the reason; it never retries.

**Only when a window opens.** F12, or the far end closing the
connection, still returns to the panel -- auto-connecting again there
would turn F12 into a way back into what you were leaving.

See `docs/terminal.md`.

### `system.rtc.timezone`

The RTC counts UTC, NTP delivers UTC, and everything that stores or
compares a time uses UTC. The zone only changes what `clock` and `cal`
**display**, and both always show the zone's abbreviation, so the
reading is never ambiguous.

Two forms (`z_tz_parse()`, `sw/common/zrtc.h`):

| form | examples | meaning |
| --- | --- | --- |
| city | `Berlin`, `New York`, `Sydney`, `Kolkata` | that city's offset **and its daylight-saving rule** |
| fixed offset | `UTC`, `UTC+2`, `UTC-5:30` | `UTC+2` is two hours **ahead** of UTC; no DST |

City names match regardless of case. The **settings app lists every
city** (about sixty) plus whole-hour offsets, so nobody has to know the
exact spelling.

**NTP does not fix daylight saving.** NTP delivers UTC and nothing else
-- no zone, no DST -- so a fixed `UTC+1` in Berlin is an hour wrong from
late March to late October. That is what the city table is for: each
city carries one of four built-in rules, and the rule moves the clocks.

| rule | cities | changes |
| --- | --- | --- |
| EU | Berlin, London, Paris, Kyiv... | last Sunday of March and October, 01:00 UTC |
| US/Canada | New York, Chicago, Toronto... | 2nd Sunday of March, 1st Sunday of November, 02:00 local |
| SE Australia | Sydney, Melbourne, Adelaide | 1st Sunday of October, 1st Sunday of April |
| New Zealand | Auckland | last Sunday of September, 1st Sunday of April |
| none | Tokyo, Kolkata, Sao Paulo, Moscow... | -- |

The table is `z_tz_cities[]` in `sw/common/zrtc.c`: name, standard
offset, rule, and abbreviations for display (`CET`/`CEST`; a city
without a common abbreviation shows `UTC+8`).

**Deliberately missing:** places whose rules are irregular or keep
changing -- Chile, Egypt, Israel, Morocco, Paraguay. A city listed with
a rule that is wrong would be worse than not listing it. Use a fixed
`UTC+n` there.

**The cost of a table** is that it is compiled in. When a country
changes its rules (Mexico dropped DST in 2022, and the EU keeps
discussing it), `zrtc.c` changes. That is true of any zone database;
this one just has no automatic updates. `test_zcfg.c` checks the table
stays sorted and every entry parses.

A value that does not parse shows **UTC, labelled UTC**, and the app
prints why on the serial console.

### `system.keyboard.layouts`

The keyboard layouts to cycle through with Super+Space, by name,
separated by commas: `us,de`. The first is the one the machine starts
in. Names are the ones in [keyboard_layouts.md](keyboard_layouts.md):
`us` `gb` `de` `de-nodeadkeys` `it` `fr` `es` `latam` `br` `ch` `ch-fr`
`se` `fi` `dk` `no` `pt` `be` `us-intl` `jp`, and the Japanese input
methods `ja` `ja-kata` `ja-us`. An unknown
name is skipped with a message on the serial console; a list with
nothing usable in it means `us`.

`wm` reads it at startup and again on the first key pressed after a
reload. A reload keeps the layout in use if it is still on the list,
so editing the list does not switch the keyboard mid-sentence.

### `system.video.mode`

`white`, `amber`, `green` or `paper` -- the same words as the `color`
console command, case-sensitive. The kernel applies it when it loads
the file, at boot and on every reload.

**Absent means "leave the display alone"**, not "force white". So
removing the line and reloading does not undo a `color` typed a moment
ago. At power-on "as it is" is white anyway.

Super+P cycles the phosphor for now (white, amber, green, paper)
without touching this setting: a reboot, or a config reload, brings
this one back ([window_manager.md](window_manager.md)).

## The file format

```
# a comment
section.name: value
section.name = value
section.name value
```

- **One setting per line.** Blank lines and lines starting with `#`
  (after optional whitespace) are ignored.
- **Keys are dotted.** Letters, digits, `.`, `_` and `-`, with a `.`
  inside -- `apps.term.auto_connect`, not `auto_connect`. This is what
  keeps a stray line of prose ("remember to set the zone") from being
  read as a setting called `remember`, which the space-separated form
  would otherwise allow.
- **Separator:** `:`, `=`, or just whitespace.
- **The value** runs to the end of the line with surrounding blanks
  trimmed. There is no quoting and **no inline comment** -- a value may
  contain `#` or `:`.
- **Repeated keys:** the **last** occurrence wins.
- **Limits:** keys up to 63 characters, values up to 127, lines up to
  255. A longer line is reported and ignored, never truncated into a
  different value.
- **Unknown keys** are loaded and kept. An app from outside this tree
  can read its own settings the same way.
- **Line endings:** CRLF or LF.

A malformed line is reported on the serial console with its line number
and skipped; everything else still loads.

A commented template ships on the card image (`sw/data/zeitlos.cfg`),
every setting commented out, so a fresh card behaves exactly as if the
file were absent.

## Editing it

Any editor: `vi /zeitlos.cfg` from posix, `text`, `te` in repl, or pull
it off the card. **Then reload**, because the kernel keeps what it read
at boot:

| how | where |
| --- | --- |
| `cfg reload` | serial console |
| **Reload file** | the `settings` app |
| reboot | -- |

What picks up a reload:

- `system.video.mode` -- immediately; the kernel applies it.
- `system.rtc.timezone` -- on the next second in `clock`, the next check in `cal`.
- `apps.term.auto_connect` -- the next term window opened.
- `system.keyboard.layouts` -- the next key pressed (`wm`).

### The settings app

`settings` edits four of the settings above:

- **Colour:** a row of buttons. Choosing one applies it and saves it.
- **Time zone:** a scrolling list of cities and offsets. Select one
  (click, arrows, or a letter: B is Bangkok, B again Beijing), then
  **Set time zone** -- or Enter, or double-click.
- **Terminal auto-connect:** an **Edit** button that opens a text
  prompt. Typing `default` removes the line. A connection that does not
  start with `port`/`serial`/`telnet`/`ssh`/`none` is refused, and
  nothing is written.
- **Keyboard layouts:** another **Edit** prompt, listing the layout
  names. A name zkbd does not know is refused, and nothing is written.

**It does not clobber anything it does not recognise.** Every change
reads the file, rewrites **only the lines for the one key being changed**
with `z_cfg_text_set()`, writes the file back and asks the kernel to
reload. Comments, blank lines, other keys and even malformed lines are
copied back byte for byte, CRLF included. It shows how many other
settings the file has, so it is visible that they are being kept.

It does not keep its own model of the file and write that out, because
a model only holds what the app understands -- writing it back is
exactly how an editor loses everything else.

See `docs/settings_app.md`.

### The console

```
> cfg
/zeitlos.cfg: 2 setting(s), generation 1
  apps.term.auto_connect     port repl0
  system.rtc.timezone        UTC  (default)
  system.video.mode          amber
  my.own.key                 42  (not read by anything built in)
> cfg get system.rtc.timezone
UTC (default -- not set in /zeitlos.cfg)
> cfg reload
cfg: /zeitlos.cfg: 2 setting(s)
```

## For app authors

`sw/common/zcfg.h`; link `zcfg.o`.

```c
#include "../../common/zcfg.h"

char v[Z_CFG_VAL_MAX];
z_cfg_get("apps.myapp.greeting", v, sizeof(v));   // file value, else default
bool on = z_cfg_get_bool("apps.myapp.sound", true);
int32_t n = z_cfg_get_int("apps.myapp.rows", 25);
```

- **`z_cfg_get()`** writes the effective value -- the file's, else the
  default from `z_cfg_known[]`, else `""` -- and returns `true` only if it
  came from the file.
- **`z_cfg_get_bool()`** accepts `yes/no`, `true/false`, `on/off` and
  `1/0`, in any case.
- **Nothing to handle on failure.** An old kernel or the simulator reads
  as "not set".
- **Noticing a reload.** `z_cfg_generation()` increments on every
  (re)load. Compare it cheaply and re-read on a change -- `clock` does
  this once a second.
- **Listing what is set.** `z_cfg_entry(i, ...)` walks the settings the
  file actually sets, in file order.
- **Reloading.** `z_cfg_reload()` re-reads the file.

**Choose a key under `apps.<yourapp>.`**, and add it to `z_cfg_known[]`
and the table above if it ships in this tree.

**Editing a setting from an app:** read the file, `z_cfg_text_set()`,
write it, `z_cfg_reload()`. See `save()` in `sw/apps/settings/settings.c`.

## How it works

- **Loading** -- `k_cfg_load()` (`sw/os/cfg.c`), called by `sh()` right
  after the sdcard is mounted and before the init-cancel window and
  `init()`. Even a cancelled boot has the settings for a later `run`.
  The file is read in 256-byte chunks and assembled a line at a time,
  so its length is not bounded by a buffer.
- **The store** -- one 2KB arena of `key\0value\0` strings plus a
  48-entry offset table, in kernel `.bss`. That is **flash as well as
  RAM**, because `kernel.bin` is padded to `_end`, which is why it is
  small. A file that overflows it loads what fits and says how much was
  dropped.
- **No half-loaded store** -- the whole load runs under `k_fs_enter()`,
  so no app is scheduled between "cleared" and "filled".
- **Syscalls** -- `CFG_GET`, `CFG_ENTRY` and `CFG_RELOAD`, appended to
  the end of `sw/common/syscalls.def`, so older app binaries keep
  working. `CFG_RELOAD` reaches FatFs and is in `k_syscall_touches_fs()`.
  Arguments are plain structs (`zcfg.h`), read by the kernel in the
  caller's own mapping, like `sw/common/zfs.h`'s.
- **One parser** -- `sw/common/zcfg.c` is compiled into the kernel (with
  `-DZCFG_KERNEL`, which leaves out the app wrappers), into apps, and
  into the host tests, so there is no second opinion about what a line
  means.

Kernel cost: about 11KB of `kernel.bin` (parser, store, shell command),
leaving ~53KB of the 256KB image budget.

## Testing

```
cc -std=gnu99 -Wall -DZCFG_KERNEL -I sw/common -o /tmp/test_zcfg \
   sw/common/tests/test_zcfg.c sw/common/zcfg.c sw/common/zrtc.c
/tmp/test_zcfg
```

`test_zcfg.c` covers the following:

- **The parser:** every separator, comments, CRLF, `#` inside a value,
  undotted and overlong keys.
- **The editor:** replace, append, remove, duplicates, and byte-for-byte
  preservation of everything else.
- **Time zones:** the city table (sorted, every entry parses), and real
  2026 transitions for all four rules in both hemispheres (EU, UK, US
  Eastern and Pacific, Sydney, Auckland), checked against published
  dates rather than against the code's own idea of them.

`sw/apps/settings/tests/render.c` runs the **real settings app** against
an in-memory `/zeitlos.cfg`. It checks the following:

- editing through the UI leaves comments, unknown keys and invalid
  lines untouched;
- the time zone list: first-letter jumps and walking the matches,
  prefixes, a single click selecting without saving, the Set time zone
  button enabling, saving and disabling, Enter saving, the Tab order
  through it, every row round-trips, and a value with no row is still
  shown;
- saving allocates nothing -- the device's 16KB heap-and-stack cannot
  spare a file-sized `malloc`, which the host's `malloc` would never
  reveal;
- `default` removes a line;
- bad values are refused;
- cancelling writes nothing.

It also renders the panel. `sw/apps/term/tests/render.c` covers
auto-connect: immediate, waiting, timing out, Esc, bad values, and F12
not re-triggering it.
