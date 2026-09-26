# The flash key/value store

A few kilobytes of settings that belong to the **machine** rather than
to a card: they exist with no sdcard inserted, and they do not travel
with one. It lives in the last 8 KB of the configuration flash and the
kernel keeps it.

**Status: running on boards** -- it holds the password, the lock
policy and the SSH host key there. Host-tested, power cuts included.
The on-board test (`kv test`, below) has not yet been run on a board,
so its timings are still to be measured.

| key | written by | |
|---|---|---|
| `auth.password` | `sw/os/auth.c` only | the password's hash |
| `sys.lock.boot`, `sys.lock.idle`, `sys.lock.console` | `sw/os/auth.c` only | the screen-lock policy |
| `apps.netserve.hostkey` | `netserve` | the SSH host key's 32-byte seed ([netserve.md](netserve.md), "SSH") |

| | |
|---|---|
| `sw/os/kvlog.c` | the format and every operation on it; no platform dependencies |
| `sw/os/kvstore.c` | the kernel's side: where, who may write, `Z_SYS_KV`, `kv` |
| `sw/common/zkv.h` | the app API |
| `sw/os/tests/test_kvlog.c` | the host test |

## What goes here, and what does not

Most configuration belongs in `/zeitlos.cfg` on the card
([config.md](config.md)), and should stay there: it is a text file
anyone can edit, back up and copy between machines.

The store is for the few things that cannot work that way:

- **Settings that must hold with no card.** A screen lock that switched
  itself off whenever the card was pulled would not be a lock.
- **Secrets and identity**, which should not be copied along with a card
  someone borrows: the password's hash, a host key.

It is deliberately small -- 4 KB of live data at most, keys of 31
characters, values of 256 bytes -- so that it cannot become a second
configuration system by accident.

**Losing it is survivable.** A full JTAG image or a DFU upgrade may
erase it (see "Images" below). That costs the password and whatever
else is here, and nothing else: the machine boots as a fresh one would.

## Where

The **last 8 KB of the chip**: two 4 KB sectors.

| flash | store | why it is free |
|---|---|---|
| 2 MB | `0x1FE000`-`0x1FFFFF` | the tail of the jumploader region; the 25F jumploader ends at `0x1E8530`, the 45F one at `0x1F7BE9` |
| 4 MB, 8 MB, 16 MB | the last 8 KB | past the jumploader and the ZAR; `zfpga` stops short of it |
| over 16 MB | the last 8 KB of the first 16 MB | all the controller's 3-byte addresses reach |

The kernel reads the chip's size from its JEDEC ID at boot
(`Z_SPIFLASH_ID`). A bitstream without the flash writer
(`Z_FEATURE2_FLASHW` clear) cannot report it; the store is then assumed
to be on a 2 MB chip and is **read-only**.

What changed elsewhere to make room:

- `release/lib/layout.py` has a `kv` region, and the jumploader's limit
  is 184 KB rather than 192. It cross-checks `Z_KV_SIZE` (`zsoc.h`)
  against `ZFPGA_KV_SIZE` (`sw/apps/zfpga/boot.c`), as it does the
  other offsets.
- `zfpga`'s "after the jumploader" space ends 8 KB before the chip does.
- `flashtest` moved from `0x1FF000`, which is now the store, to
  `0x1FC000`, and refuses outright if its sector is ever the store.
- **`Z_SYS_FLASH` refuses to erase or program the store**, whoever
  asks, with `Z_FLASH_E_RESERVED` (`zflash.h`). The check is made modulo
  the chip size, because the chip ignores address bits above its size:
  on a 2 MB chip, `0x3FE000` *is* `0x1FE000`.

## The format

Two sectors. At most one is **active**: the one whose header is valid
and has the higher generation.

```
sector header (16 bytes)          record (padded to 4 bytes)
  0  "ZKV1"                          0  key length, 1-31 (0xFF: end of log)
  4  generation                      1  type: 0x5A set, 0xD1 delete
  8  ~generation                     2  value length, 0-256
 12  (left erased)                   4  CRC-32 of bytes 0-3, key, value
                                     8  key, then value
```

A write **appends** a record to the active sector; the latest valid
record for a key wins. When the append does not fit -- or the caller
asks for `Z_KV_SCRUB` -- the store **compacts**: the live records are
copied to the other sector, the new record after them, and then that
sector's header is written with the next generation. Last of all the
old sector is erased.

### Why a power cut cannot corrupt it

