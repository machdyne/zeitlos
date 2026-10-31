# Configuration: `/sys/zeitlos.cfg`

A plain text file of `key: value` settings in `/sys` on the sdcard, the
system's own folder ([layout.md](layout.md)).
The kernel reads it at boot, **before any app starts**, and keeps the
settings in memory; apps ask the kernel. No card or no file means every
setting has its default.

**A card that comes up late** still gets its config read, once. With
the core apps in flash, boot tries the card once rather than waiting, so
that a board with no card starts at once -- and the first access to a
freshly powered card can fail (issue #7). Boot then says `cfg: no sdcard
yet -- init will try again`, and the init script, after loading the
shells from the card (which is what brings a late card up), tries once
more: `cfg: the sdcard came up during init -- reading /sys/zeitlos.cfg`,
exactly as `cfg reload` would, and before anything that reads a setting
(the speech service, `system.tts.enabled`). If the card is still not up,
boot gives up for this boot and says so; `cfg reload` reads it by hand.
Apps already running see the new generation; the video mode is applied
at once. `sw/os/cfg.c`, `k_cfg_retry()`, called from `init()` only.

```
# /sys/zeitlos.cfg
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
| `apps.arcade.skill` | `easy` | `snake`, `blocks`, `bricks`, `mausrun` | how hard the arcade games start: `easy`, `normal` (the games as they were) or `hard`; a choice made in a game's menu is remembered there and wins ([arcade.md](arcade.md#skill)) |
| `apps.backup.to` | `/usb/backup` | `backup` | the folder on the backup drive that backups go to ([backup.md](backup.md#settings)) |
| `apps.backup.from` | `/home /data /sys/zeitlos.cfg` | `backup` | folders and files backed up, separated by spaces (quotes around a path with spaces) |
| `apps.backup.skip` | `/data/ask *.tmp` | `backup` | left out: a path starting with `/`, or a name pattern (`*`, `?`); `none` for nothing beyond the fixed rules |
| `apps.backup.keep` | `10` | `backup` | the newest backups never thinned to make room |
| `apps.backup.verify` | `yes` | `backup` | read every copy back before its backup is complete |
| `apps.backup.eject` | `no` | `backup` | release the drive after each backup, so it can be pulled |
| `apps.backup.name` | `zeitlos` | `backup` | this computer's folder on the drive, so several can share one |
| `apps.backup.watch` | `none` | `backup` | folders or files (inside `from`) whose saves are backed up on their own, 10 s after, while the backup window is open ([backup.md](backup.md#watch)) |
| `apps.bbs.dir` | `/data/bbs` | `bbs` | the BBS's data directory: its own `bbs.cfg`, the users, the bulletins ([bbs.md](bbs.md#the-data-directory)) |
| `apps.irc.channels` | *(none)* | `irc` | channels to join once connected, `#zeitlos #fpga` ([irc_app.md](irc_app.md)) |
| `apps.irc.nick` | `zeitlos` | `irc` | your nickname; an `_` is added while it is taken |
| `apps.irc.server` | `irc.libera.chat 6667` | `irc` | server and port to connect to at start; empty: wait for `/connect` |
| `apps.print.format` | `auto` | `print` | to an IPP printer: `auto` (PDF if the printer takes it, else PWG raster), `pwg`, `pdf` or `ps` ([print.md](print.md)) |
| `apps.print.paper` | `letter` | `print` | the paper when a job does not name one: `letter`, `a4`, `legal`, `a5`, `4x6` ... |
| `apps.print.printer` | (none) | `print` | the printer: `ipp://ADDRESS/ipp/print` (driverless IPP: a PIXMA, most printers since 2013) or `socket://ADDRESS` (PostScript on port 9100, a laser) ([print.md](print.md#setting-up)) |
| `apps.print.trays` | *(none)* | `print` | which tray holds which paper, for a printer with several: `a4 main; a5 rear labels` -- paper, the printer's tray name, and optionally the paper's kind ([print.md](print.md#trays)) |
| `apps.netserve.allow` | `subnet` | `netserve` | accept connections from this subnet only, or `any` ([netserve.md](netserve.md)) |
| `apps.netserve.echo` | `off` | `netserve`, `init` | an echo service on this port, for testing |
| `apps.netserve.http` | `off` | `netserve`, `init` | HTTP: a port and a directory to serve, `80 /data/netserve/www` |
| `apps.netserve.ssh` | `off` | `netserve`, `init` | SSH: listeners, `;`-separated -- a port, a port name and flags (`noauth`, `any`, `subnet`): `22 posix0; 2222 bbs0 noauth any` ([netserve.md](netserve.md#listeners)); a seeded TRNG, and a 10+ character password for any listener that is not `noauth` |
| `apps.netserve.ssh_auth` | `both` | `netserve` | SSH logins by `key` (`/data/netserve/authkeys`), `password`, or `both`; not for `noauth` listeners |
| `apps.netserve.ssh_sessions` | `2` | `netserve` | SSH sessions at a time, 1-4, about 10KB of memory each |
| `apps.netserve.telnet` | `off` | `netserve`, `init` | telnet: listeners, as for SSH: `23 repl0; 2323 bbs0 noauth`; a 10+ character password for any listener that is not `noauth` |
| `apps.term.auto_connect` | *(none)* | `term` | what a new term window connects to by itself |
| `apps.zerdesk.allow` | `subnet` | `zerdesk` | accept viewers from this subnet only, or `any` ([remote_desktop.md](remote_desktop.md)) |
| `apps.zerdesk.port` | `8080` | `zerdesk` | the remote desktop, on this port |
| `apps.zerdesk.viewers` | `6` | `zerdesk` | viewers at once, 1-6; past that, a plain-text 503 while a relay is free |
| `system.apps.nfs_first` | `yes` | kernel | a bare program name is looked for in `/nfs/apps` first, while an NFS export is mounted; `no` for the card's `/apps` first as before ([nfs.md](nfs.md)) |
| `system.cursor.large` | `no` | `wm` | start with the large pair of pointer shapes: a 9x9 X and Z, or `/data/wm/cursor.zsp` when the card has it; Super+M switches until the next reload ([cursor.md](cursor.md)) |
| `system.font.japanese` | `no` | `wm`, `settings` | start `jfont` at boot, so Japanese draws at 6x12 -- about 190KB of RAM ([text_encoding.md](text_encoding.md)); `settings` switches it on and off at once |
| `system.keyboard.layouts` | `us` | `wm` | keyboard layouts, comma-separated; the first is used at start, Super+Space cycles |
| `system.rtc.timezone` | `UTC` | `clock`, `cal`, `irc` | local time shown; the RTC itself stays UTC (file times too -- [filesystem.md](filesystem.md#timestamps)) |
| `system.tts.enabled` | `no` | kernel | speech: start `tts` at boot, for a machine set up for someone who cannot see it |
| `system.tts.voice` | `recorded` | `tts` | speech: `recorded`, `male` or `female`. `recorded` is a real person's voice, built from the speech pack's diphones; without a pack that has them, the male synthesised voice speaks. `male` and `female` choose the synthesised voice; a female one is a higher pitch (200Hz) AND a shorter vocal tract (formants 17% higher), because pitch alone only makes a squeaky male voice |
| `system.tts.pitch` | `110` | `tts` | speech: base pitch in Hz, 50-300 (200 for the recorded and female voices) |
| `system.tts.formants` | `100` | `tts` | speech: vocal tract size, % of the default, 85-120 (the range measured not to clip at any pitch). Higher is a smaller, younger-sounding voice; `female` is 117 |
| `system.tts.expression` | `100` | `tts` | speech: how much the pitch moves, % -- 0 is a monotone, 200 twice as lively. Many people turn this down at high speed |
| `system.tts.rate` | `180` | `tts` | speech: words per minute, 80-450 |
| `system.tts.volume` | `200` | `tts` | speech: volume, 0-255 |
| `system.mouse.speed` | `16` | `wm` | USB host boards: pointer speed in sixteenths of the board's own, 1-63 -- 8 half, 32 double; set in `settings` (17 levels, 4 to 63), applied by the kernel at boot and on reload; absent is 16 ([user_input.md](user_input.md#pointer-speed)) |
| `system.video.mode` | `white` | kernel | display colour, applied at boot and on reload |
| `system.video.brightness` | `8` | kernel | HDMI/DVI monochrome brightness, 1-16, applied at boot and on reload |
| `system.video.desktop_colour` | `yes` | `wm` | eight-plane colour boards: the desktop in four colours, accents on window frames and the dock ([color.md](color.md#desktop-colour)); Super+F10 switches until the next reload |
| `system.video.desktop_background` | `yes` | `wm` | desktop colour: the desktop's background tinted too |
| `system.video.desktop_theme` | `phosphor` | `wm` | desktop colour: the theme -- `phosphor`, `slate`, `nord`, ... ([color.md](color.md#themes)); settings steps through them |
| `system.video.desktop_accents` | -- | `wm` | desktop colour: custom accents over the theme, `dark,light` in RGB444 hex (`123,CA8`) |
| `system.video.desktop_base` | -- | `wm` | desktop colour: custom black and ink, `black,ink` (`223,DDE`); the palette then shows all four and the pointer is tinted |
| `system.video.composite` | `color` | kernel | composite colour boards: `color`, or `mono` for black and white |
| `system.video.ddmi_audio` | `no` | kernel | DDMI boards built with it: `yes` sends sound and display info over the HDMI connector |
| `system.update.url` | *(the built-in distribution's)* | `update` | where releases come from, `http://host/.../release/`: a mirror, or another distribution ([distribution.md](distribution.md#following-another-distribution)) |
| `system.update.key` | *(the built-in keys)* | `update` | the Ed25519 public key, 64 hex digits, releases from there must be signed with |
| `system.update.key2` | *(none)* | `update` | a second key accepted too, while a distribution changes keys |

The same table, with one-line help, is `z_cfg_known[]` in
`sw/common/zcfg.c` -- `cfg` at the console prints it with the values in
effect. Keep the two together when adding a key.

### `apps.term.auto_connect`

The same text term's Open bar takes (typed on its start panel):

```
apps.term.auto_connect: port repl0
apps.term.auto_connect: port posix0
apps.term.auto_connect: telnet bbs.machdyne.com
apps.term.auto_connect: ssh me@10.0.0.5
apps.term.auto_connect: serial 9600
```

Absent, empty or `none`: the start panel, as without a config file.

**It waits for the provider.** A window opened at boot can be on screen
before `repl0` has registered -- term starts the shells on demand, and
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

### `system.video.brightness`

1 to 16, the level Ctrl+Super+Minus / Ctrl+Super+Equal step through
and their caption shows. 8 is the white HDMI/DVI has always had, and
the power-on default; 16 is full white. The monochrome phosphor only:
colour mode, VGA and composite are unaffected
([socctl.md](socctl.md#brightness)). Applied at boot and on every
reload; absent leaves the display as it is; ignored on boards without
HDMI/DVI.

### `system.video.desktop_colour`, `system.video.desktop_background`

`yes` or `no`. On a board with eight colour planes and the colour
window (the 45F boards), wm shows the desktop in four colours: the
focused window's titlebar and frame, the dock's outline and running
dots, and with `desktop_background` the desktop itself, tinted in
colours that follow `system.video.mode` and the brightness. A focused
screen app's colour window takes precedence, and the tints come back
when it loses the focus. Super+F10 switches desktop colour until the
next reload. Ignored elsewhere ([color.md](color.md#desktop-colour)).

### `system.video.desktop_theme`, `system.video.desktop_accents`, `system.video.desktop_base`

The desktop colour scheme. `desktop_theme` names one of the built-in
themes ([color.md](color.md#themes)): `phosphor` (the default: each
display colour's own accents), accent themes such as `slate`, `navy`,
`plum` or `midnight` (new tints, the phosphor's exact black and ink),
or full themes such as `nord`, `gruvbox`, `solarized-dark` or
`workbench` (all four colours their own). An unknown name is the
default. settings' *Desktop colours* `<` `>` buttons write it.

`desktop_accents` (`dark,light`) and `desktop_base` (`black,ink`) are
custom colours in RGB444 hex -- three digits each, `#` allowed --
applied over the theme. A value that does not parse is ignored. With
`desktop_base` the palette shows every pixel, so the pointer's XOR
tints; keep black and the light accent, and the ink and the dark
accent, well apart. Choosing a theme in settings removes both.

### `system.video.ddmi_audio`

`yes` or `no` (also `on`/`off`, `1`/`0`), on a board whose bitstream
has DDMI audio (`GPU_DDMI_AUDIO`; [ddmi_audio.md](ddmi_audio.md)).
`yes` adds data islands to the HDMI/DVI output: an AVI InfoFrame that
tells a TV the picture is 640x480, 4:3, full-range RGB and computer
content -- so it does not stretch, crop or grey the blacks, and often
switches to its low-latency mode -- and the sound: everything the
board plays, as the jack or S/PDIF does, at the system volume. Takes
effect at the next frame.

**Off by default, and only on if this file says so.** These boards
cannot ask the display what it accepts (no EDID), so this is your call.
A display that shows nothing with it on -- a DVI-only monitor might --
is put right by setting `no` here: edit `zeitlos.cfg` on the card from
a PC; or at the serial console, `ddmi off` turns it off at once (until
the next boot or `cfg reload`) and `xf`/`xmf` can bring a corrected file. Absent leaves the output as it is,
which at power-on is plain DVI; ignored on every other board.

### `system.video.composite`

`color` or `mono`, on a composite board built with colour
([composite.md](composite.md#colour)). `mono` turns the colour signal
off -- no colour burst, the palette's brightness in greys -- for a TV,
a cable or a region where the colour does not work; colour programs
carry on unchanged and simply show in black and white. Applied at boot
and on every reload. Absent leaves the output as it is, which at
power-on is colour (or mono on a build with `GPU_COMPOSITE_MONO`).
Ignored on every other board, so one file serves them all.

From a REPL, `(composite-color #f)` does the same until the next
reload.

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

Any editor: `vi /sys/zeitlos.cfg` from posix, `text`, `te` in repl, or pull
it off the card. **Then reload**, because the kernel keeps what it read
at boot:

| how | where |
| --- | --- |
| `cfg reload` | serial console |
| **Reload file** | the `settings` app |
| reboot | -- |

What picks up a reload:

- `system.video.mode` -- immediately; the kernel applies it.
- `system.video.brightness`, `system.video.composite`,
  `system.video.ddmi_audio` -- at the next frame; the kernel applies
  them.
- `system.rtc.timezone` -- on the next second in `clock`, the next check in `cal`.
- `apps.term.auto_connect` -- the next term window opened.
- `system.keyboard.layouts` -- the next key pressed (`wm`).
- `system.video.desktop_colour`, `system.video.desktop_background`,
  `system.video.desktop_theme`, `_accents`, `_base` -- within a second
  (wm checks the store's generation once a second).
- the dock's arrangement, `/data/wm/wm.cfg` (not a key here, a file of
  its own: [dock.md](dock.md)) -- within a second of the next key,
  click or pointer movement (`wm`).

### The settings app

`settings` edits these of the settings above:

- **Colour:** a row of buttons. Choosing one applies it and saves it.
- **Desktop colours:** `<` `>` step through Off and the themes, saving
  each at once.
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
/sys/zeitlos.cfg: 2 setting(s), generation 1
  apps.term.auto_connect     port repl0
  system.rtc.timezone        UTC  (default)
  system.video.mode          amber
  my.own.key                 42  (not read by anything built in)
> cfg get system.rtc.timezone
UTC (default -- not set in /sys/zeitlos.cfg)
> cfg reload
cfg: /sys/zeitlos.cfg: 2 setting(s)
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
an in-memory `/sys/zeitlos.cfg`. It checks the following:

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
