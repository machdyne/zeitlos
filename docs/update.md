# Updates

Bringing a card up to date with a newer build without writing a new
image: the apps, docs and data change, and `/home`, the configuration
people edited, the BBS's data and everything else that is theirs stay
as they are. The plan this follows is in [layout.md](layout.md#upgrades).

```
$ make apps                                   on the PC: build
$ release/zrelease tftp                       publish to /srv/tftp/release

$ update v0.0.7 -t tftp 192.168.1.10          on the board, in posix
> run update v0.0.7 -t tftp 192.168.1.10      or at the kernel console
```

## Status (October 2026)

| | |
|---|---|
| **Done** (round 0) | the host side: `zrelease tftp`, the manifest, `/sys/manifest` on every card image, the `net.cfg` template on the card |
| **Done** (round 1) | `update` on the board, over TFTP. Run on a Lakritz against a real store; three things found there and fixed: a lost end-of-file ("Inside"), a slow first check (now CRC-32, "Two hashes"), and running out of memory writing the manifest ("Inside"). Tested on the host against stores the release tool writes, including a run stopped at every step and one out of memory at every point |
| **Done** (round 2) | objects as LZ4 blocks (manifest format 3, "Compression"); host-tested, including a format-2 store read by the new `update` |
| Next | an ABI check; HTTPS from GitHub releases as the default; then `-t nfs` |
| Later | core apps on the card on request; the ask packs; a signed pack |

What an update **does not touch**: the kernel and the core apps in
flash (`CORE_APPS` in the Makefile). Flash is updated as before
([releases.md](releases.md#flashing-the-parts-separately),
[dfu_upgrade.md](dfu_upgrade.md)).

## On the board: `update`

```
update                              what the card has
update v0.0.7 -t tftp 192.168.1.10  update from a TFTP server (an address or a name)
update latest -t tftp server        whatever the server's LATEST names
update -n v0.0.7 -t tftp server     what would change; changes nothing
update -v v0.0.7 -t tftp server     SHA-256 every file on the card; trust no manifest
update ... -d DIR                   the store is DIR/ on the server, not release/
```

No `net.cfg` is needed: with none, `net` takes its address from DHCP,
which is why TFTP came first. `update v0.0.7` with no `-t` says that
updating from the project's own releases is not there yet.

A run looks like this:

```
$ update v0.0.7 -t tftp 192.168.1.10
update: v0.0.7 from tftp 192.168.1.10
update: release is v0.0.7 snapshot 3f2a9c01e4b7 (v0.0.6-45-g58e067a-dirty, 2026-10-19T09:40:12Z)
update: card has   v0.0.7 snapshot 81c0d2e3f4a5 (v0.0.6-44-gb9736d1, 2026-10-18T17:02:55Z)
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
manifest, not enough room); 3 a usage error.

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
  at `/apps/wm` by hand to shadow flash, the ask packs.
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
list (`release/lib/mkfatimg.py`, `plan()`), so the two cannot differ.
It builds nothing: build the apps first, as for a card. The ask packs
are left out for now (below).

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
  v0.0.7/manifest                 what a card of v0.0.7 holds
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

A file is stored once however many paths or releases share it, and the
same layout maps onto GitHub release assets, which are one flat list,
when updates come from there.

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
zeitlos-manifest 3
name v0.0.7
version 0.0.7
snapshot 3f2a9c01e4b7
source v0.0.6-45-g58e067a-dirty
made 2026-10-19T09:40:12Z
packs zdocs arklite
d home
f 3f2a9c01e4b7d0a1c2e4...(64 hex) 1c2b3a49 612388 r apps/sheet
f 9d41c2a07b55...                 8e0f61d2 1734 k sys/zeitlos.cfg
```

- Header lines are `key value`; a reader skips keys it does not know.
  The format number goes up only for a change an old reader would
  misread, and `update` refuses a newer one.
- `d` lines are folders a card has even when empty; `f` lines are
  files: SHA-256, CRC-32 (8 hex digits, zlib's), size, policy, path.
  Each group is sorted by path (bytewise), so the board merges two
  manifests in one pass with no memory per file. The path is the rest of the line; it has no leading
  slash.
- `packs` says which ask packs an image carried. Their files are not
  listed.
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
| 3 | as 2 | LZ4 blocks, `o/ab/<hash>.lz` | |

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

- **Ask packs** (`/opt/ark/<pack>`, `/data/ask/<pack>`): not published,
  and an update leaves them alone. The ARK scroll (`/opt/ark/*.md`) is
  included.
- **ABI.** Card apps compile in `sw/common/syscalls.def` and the core
  apps' message protocols, and an update does not change flash. Until
  there is a check, update a card only against a flash built from the
  same tree, as with a card image.
- **Core apps on the card**, NFS, HTTPS: later rounds.
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
object, `zrelease tftp` from the command line, and -- with
mtools -- that a card image's `/sys/manifest` has the same snapshot id
as the store made from the same tree and lists exactly the card's
files.

`python3 release/lib/test_mkfatimg.py` covers the image itself.

```
$ make -C sw/apps/update/tests
```

runs two programs. `test_update` builds `upd.c` for the host (with ASan and UBSan), publishes two small
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
room, `-n`, `latest`, and `update` with no release.

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
$ sw/apps/update/tests/test_update --update CARD-DIR STORE-DIR v0.0.7 [-n] [-v]
```