Everything rests on two facts about NOR flash: programming can only
turn 1s into 0s, and erasing only turns them back.

- **The header is the commit.** It is written after every record it
  covers. Until it exists the old sector is the store; once it exists
  the new one is complete.
- **A valid header is a complete header.** The generation is stored
  twice, the second time inverted. Cut off mid-program, some bit
  position is still 1 in both copies and the pair fails the test; the
  same happens to an old header partially erased back to 1s. Without
  this check, a half-erased old sector can read as *newer* than its
  replacement -- the host test builds exactly that case.
- **A record cut off mid-program fails its CRC** and is skipped; the
  previous record for that key still stands.
- **Appends go only onto erased flash.** A cut-off append can leave
  programmed bytes past the end of the log while the length byte that
  ends the log still reads 0xFF. The next write checks that its space
  is blank and compacts instead of programming over the debris.

So after a cut, every key holds its old value or its new one. Nothing
is repaired at boot, because nothing needs to be: the next write simply
starts from what is there.

### Why reads need no lock

Nothing is cached between calls: every operation reads the flash, the
only source of truth. A writer is serialised by a lock, but a reader
takes none, so a reader can be switched out mid-scan while a writer
compacts and erases the very sector it was reading. The host test does
exactly that, and found two bugs in the first version:

1. **A false "no such key".** The sector was erased under the reader,
   so its log appeared to end early. Now an answer is only believed if
   the sector's header still has the generation it had when the scan
   began. A sector is never rewritten under the same generation, so
   that is enough, and anything else starts over.
2. **A false "empty store".** A compaction that ran between the reads
   of the two headers showed the new sector not yet committed and the
   old one already erased. The store is never actually empty once
   written -- the commit comes before the erase -- so an empty result is
   only believed when it is seen twice in a row.

The value itself is also checked against its CRC as it is copied out,
for an erase that lands between the scan and the generation check.

### Faults that report no error

Two hardware faults are handled as well as power cuts:

- **A cell that will not program** (worn flash) while the program
  reports success: every write is read back, and a mismatch is an
  error before anything is committed.
- **A misread while compacting**: each live record is read once, into
  RAM, and checked against its CRC there, and every decision after
  that -- its length, whether it is the key being replaced -- is made
  from the checked copy. The first version compared keys by reading
  the flash a second time, and a misread there made an unrelated key
  look like the one being replaced, dropping it. The host test's fault
  mode found it.

What is **not** defended: a misread during the scan itself. The SPI
flash has no ECC and the store trusts a read that passes the CRC.

## Who may write

- **One writer at a time**, system-wide. The lock records its holder,
  and a process killed inside `Z_SYS_KV` releases it through the same
  path that releases a `Z_SYS_FLASH` session (`k_flash_release_pid()`).
  There is nothing else to clean up: a writer that stops halfway is,
  to the log, a power cut.
- The store writes under its own flash session owner (`K_FLASH_KV`).
  While an app holds a `Z_SYS_FLASH` session, store writes fail with
  `Z_KV_E_BUSY` rather than interleave with it.
- **`auth.*` and `sys.*` belong to the kernel.** Through `Z_SYS_KV` and
  the `kv` command they can be listed but not read or written. Kernel
  code (`k_kv_*()`) has no such restriction; that is what auth.c's
  `passwd` uses.

This is an API boundary and not secrecy. Any app can read any address,
the flash window included ([mpu.md](mpu.md)), so a value here is hidden
from well-behaved code only. The password will therefore be stored as
a salted, slow hash, never as itself.

## For apps

`sw/common/zkv.h`:

```c
uint8_t key[32];
uint32_t len;
if (z_kv_get("apps.netserve.hostkey", key, sizeof(key), &len) != Z_KV_OK) {
    make_new_key(key);
    z_kv_set("apps.netserve.hostkey", key, 32);
}
```

- Keys: 1-31 characters of `A-Z a-z 0-9 . _ -`. Use
  `apps.<app>.<name>`, as in `/zeitlos.cfg`.
- Values: 0-256 bytes, binary is fine.
- A write returns once it is durable. An append takes a few
  milliseconds; a compaction, which is two sector erases, up to about a
  second (400 ms per erase at worst). Setting a key to the value it
  already has writes nothing.
- `z_kv_set_flags(..., Z_KV_SCRUB)` compacts at once, so that the value
  replaced is **physically** erased rather than superseded. For a
  secret being changed.
- `z_kv_entry()` enumerates keys; `z_kv_info()` reports where the store
  is and how full.

