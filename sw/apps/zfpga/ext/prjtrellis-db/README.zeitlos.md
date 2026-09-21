# Vendored: prjtrellis-db

The Project Trellis bit database for Lattice ECP5, as consumed by
`zfpga`. `tools/mkzdb.py` converts it into the packed `.zdb` files the
card carries; nothing here is read on the device.

| | |
|---|---|
| Upstream | https://github.com/YosysHQ/prjtrellis-db |
| Commit | `015e0330630d7c238c0e4f2cdd9c8157eb78c54a` (2025-09-15) |
| Licence | CC0 1.0 Universal -- see `COPYING`, copied verbatim |

## What is here

**Verbatim, trimmed only by omission.** No file has been edited, so a
refresh is `diff -r` against a newer upstream checkout.

| Path | |
|---|---|
| `COPYING` | upstream licence |
| `devices.json` | upstream, whole |
| `ECP5/LFE5U-25F/` | `tilegrid.json`, `iodb.json`, `globals.json` |
| `ECP5/tiledata/<type>/bits.db` | the 134 tile types that occur in a 25F, and no others |

## What is not, and why

| Omitted | Why |
|---|---|
| MachXO, MachXO2, MachXO3, MachXO3D | not a target |
| `LFE5U-12F` | byte-identical tilegrid to 25F; `mkzdb.py` emits one `.zdb` serving both |
| `LFE5U-45F`, `LFE5U-85F` | to be added when a phase needs them on the device (`docs/zfpga.md` §9.1). Host tests use an installed database instead |
| `LFE5UM*`, `LFE5UM5G*` | SERDES variants; no board in this tree uses one |
| `ECP5/timing/` | there is no timing analysis |
| `tiledata/` for types not in a 25F | not reachable from a vendored device |

## Refreshing

```
git clone https://github.com/YosysHQ/prjtrellis-db /tmp/ptdb
diff -r /tmp/ptdb/ECP5/tiledata/PLC2 ECP5/tiledata/PLC2      # etc.
```

Copy changed files over, update the commit above, rebuild the `.zdb`
files, and run `tests/run.sh`. A database change that alters any
existing bit breaks byte-identity with whatever `ecppack` is installed,
which is the point: the test says so before a board does.
