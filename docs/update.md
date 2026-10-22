# Updates

Bringing a card up to date with a newer build without writing a new
image: the apps, docs and data change, and `/home`, the configuration
people edited, the BBS's data and everything else that is theirs stay
as they are. The plan this follows is in [layout.md](layout.md#upgrades).

```
$ update v0.0.7                               on the board, in posix: from the
                                              distribution's server, signed
$ update v0.0.7 -s games                      ... and add the games set
> run update latest                           or at the kernel console

$ make apps                                   development: build on the PC,
$ release/zrelease tftp                       publish to /srv/tftp/release,
$ update v0.0.7 -t tftp 192.168.1.10          and update from it
```

Where releases come from, and why plain HTTP is safe, is
[distribution.md](distribution.md).

## Status (October 2026)

| | |
|---|---|
| **Done** (round 0) | the host side: `zrelease tftp`, the manifest, `/sys/manifest` on every card image, the `net.cfg` template on the card |
| **Done** (round 1) | `update` on the board, over TFTP. Run on a Lakritz against a real store; three things found there and fixed: a lost end-of-file ("Inside"), a slow first check (now CRC-32, "Two hashes"), and running out of memory writing the manifest ("Inside"). Tested on the host against stores the release tool writes, including a run stopped at every step and one out of memory at every point |
| **Done** (round 2) | objects as LZ4 blocks (manifest format 3, "Compression"); host-tested, including a format-2 store read by the new `update` |
| **Done** (round 3) | the ABI check ("ABI"): `tools/gen_abi.py`, `sw/common/zabi.h`, `Z_SYS_ABI`, the manifest's `abi` line, `update -f`; host-tested |
| **Done** (round 4a) | sets ("Sets", manifest format 4, `-s`); signed releases over plain HTTP from the distribution's server as the default ([distribution.md](distribution.md)); the images `zeitlos`, `-serious`, `-base`, `-arkmedium`; zdocs and arklite as files an update knows; `zrelease http`, `keygen`, `ship` to the server; host-tested, including Monocypher checking signatures made by `release/lib/ed25519.py` |
| Next (round 4b) | `update` as a core app in flash, with a window: the installer for a board with no card |
| Later | NFS was dropped; core apps on the card on request |

What an update **does not touch**: the kernel and the core apps in
flash (`CORE_APPS` in the Makefile). Flash is updated as before
([releases.md](releases.md#flashing-the-parts-separately),
[dfu_upgrade.md](dfu_upgrade.md)).

## On the board: `update`

```
update                              what the card has: version, sets, ABI
update v0.0.7                       from the distribution's server (HTTP, signed)
update latest                       whatever that server's LATEST names
update v0.0.7 -s games              ... adding a set; -s games,dev; -s all
update v0.0.7 -t http URL           from another server (same keys)
update v0.0.7 -t tftp 192.168.1.10  from a TFTP server (an address or a name; unsigned)
update -n v0.0.7                    what would change; changes nothing
update -v v0.0.7                    SHA-256 every file on the card; trust no manifest
update -f v0.0.7                    though the release's ABI is not the flash's ("ABI"),
                                    or LATEST names an older release than the card's
update ... -t tftp SERVER -d DIR    the store is DIR/ on the TFTP server, not release/
```

No `net.cfg` is needed: with none, `net` takes its address from DHCP.
The server is the one `update` was built with (Machdyne Zeitlos,
`http://zeitlos.machdyne.com/release/`) unless `system.update.url` in
`/sys/zeitlos.cfg` names another ([distribution.md](distribution.md)).

A run looks like this:

```
$ update v0.0.7
update: checking the signature ...
update: signed by key 5a1f0e9c27b3d814, which this machine trusts
update: v0.0.7 from http://zeitlos.machdyne.com/release/
update: release is v0.0.7 snapshot 3f2a9c01e4b7 (v0.0.6-45-g58e067a-dirty, 2026-10-19T09:40:12Z)
update: card has   v0.0.7 snapshot 81c0d2e3f4a5 (v0.0.6-44-gb9736d1, 2026-10-18T17:02:55Z)
update: sets base productivity audio net dev embedded games arklite
update: abi 03a25f4cbf544cf0, as the flash has
update: 3 to write (812 KB), 1 to remove, 318 unchanged
  [1/3] apps/sheet  205 KB  ok  96 KB/s
  [2/3] apps/update  51 KB  ok
  [3/3] docs/update.md  21 KB  ok
  removed docs/old.md
update: done in 9 s: 3 written, 1 removed (812 KB, 92 KB/s).
update: programs that were running still use the old files; restart them (or reboot) to use the new ones.
```

(The numbers are an illustration; a board's TFTP rate is what
[net_throughput.md](net_throughput.md) measured, 88-150 KB/s to the
card.) A file of 64 KB or more shows its progress on one line while it
arrives (`61%  140 KB/s  ~28 s left`). Run again, the same command says
`up to date` after fetching one manifest.

**How much.** The base card is 408 files, 17.4 MB, and its manifest
43 KB (October 2026, without the speech pack); the store holds it as
10.4 MB of LZ4 blocks ("Compression"), and the summary says how much
came over the network. A card with nothing on it is a few minutes at
TFTP's rate. A card from before manifests is
checked first: every file of the shipped size is read once, at the
speed the card reads (FatFs reads about 600 KB/s, [sdcard.md](sdcard.md)),
and only what differs is fetched. After that, and for any card with a
manifest, a run reads nothing it does not have to, and a development
rebuild is the files that changed. A card written from an image of the
same tree is `up to date` at once: the image's `/sys/manifest` has the
same snapshot id.

**Where the output goes.** Typed in posix, in that terminal; from the
kernel console or repl, on the console. posix now gives a program's
output to the session that started it, matched by pid, and refuses the
connection otherwise, so a program started elsewhere falls back to the
console instead of writing into some other terminal or nowhere
([posix.md](posix.md), section 9e, "Which session gets it").

**Stopping it.** `kill update0` in posix, `kill <pid>` at the console,
or from `info`. Ctrl+C does not stop a program started with `run`
([posix.md](posix.md#ctrlc)). It is safe to stop at any moment: running
it again finishes the job, using what was already fetched.

**One at a time.** A second `update` while one runs says so and stops.
`net` does one TFTP transfer at a time; a `tget` started meanwhile
makes `update` wait for it (up to ten seconds) rather than fail.

**Exit status:** 0 the card matches the release (or `-n`, or nothing
to do); 1 partly done -- a file was in use or a fetch failed, run it
again; 2 nothing could be done (no network, no such release, a damaged
manifest, not enough room, another ABI without `-f`); 3 a usage error.

`update` with no release shows `/sys/version`, the manifest's counts,
and the ABI the card's files were built for beside the flash's:

```
$ update
zeitlos 0.0.7
snapshot v0.0.7 3f2a9c01e4b7 v0.0.6-45-g58e067a 2026-10-19T09:40:12Z
/sys/manifest: 408 files, 17.4 MB
abi: card 03a25f4cbf544cf0, flash 03a25f4cbf544cf0
```

### ABI

An update writes the card and leaves flash alone, and the card's
programs talk to what is in flash: they call the kernel's syscalls
with its argument structs, are loaded by its executable loader at the
address they were linked for, and send messages to the core apps (fs,
wm, net, usb, term, ...) in those apps' protocols. A release built
against different interfaces can have programs that crash or misbehave
on this flash. So:

- **The id.** `tools/gen_abi.py` reduces the files that define those
  interfaces -- `syscalls.def` (the syscall names in order), the app
  linker script, and the headers listed in the tool (`zeitlos.h`,
  `zproc.h`, `zmsg.h`, `zexec.h`, `zfsproto.h`, `zwm.h`, `znet.h`,
  `zsubjects.h`, ...) -- to what a compiled program depends on, and
  hashes that: 16 hex digits. Comments, whitespace, `#include` lines,
  prototypes and inline helper functions are left out, so documenting
  or refactoring a header does not change it; a struct field, a
  `#define`'s value, a syscall added or moved, a message subject does.
- **Committed.** The id is in `sw/common/zabi.h`, generated and
  committed like `docs/zapi.md`. The kernel build, the apps build
  (`sw/apps/Makefile`) and `zrelease check` fail while it is stale:

  ```
  gen_abi: sw/common/zabi.h is stale: it says 03a25f4cbf544cf0, the sources say e95de854f6cbba6f.
  gen_abi: an interface between flash and the card changed (...); run `make abi` and commit sw/common/zabi.h with the change.
  ```

  So an interface change cannot reach a board or a release without a
  new id. `make abi` writes it; commit it with the change.
- **The kernel says.** `Z_SYS_ABI` (appended last in `syscalls.def`;
  `z_abi()` in `zeitlos.h`) returns the id the kernel was built with.
  The core apps are flashed with the kernel from the same tree, so it
  speaks for them too.
- **The release says.** Every manifest has an `abi` line: the store's
  (`zrelease tftp`) and a card image's `/sys/manifest`. After an update
  the card's `/sys/manifest` has the release's.
- **`update` compares** them before writing anything:

  | release | kernel | |
  |---|---|---|
  | same id | | goes ahead (`abi ..., as the flash has`) |
  | another id | | **refused**, nothing changed; `-f` goes ahead, `-n` says it would be refused |
  | an id | too old to say (no `Z_SYS_ABI`) | a warning, and goes ahead |
  | no `abi` line (an older release) | | a note (`not checked`), and goes ahead |

  ```
  update: refused: the release is built for abi 7c1e0a9d3b2f4e61, and the kernel and
          core apps in flash are abi 03a25f4cbf544cf0. Its programs may not run on this flash.
          Flash the release's kernel and core apps first, then run update again;
          or update anyway with -f (docs/update.md, "ABI"). Nothing was changed.
  ```

  A card that is already `up to date` is not checked: nothing would be
  written.

The id says "the same interfaces", not "compatible interfaces": a
syscall appended at the end is harmless to old programs, but it is a
new id all the same, since which changes are compatible cannot be told
from the text. A false alarm costs a `-f`; a missed one, a crash.
Flash it as [releases.md](releases.md#flashing-the-parts-separately)
says, kernel and core apps together, then update the card.

What the id does not cover: the bitstream (the CSRs say what a
bitstream has, [csrs.md](csrs.md)), card paths (`zpaths.h`, read by
flash and card alike but changed often, and a moved file is found by
neither side rather than misread), and a core app's behaviour as
opposed to its protocol.

### Sets

A **set** is a label on a card file: which group it comes in. Every
file has one, and a card has the sets someone chose -- all of them on
`zeitlos.img`, `base` alone on `zeitlos-base.img`:

| set | programs | and |
|---|---|---|
| base | posix, vi, zetta, settings, info, calc, clock, cal, keyboard, jfont, read, ask, update, backup | fonts, `/docs`, the ARK scroll, `zeitlos.cfg`, `net.cfg`, the zdocs pack |
| productivity | sheet, slide, print, convert, draw, view, automate | slide's tour and examples, `/media/images`, the demo scripts |
| audio | play, midi, mmod, track, tts, audiotest | the speech pack, the `.mod` files, `/media/audio` |
| net | web, irc, netserve, zerdesk, mesh, bbs, fed | `roots.der`, zerdesk's page, the BBS's and fed's data |
| dev | zcc, zfpga, hex, cryptobench, hello_win, portdemo, ttytest | libz, zcc's headers and examples, zfpga's databases and examples |
| embedded | basic, i2c, ls99, bench, logic, sechs, serial, zlink | bench's examples, the BASIC programs |
| games | casino and its games, snake, blocks, bricks, mausrun, chess, kidgames, chip8, space3d, gpu3d, gpudemo | mausrun's levels, chip8's ROMs |
| arklite | -- | the Ark Lite pack (about 18 MB) |

`release/lib/mkfatimg.py` says which (`SETS`, `APP_SET`): an app's own
`/data/<app>/` goes with it, and the release tool refuses a card file
it cannot place. A set depends only on `base`, which every card has.

**Adding:** `update v0.0.7 -s games` (several: `-s games,dev`; `-s all`
for every set the release has). Without `-s`, an update keeps the sets
the card has. A card written before sets has all of them. A blank card
gets every set unless `-s` says which (then `base` and those).

**Not removing.** There is no `-x`: a set, once on a card, stays. Taking
files away is the easiest way into a broken state, and a hand-deleted
app comes back at the next update, since the card's manifest still
lists it.

**One release per card.** `update v0.0.8 -s games` on a 0.0.7 card
brings every set the card has to 0.0.8 as well as adding games: a
0.0.7 app beside 0.0.8's `base` (the fonts, the zcc headers, the
settings formats) or 0.0.8's kernel is what "ABI" refuses. To add a set
without moving, name the card's own release: `update v0.0.7 -s games`.

**When an app moves between sets.** One rule: *an update never removes
what the release still ships; sets only decide what is added.* A file
of the release is included if its set is on the card, or if the card
has it already. So an app moved from games to dev stays on a card
without dev, and is updated; a new app in a set the card has arrives;
one in a set it does not have does not; a new set is offered, not
installed. A file the release no longer ships at all is removed as
always -- only if it is still exactly what was put there.

`update` (no release) shows the card's sets. The release's manifest
lists the sets it offers, with a title and a line each, for `-s all`
and for people.

### Going back

`update v0.0.6` on a 0.0.7 card goes back: it is the same comparison,
of the card with that release, whatever the numbers say. It needs the
server to still have v0.0.6 (nothing is deleted there), and an ABI the
flash runs -- else the flash goes back first. Apps only in 0.0.7 are
removed (unless edited); the person's files stay, though a 0.0.6 app
may not read something a 0.0.7 one saved.

`update latest` does not go back: a LATEST naming a release signed as
older than the card's is refused ([distribution.md](distribution.md#why-plain-http-is-safe));
naming it, or `-f`, does.

### A card without `update`

Cards written before round 1 have no `/apps/update`. The store keeps
the release's own copy under a name a person can type, and
`zrelease tftp` prints the line:

```
> tget 192.168.1.10 release/v0.0.7/update /apps/update
> run update v0.0.7 -t tftp 192.168.1.10
```

The same line is the answer when a release's manifest is a newer
format than the card's `update` reads: `update` says so and prints it.

### What it does with each file

Comparing the store's manifest with the card's `/sys/manifest`:

- **`r` file**: unchanged in the release and the card copy the size the
  manifest says: left without reading it. Otherwise the card copy is
  compared -- a different size needs no reading; the same size is read
  once and its CRC-32 compared -- and written if it differs. (`-v`
  reads every file and compares SHA-256.)
- **`k` file**: written if missing. Replaced only while the card copy
  is still exactly what an earlier release shipped -- its hash is the
  one in `/sys/manifest` -- so an unedited `zeitlos.cfg` gets the new
  template's comments, and an edited one is never touched. (A
  refinement of layout.md's "written only if missing".)
- **New path**: written, folders made as needed.
- **Gone from the release**: removed if the card copy still has the old
  hash; left, and reported, if someone changed it -- it is theirs now
  and drops out of the manifest.
- **In no manifest**: never touched. `/home`, files you made, an app put
  at `/apps/wm` by hand to shadow flash, the arkmed pack.
- **Of a set the card does not have**: not written, unless the card has
  that file already ("Sets").
- **A card without `/sys/manifest`** (written before this): each shipped
  file of the shipped size is read and its CRC-32 compared (sizes that
  differ need no reading); `r` files are replaced if different, `k`
  files kept, nothing removed. The card has a manifest afterwards.

Then `/sys/manifest` and `/sys/version` are written. They describe what
is really on the card: the release's entry for every file that is now
as the release has it -- including a `k` file kept because it was
edited -- and the old entry for one that is not (in use, or not reached
before a stop). So the card's snapshot id equals the release's exactly
when there is nothing left to do.

Before writing anything it checks the free space: what it will write,
plus the largest file (fetched beside its old version), less what the
replaced and removed files give back.

### Files in use

fs refuses to rename a file some program has open **for writing**, and
`update` renames the old file aside before putting the new one in
place, so such a file is found and left: the run says which, keeps the
new version as `<path>.upd`, and the next run puts it in place without
fetching it again.

A file open only **for reading** is not protected: FatFs here has no
file locking, so it can be replaced under the program reading it (tts
keeps its speech pack open this way). That program goes on reading
clusters that are no longer its file. The summary says to restart
programs that were running; rebooting after an update is the simple
answer.

### How it is safe to stop

Each file:

1. fetched to `<path>.upd`, its CRC-32 taken as it arrives (SHA-256
   too with `-v`); a wrong size or CRC
   is fetched once more, then the run stops (the server or the link is
   the problem, and every other file would fail the same way);
2. `<path>` renamed to `<path>.old`;
3. `<path>.upd` renamed to `<path>`;
4. `<path>.old` removed.

Stopped between any two steps, the card has the old file, the new
file, or neither with a `.upd`/`.old` beside it. The next run sees the
file missing or different, uses a complete `.upd` instead of fetching,
and removes the leftovers. `/sys/manifest` itself is replaced the same
way, last, and if a stop leaves it missing the next run reads its
`.upd` or `.old`. The host tests stop a run after each of its mutating
calls in turn and check that the next run always ends with exactly the
release, the person's edits intact and nothing left over.

### Inside

`sw/apps/update/upd.c` is the work, in portable C over a small table of
functions (`upd.h`): card files, fetching by name, output, a clock.
`update.c` binds it to the board: zfsapp for the card and the
terminal. `upd_net.c` is the mailbox and the TFTP source: `net`'s TFTP
client as a stream (`zstream`, the same request `tget` makes). It reads
the mailbox in one place, because `zstream_pull()` and the usual
port-send loop each throw away messages that are not theirs, and here
chunks from `net` and acknowledgements from posix arrive together.

Stream messages become **events in a queue**, not one pending slot:
`net` sends a file's last chunk and its end-of-file together, without
waiting for another pull (`try_deliver_get()`, `sw/apps/net/tftp.c`),
and both arrive in one pass. The first board run kept the chunk,
dropped the end-of-file, pulled again from a `net` that had finished,
and reported `timed out` for a manifest `net` had fetched whole. A
chunk is copied into its queue slot at once, since its bytes are lent
by `net` only until this process reads its next message.

**Two hashes.** Each file has a SHA-256 and a CRC-32 in the manifest.
The board checks with the CRC: whether the card's copy is the shipped
one, and whether a download arrived whole. SHA-256 in software costs a
Lakritz -- which leaves the SHA-256 block out to fit colour --
about 690 cycles a byte, ~70 KB/s ([sha256_hw.md](sha256_hw.md)):
slower than TFTP delivers, and on a card from before manifests, where
every file has to be looked at, minutes of checking. The CRC
(zlib's, four bytes a step from four tables) runs at about 7.5
instructions a byte, so the check goes at the speed the card can be
read. The CRC guards against accident, which is all a plain TFTP
transfer can be protected against anyway; SHA-256 names the objects,
makes the snapshot id, is what `-v` checks, and is what will make a
signed HTTPS update trustworthy.

About 51 KB on the card (integer-only printf, no FILE layer, no 64-bit
division); 512 KB of stack and heap. The heap holds the release's
manifest (fetched into 64 KB, then trimmed to its size), the card's,
their parsed entries, the plan, and the card's new manifest, which is
written in one allocation of exactly its size, counted first. Measured
on the host for the 408-file base card: about 110 KB at the peak with
no card manifest and 175 KB with one (64-bit pointers; less on the
board). The first board run had 256 KB and a writer that grew its
buffers by doubling, holding old and new blocks at once, and stopped
with "out of memory writing the manifest" after putting every file in
place -- harmless, since the next run finds the files right and writes
the manifest, but the reason the tests now count memory.

## Publishing: `zrelease tftp`

```
$ release/zrelease tftp                           /srv/tftp/release, named v<version>
$ release/zrelease tftp --dest DIR                another store
$ release/zrelease tftp --name v0.0.7-audio       another name beside it
$ release/zrelease tftp --no-latest               leave LATEST pointing where it was
$ release/zrelease tftp --dry-run                 gather and hash, write nothing
```

It gathers exactly what `zrelease sdcard` puts on a card, from the same
list (`release/lib/mkfatimg.py`, `plan()`), so the two cannot differ:
every set, and the zdocs and arklite packs when they are built (a pack
not built leaves its files out, with a note; `build` requires them).
It builds nothing: build the apps first, as for a card. It does not
sign: TFTP is for development on a LAN.

`zrelease http` writes the same store **signed**, for any web server
(`--dest`, default `release/dist/http/release`), with the release
machine's key ([distribution.md](distribution.md)). `zrelease build`
writes it into `release/dist/update/release`, and `zrelease ship`
signs that and uploads it to the distribution's server.

Because it builds nothing, running it under `sudo` is harmless -- the
Makefile's warning about `tftp-dist` is about builds run as root. Owning
the directory is simpler:

```
$ sudo install -d -o $USER /srv/tftp/release
```

At the end it prints the line to type on the board.

### The TFTP server

Any server that serves a directory; with `tftpd-hpa` on Debian or
Ubuntu, `/etc/default/tftpd-hpa`:

```
TFTP_DIRECTORY="/srv/tftp"
TFTP_OPTIONS="--secure"
```

The board asks for names relative to the server's root:
`release/v0.0.7/manifest`, `release/o/3f/3f2a9c01e4b7d0a1c2e4.lz`. A
store elsewhere than `<tftp root>/release` has to be served as that.

No `net.cfg` is needed on the board: with none, `net` takes its address
from DHCP, which is the point of starting with TFTP.

## The store

```
/srv/tftp/release/
  LATEST                          v0.0.7
  INDEX                           one line per release
  v0.0.7/manifest                 what a card of v0.0.7 holds, every set
  v0.0.7/manifest.sig             its signature (HTTP stores; distribution.md)
  v0.0.7/manifest.prev            the one it replaced (see "Publishing again")
  v0.0.7/update                   apps/update, by a name to type (see "A card without update")
  o/3f/3f2a9c01e4b7d0a1c2e4.lz    a file's contents as LZ4 blocks, named by its hash
```

**Objects, not a copy of the card.** Three reasons, all from TFTP:

- `net` takes filenames of at most 63 bytes (`sw/apps/net/tftp.c`).
  `release/v0.0.7/data/zcc/libz/include/zplat_zeitlos.h` is already 52,
  and longer card paths exist. An object's name is 36 bytes, whatever
  the path.
- TFTP cannot list a directory. The board asks for the manifest, then
  for exactly the objects it names; nothing else needs a name.
- Publishing has to be safe while a board is reading. Objects go in
  first, and the manifest is renamed into place last, so a board sees
  the old release or the new one, never a mixture.

A file is stored once however many paths or releases share it. The
same store, signed, is what the distribution's server serves over HTTP
(`zrelease http`, `zrelease ship`; [distribution.md](distribution.md)):
one GET per object over one kept-alive connection.

**INDEX** lists every release in the store:

```
# name snapshot version source made files bytes
v0.0.7 3f2a9c01e4b7 0.0.7 v0.0.6-45-g58e067a-dirty 2026-10-19T09:40:12Z 412 31544832
```

Look at any of it from the board with `tget`:

```
> tget 192.168.1.10 release/INDEX -
> tget 192.168.1.10 release/v0.0.7/manifest /tmp/manifest
```

## Versions and snapshots

An update compares **file contents**, never version strings. That is
what lets v0.0.7 be published again and again during development: after
rebuilding one app, `zrelease tftp` adds one object, and `update` will
write that one file.

Each publish has a **snapshot id**: the first 12 hex digits of SHA-256
over the manifest's file and folder lines. It depends on the files
alone -- not the time, not the commit -- so the same files always have
the same id, a card written from an image and a store made from the
same tree agree, and "is this card up to date?" is one comparison. The
manifest also records `git describe` and the time, for people.

There is no history of snapshots: a name holds one, the latest. Two
that should coexist get two names (`--name`).

### Publishing again

- **Same files:** nothing is written, not even the manifest; it says
  `unchanged`.
- **Changed files:** their objects are added, the manifest is replaced,
  and the previous one is kept as `manifest.prev`.
- **Pruning:** objects that no `manifest` or `manifest.prev` in the
  store names are deleted. `manifest.prev` is one step of grace for a
  board that read the old manifest and is still fetching its files; it
  is not a way back. A board caught by two publishes in quick
  succession reports a missing file and is run again.
- A store being published to is locked; a second `zrelease tftp` on it
  stops at once.

## The manifest

The same format for `/sys/manifest` on a card (what is there) and
`<name>/manifest` in a store (what should be):

```
zeitlos-manifest 4
name v0.0.7
version 0.0.7
snapshot 3f2a9c01e4b7
abi 03a25f4cbf544cf0
source v0.0.6-45-g58e067a-dirty
made 2026-10-19T09:40:12Z
packs
set base Base | the shell, settings, the reader, documents and fonts
set games Games | arcade, casino, chess, chip8 and more
installed base games
d home
f 3f2a9c01e4b7d0a1c2e4...(64 hex) 1c2b3a49 612388 r productivity apps/sheet
f 9d41c2a07b55...                 8e0f61d2 1734 k base sys/zeitlos.cfg
```

- Header lines are `key value`; a reader skips keys it does not know.
  The format number goes up only for a change an old reader would
  misread, and `update` refuses a newer one.
- `d` lines are folders a card has even when empty; `f` lines are
  files: SHA-256, CRC-32 (8 hex digits, zlib's), size, policy, set,
  path. A set is `[a-z][a-z0-9-]`, up to 16; `-` on a card is an entry
  carried over from a manifest without sets.
  Each group is sorted by path (bytewise), so the board merges two
  manifests in one pass with no memory per file. The path is the rest of the line; it has no leading
  slash.
- `abi` is the ABI id of the tree the files were built from ("ABI").
  A header, so not in the snapshot id; written since round 3, and a
  reader that predates it skips it. Republishing the same files with
  another `abi` rewrites the manifest though the snapshot is the same.
- `set` lines (a release): the sets it offers, in order, `name title |
  description`. `installed` (a card): the sets it has; a card without
  the line has them all.
- `packs` says which ask packs an image carried **whose files are not
  listed** -- arkmed, which only an image has. zdocs and arklite are
  listed file by file, in base and arklite.
- `/sys/manifest` and `/sys/version` are not in it. They describe the
  card, and the updater writes them itself, last. `/sys/version` is
  `zeitlos 0.0.7` as before, then `snapshot <name> <id> <source> <made>`.

`release/lib/manifest.py` writes and reads it.

**Formats.** Each step is still read, on a card and in a store; an
update writes format 3.

| | lines | objects | |
|---|---|---|---|
| 1 | no CRC | raw | round 0. With no CRC to compare, the board falls back to SHA-256, slow without the SHA-256 block |
| 2 | SHA-256 and CRC-32 | raw, `o/ab/<hash>` | round 1 |
| 3 | as 2 | LZ4 blocks, `o/ab/<hash>.lz` | round 2 |
| 4 | as 3, and the set | as 3 | round 4a. The set is in the snapshot id, so a card from a format-3 store is not `up to date` against the format-4 store of the same tree: the next run fetches nothing and rewrites the card's manifest |

The snapshot id is over the lines alone, so formats 2 and 3 of the
same files have the same id: a card updated from a format-2 store is
`up to date` against the format-3 store of the same tree. Republishing
a format-2 store (`zrelease tftp` again) rewrites its manifest as
format 3 and adds the `.lz` objects; the raw ones stay while
`manifest.prev` names them, and are pruned the publish after.

An `update` from round 1 reads up to format 2 and refuses 3, printing
the line that fetches the release's own updater (`tget <server>
release/v0.0.7/update /apps/update`, "A card without update"); the
store keeps that copy uncompressed for exactly this.

### Compression

Format 3's objects are the file in 32 KB blocks, each after a 4-byte
little-endian word: bit 31 set, the block is stored as it is; clear,
it is an LZ4 block (`lz4_Block_format.md`). The low 31 bits are the
length that follows. Every block but the last holds 32768 bytes of the
file; the manifest's size says how many the last holds. Blocks are
independent, so the board needs no history window: it collects a
block, decodes it into a 32 KB buffer with `sw/common/zexec_lz4.c` --
the decoder that unpacks compressed executables from flash -- and
writes it out. Every length is checked against the manifest's size
before anything is copied, and the CRC-32 is of the decoded file, as
before. The compressor is `tools/mkzar.py`'s, the flash archive's,
which needs nothing installed; a block that would not get smaller is
stored. Publishing the whole base card takes it about 20 s; a
republish compresses only new objects.

**Not gzip**, though gzip -9 packs the card smaller (7.8 MB, 45%). On
this CPU -- a PicoRV32 at 48 MHz, about 9 cycles an instruction,
measured by counting instructions under QEMU against cycle counts in
[cryptobench.md](cryptobench.md) -- inflate costs 98 to 139
instructions per byte it produces (docs and an app, `zinflate.c`), so
a TFTP update would go at 40-55 KB/s instead of 150-200, and an HTTPS
one would spend more on inflating than it saved on decrypting. LZ4
costs 9.5 to 10.6 instructions a byte. [executables.md](executables.md)
made the same choice for the same reason.

| | the base card | decoding, per byte |
|---|---|---|
| raw | 17.4 MB | -- |
| LZ4 blocks (as published) | 10.4 MB, 60% | ~10 instructions |
| LZ4-HC (lz4 module, for comparison) | 10.2 MB, 59% | same decoder |
| gzip -9 | 7.8 MB, 45% | ~100-140 instructions |

It matters most for HTTPS, where every byte received costs ~450 cycles
to decrypt. Over TFTP the gain depends on what limits a transfer there
-- the network's round trips, or the card's writes, which LZ4 does not
change -- and is still to be measured on the board.

### Policies

Every file the release ships has one, declared per list in
`release/lib/mkfatimg.py`, so a new list has to choose:

| | | what |
|---|---|---|
| `r` | replace | programs, the zcc runtime and headers, docs, fonts, chip databases, media, demos |
| `k` | keep | `/sys/zeitlos.cfg`, `/data/net/net.cfg`, the BBS's and fed's settings and texts, the remote desktop's page, the zcc, bench, BASIC and zfpga examples |

## Not yet

- **arkmed** (`/opt/ark/arkmed`, `/data/ask/arkmed`): on
  `zeitlos-arkmedium.img` only, never in a release's manifest; an update
  leaves it alone. zdocs and arklite are in the base and arklite sets.
- **`update` in flash**, with a window, for a board with no card:
  round 4b. Until then it is a card app (base).
- **Core apps on the card**: not planned. NFS was dropped.
- **Kabellos boards** cannot fetch from a TFTP server on the LAN (the
  ESP32's NAPT drops the replies, [esp32link.md](esp32link.md#known-issues)).
- **Empty folders** a release no longer ships are left in place.
- **Files open for reading** can be replaced under the program reading
  them ("Files in use", above).

## Testing

```
$ python3 release/lib/test_store.py
```

No toolchain needed; the apps are faked. It checks the manifest (round
trip, ordering, content-only ids, damage and newer formats refused,
bad paths), that every card file has a policy and which are `keep`,
LZ4 blocks (round trip from 0 to 100,001 bytes around the block size,
text and noise, noise stored), publishing (objects named by their hash
and decoding to it, block headers that add up, deterministic output,
every TFTP name within 63 bytes, LATEST, INDEX), publishing again
unchanged, a rebuilt app (one new object, `manifest.prev`, pruning a
publish later), a format-2 store republished as format 3 (its raw
objects kept while `manifest.prev` names them, then pruned), a damaged
object, the `abi` line (written, read back, not in the snapshot id, a
new one republishing an otherwise unchanged release, and in
`zrelease tftp`'s manifest); sets (each file's set read back, the
offered sets and their titles, a card's `installed` line outside the
snapshot id, a file moved to another set a new snapshot, bad set names
refused, `-`); signatures (Ed25519 against RFC 8032's test vectors,
`manifest.sig` written and checked, two keys, a changed manifest no
longer checking); `release/dist.cfg` and `zdist.h` current, a fork's
config, bad URLs and keys refused, `keygen` (mode 600, never inside the
repository, `--add`); `zrelease tftp` from the command line (every set
offered, not signed) and `zrelease http` refusing a key `dist.cfg` does
not list, and -- with
mtools -- that a card image's `/sys/manifest` has the same snapshot id
as the store made from the same tree, records the tree's ABI id, and
lists exactly the card's files.

```
$ python3 tools/test_gen_abi.py
```

checks the ABI id: that `sw/common/zabi.h` is current, that a comment,
whitespace, a prototype, an inline helper, an `#include`, a `#` in a
block comment and the kernel function behind a syscall do not change
it, and that a struct field, a `#define`'s value, a syscall appended,
a syscall replaced, the app link address and a function-pointer typedef
do (and leave the committed id stale).

`python3 release/lib/test_mkfatimg.py` covers the image itself, and
the sets: every listed file has one, an app without one is refused,
each set has files, a base plan has base's alone, one without base is
refused; and each image (`base`, `serious`, `full`, `arkmedium`, with
fake arklite and arkmed packs) has exactly its sets' files, says which
in `/sys/manifest`, and lists zdocs and arklite file by file but only
names arkmed.

```
$ make -C sw/apps/update/tests
```

runs three programs. `test_update` builds `upd.c` for the host (with ASan and UBSan), publishes two small
releases with `release/lib`'s own `manifest.py` and `store.py`
(`mkfixture.py`), and updates a card that is a directory: a blank card
and the same release again; v1 to v2 with changed, added and removed
files, a folder two deep and a name with a space; edited and unedited
`k` files; a removed file someone edited; a card from before manifests;
`-v`; a card whose manifest is format 1 (updated from the manifests
alone, without reading the unchanged files: the bytes read are
counted) and a card without one (each file read once, by CRC); a
file in use and the run that finishes it; **memory**: the heap
limited from 2 KB upwards until the run succeeds, every failed run
followed by one that must leave exactly the release (upd.c's
allocations are counted through `alloc_hook.h`, and `--update` prints
the peak); **the run stopped
after every one of its mutating calls** and then run again to the end;
LZ4 objects (a 300 KB file decoded across ten blocks; the transfer
handed over in chunks of varying size, so block headers and data are
split at every kind of place), a format-2 store with raw objects read
by the new `update`; a damaged object (a block header, a block's data,
a byte too many), a missing one, a damaged
and a newer-format manifest, no such release, a bad name, not enough
room, `-n`, `latest`, `update` with no release; and the ABI: the same
id (updated, recorded in the card's manifest, shown by `update`), another
(refused before anything is fetched, `-n`, `-f`), a kernel that cannot
say (a warning), and a release that does not say (not checked).
**Sets**: a blank card with `-s games` (base and games, not dev; the
card's manifest says so); up to date again, with and without `-s`; s1
to s2 (base and games updated, net and dev not added, an app moved
from games to dev kept and updated, a dropped one removed, still up to
date afterwards); `-n -s dev`, `-s dev`, `-s all`; a set the release
does not have; `-s` with a release without sets; a card from before
sets and a blank card, which get every set. **Signatures** (keys and
signatures from `release/lib/ed25519.py`, checked by the board's
Monocypher): accepted; not signed, signed by another key (and accepted
when both are trusted), changed after signing, and another release's
signed manifest under a new name, all refused with nothing written; a
file with the right size and CRC but not the manifest's SHA-256,
accepted over TFTP and refused when signed; no key at all; LATEST
naming an older release refused, `-f` and naming it going back.

`test_http` runs the HTTP client in `upd_net.c` against a `net` whose
raw sockets behave like `net.c`'s (CONNECT and CONNECTED, the request
as DATA, the reply in pieces of at most 4 KB with no more than eight
unacknowledged) and a web server behind it: files of 0, 1, 4095,
4096, 4097 and 300,000 bytes by Content-Length and by chunks (of every
size, with an extension and a trailer), read 1, 7, 1468 or 2048 bytes
at a time; twenty files on one connection; a new connection after
`Connection: close`, after each HTTP/1.0 reply, and after a server that
hangs up between files or just as a request goes out; a body ended by
the close; the request line and `Host`; 404, a redirect to https and
one elsewhere; a connection refused, a net that never answers, a
hang-up mid-file and the retry; a file abandoned half way and the next
one; and that net never has more than eight pieces unacknowledged.

`test_net` runs `upd_net.c` with the real `zstream.c` and `zobj.c`, a
mailbox per process, and a `net` that answers the way `tftp.c` does --
one chunk per pull, lent from two slots reused only after the next
pull, the last chunk and the end-of-file sent together -- with port
acks for the terminal arriving in between. Files of 0, 1, 1467, 1468,
1469, 2936, 39410 and 100000 bytes, with that end-of-file and with one
sent only on a pull; "busy" waited out and given up on; an error sent
unasked; a `net` that never answers and one that stops mid-file; a file
abandoned half way with its next chunk in flight, then another. Built
with a one-slot queue, it fails every file whose end-of-file comes with
its last chunk, which is what the board did.

The same program updates any directory from any store, which is how
the numbers above were checked against the real tree (a card image
from `zrelease sdcard`, unpacked with `mcopy -s`):

```
$ sw/apps/update/tests/test_update --update CARD-DIR STORE-DIR v0.0.7 [-n] [-v] [-f] [-s SETS] [--keys HEX]
```

(`--keys` requires a signature by one of those keys, as from a server.)

There is no kernel to ask on the host, so it behaves like a kernel too
old to say, unless `ZABI=<id>` gives the flash's id.