A write calls into the kernel and waits there for the flash. That is
preemptible, like every syscall but the filesystem's, so the rest of
the system keeps running.

## At the console

For example, after setting `apps.demo.greeting` twice:

```
> kv
kv: flash 0x1fe000-0x1fffff
kv: sector 0, generation 1: 2 keys, 3 records, 100 of 4096 bytes used, 72 live
  sys.lock.idle                     1  (kernel)
  apps.demo.greeting                5  "hello"
> kv set apps.demo.greeting "hello world"
> kv get apps.demo.greeting
"hello world"  (11 bytes)
> kv del apps.demo.greeting
> kv compact
kv: compacted in ... ms
```

The boot log has a line for it, after the memory and before the shell:

```
 - kv: store at 0x1fe000, 2 keys, 100 of 4096 bytes
```

## Testing

### On the host

```
cc -std=gnu99 -O2 -Wall -o /tmp/t sw/os/tests/test_kvlog.c sw/os/kvlog.c && /tmp/t
```

About a million checks in a few seconds, clean under
`-fsanitize=address,undefined`. The flash is simulated as NOR, and a
power cut leaves a random subset of the interrupted operation's bits
changed.

| suite | what |
|---|---|
| basics | every result code; the edges of every limit; a scrub leaves no trace of the old value |
| defences | one constructed case per defence, each with nothing else standing in its way |
| faults | 2,000 worn cells and 2,000 misreads during compaction |
| model | 20,000 random operations against a plain array, every key checked after each, `ENOSPC` included |
| every cut | ~5,000 power failures: every erase and program of the next operation, from 395 store states, three different tears each |
| cuts in a row | random cuts across 30,000 operations, some during recovery from the last |
| preempted reads | 3,000 reads with a whole compaction inside them |

**Every defence was checked by removing it.** A test that stays green
when the code it guards is broken is not testing it:

| broken on purpose | caught by |
|---|---|
| header written before the records | every cut |
| CRC ignored | every cut |
| no complement check on the header | defences (b) |
| append without checking the space is blank | defences (a) |
| no generation re-check in a read | preempted reads |
| one empty observation believed | preempted reads |
| old sector not erased after a scrub | basics |
| no CRC check on the copy during compaction | faults |
| no read-back after programming | faults |

The first run of that table had four survivors. Two exposed test gaps,
now closed; one exposed a real weakness, the compaction misread above.
The fourth, a separate "the log is dirty" test before appending, turned
out to be implied by the blank check -- a dirty log has unreadable bytes
exactly where the next record would go -- and was removed from the write
path. `kv` still reports a dirty log.

### On a board

```
> kv test
```

The binding: the real controller, the real timings, and the syscall's
refusals, which the host test cannot reach. It uses keys under
`test.kv.*` and removes them, and checks every other key is untouched.
It prints how long an append and a compaction took -- the numbers that
matter for the password, please report them.

**It is no longer built by default**: the password's code needed its
2.8 KB of kernel image ([security.md](security.md), "Cost"). Build the
kernel with `make KV_TEST=1` for a hardware check, and without it
afterwards.

## Cost

| | kernel image |
|---|---|
| store, `Z_SYS_KV` and `kv` | 13.3 KB |
| `kv test` | 2.8 KB more |

Measured against the kernel before this change (235,520 bytes, 26 KB
free). About 5 KB is the log itself; 1.7 KB is its work area, which is
`.bss` and therefore image ([kernel.md](kernel.md), "The 256KB image
budget"). With the password on top ([security.md](security.md)) the
kernel has 3,488 bytes of headroom now, with the password and netserve's boot start and config keys in; 704 with `kv test`.

## Wear

A sector is erased once per compaction as the target, and once as the
source. A compaction happens when a sector fills -- dozens to hundreds
of writes, depending on value size -- or on every scrubbing write.
Against the usual 100,000 erase cycles per sector, that is millions of
ordinary writes, and far more password changes than anyone makes. The
two sectors alternate, so the wear is even.

## Images

The store is never part of an image, and the running system is its
only writer. Two kinds of image cover it anyway:

- a **full** JTAG image (`mkflashimg.build(full=True)`), padded to the
  end of the flash;
- a **DFU** image, which runs from the user partition to the end of the
  flash.

Flashing either erases the store. That is accepted: most configuration
is on the card, and what is lost is the password and the other keys
listed here. The default JTAG image stops at its last piece and leaves
the store alone.
