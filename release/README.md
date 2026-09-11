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

Beyond the apps, docs and the ARK scroll:

- **`apps/posix`, `apps/zcc`, `apps/vi`** -- the self-hosting set. With
  these the machine can edit, compile and run without another
  computer (`docs/posix.md`).
- **`libz/`** -- the zcc runtime: `libz.bin`, `libz.sym`, and the
  headers a program compiled on the device includes.

`libz.bin` is not the copy linked inside `zcc.bin`. That one is the
compiler's own runtime; this one is what it EMBEDS into the programs it
builds, and the two land at different processes' `0x8000_0000`, so they
cannot be shared. `zcc` without it still runs and produces freestanding
binaries with no `printf` and no `malloc`.
