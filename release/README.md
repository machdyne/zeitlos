# Zeitlos releases

Builds and publishes flashable images: one per board/PMOD combination,
plus a shared sdcard image.

```
$ release/zrelease check
$ release/zrelease build v0.0.3
$ release/zrelease ship v0.0.3
```

**Documentation lives in [`../docs/releases.md`](../docs/releases.md).**

That is the only document for this system, deliberately. A second copy
here would drift from it, and drift between two descriptions of one
thing is precisely what the rest of this directory spends effort
preventing — the flash map is scraped rather than restated, the board
specs are diffed against `rtl/boards.vh`, and the sdcard file list is
checked against `tools/mkfatimg.sh`. Applying a weaker standard to the
prose than to the code would be an odd choice.

So: anything worth writing down goes in `docs/releases.md`.

`release/zrelease --help` lists the commands.


## What the card holds

**Every app in `sw/apps` except the core ones.** `wm`, `net` and `term`
live in the flash archive and must NOT be duplicated onto the card: a
card copy would shadow the per-target `net` build with the wrong PHY
driver, which is the bug `check_against_script()` was written after.

`hello_win` ships as `hellowin` and `audiotest` as `audiotst`. `run`
takes the name on the card. Those short forms used to be required:
FatFs was built with `FF_USE_LFN 0`, and 8.3 was the whole namespace.
It is `FF_USE_LFN 1` now, names up to 255 UTF-16 units in UTF-8
(`sw/os/fs/fatfs/ffconf.h`), so both long names would fit. They are
still shipped short. Restoring them would change what `run` launches.


Beyond the apps, docs and the ARK scroll:

- **`apps/posix`, `apps/zcc`, `apps/vi`, `apps/zetta`** -- the
  self-hosting set. With these the machine can edit, compile and run
  without another computer (`docs/posix.md`). `zetta` is started from
  posix (`zetta notes.txt`), as `vi` is (`docs/zetta.md`).
- **`apps/bbs`, `apps/fed`** -- the BBS and its zfed node, with their
  data in **`bbs/`** and **`fed/`**, where they look for it. `bbs/` is a
  working local BBS as shipped: `run bbs`, then call it from `term`
  (`docs/bbs.md`). `fed/fed.cfg` names no network: fed runs alone until
  it is given one -- `run fed key` for this node's public key
  (`docs/fed.md`, "Managing a network").
- **`apps/cryptobench`** -- times the cryptography with and without the
  hardware blocks, and checks their answers (`docs/crypto_hw_options.md`).
- **`libz/`** -- the zcc runtime: `libz.bin`, `libz.sym`, and the
  headers a program compiled on the device includes.

`libz.bin` is not the copy linked inside `zcc.bin`. That one is the
compiler's own runtime; this one is what it EMBEDS into the programs it
builds, and the two land at different processes' `0x8000_0000`, so they
cannot be shared. `zcc` without it still runs and produces freestanding
binaries with no `printf` and no `malloc`.
