# checkup

Kind: a **desktop app** ([app_kinds.md](app_kinds.md)), on the card
(`base` set).

Tests this machine, one line a check, and saves what it found if asked.
`sw/apps/checkup`.

```
> run checkup
```

**Status: host-tested, not yet run on a board.** `tests/render.c`
(25 checks) runs every check to the end against a scripted kernel,
with failures made on purpose, and saves the report.

```
+--------------------------------------------------------------+
| Checkup                                                      |
| Tests this machine. The card is only read.                   |
| System       pass  Zeitlos 0.1.0, abi 1c6b..., 32MB, up 13s  |
| Memory       pass  20480KB free; 256KB written and read back |
| Flash store  pass  3 keys, 312 of 4096 bytes used            |
| Random       pass  healthy; 50% ones, 0 repeats              |
| Clock        note  counting, but not set yet                 |
| Memory card  pass  7592MB, 6835MB free; 96KB read            |
| RAM disk     pass  8KB written, read back and removed        |
| Network      pass  the network driver is running (pid 2)     |
| Crashes      pass  none in the console log                   |
| ------------------------------------------------------------ |
| done: nothing failed   [Run again] [Save report...] [Close]  |
+--------------------------------------------------------------+
```

## The checks

They run in order as soon as the window opens, and again on **Run
again** (F5).

| check | what it does | pass | otherwise |
|---|---|---|---|
| System | the release, the ABI the kernel was built with, the bitstream's memory, uptime | always | -- |
| Memory | the kernel's pool; then a block of RAM borrowed as this app's [arena](kernel.md) -- a quarter of the largest free block, at most 256KB -- written with a pattern that differs at every word, read back, then the same with its complement; the arena is given back | every word read back | FAIL with the address, what was read and what was written; skip on a kernel without memory statistics or with nothing to borrow |
| Flash store | the [key/value store](kvstore.md) answers: keys, bytes used of 4 KB, whether this bitstream can write it | it answers | FAIL if it does not; skip on a kernel without it |
| Random | the [TRNG](trng.md): its health bit, then 256 raw words within 2 seconds, counted for ones (45-55%) and for a word repeated straight after itself | healthy, the words arrive, ones about half, no repeats | FAIL; skip on a bitstream without it. A check that the source works, not a test of randomness |
| Clock | the [RTC](rtc.md): there, and counting over 1.25 seconds | counting and set | note: counting but not set yet (the network sets it); FAIL if it does not count; skip without one |
| Memory card | mounted (`fs_df()`), its size and free space, and `/sys/manifest` read to the end in 1 KB steps | read to its size | FAIL on a short read; skip with no card |
| RAM disk | 8 KB written to `/tmp/checkup.tmp`, read back, compared and removed | the same | FAIL; skip with no RAM disk (1MB boards) |
| Network | the network driver (`net0`) is running | running | note: not running |
| Crashes | the last app crash report in the console log (`z_crash_find()`, [report.md](report.md)) | none | note: its first line |

**Nothing is written to the card.** It is only read; the write test is
on the RAM disk, and the RAM test on memory lent to this app alone.

**Responsive** ([responsive.md](responsive.md)): every check is a step
of well under 20 ms -- 16 KB of the RAM test, 1 KB of the manifest, at
most 64 TRNG words -- and keys and clicks are answered between steps.
The clock and the TRNG wait across steps, not in one.

## Save report

**Save report...** writes, to a file the person chooses (the save
dialog starts on the USB stick if one is in, else the RAM disk -- not
the card, since a checkup is often run because something is wrong):

```
Zeitlos checkup, up 13s: nothing failed

System       pass  Zeitlos 0.1.0, abi 1c6b6ccabdf8b075, 32MB, up 13s
Memory       pass  20480KB free; 256KB written and read back twice
...

The last crash:
*** view (pid 7) crashed: ...

The console log:
...
```

A file already there is replaced only after asking. Nothing is sent
anywhere; the owner decides whether anyone sees it.

## Keys

| | |
|---|---|
| **F5** | Run again |
| **Tab / Shift+Tab** | the next button |
| **Enter, Space** | press it (Save report... has the keyboard at first) |
| **F1** | help ([app_help.md](app_help.md)) |

## Size

85,952 bytes on the card, 129,148 in RAM (xPack GCC 14.2): `ZFMT`, the
save dialog, and a 4 KB copy of the console log with a 6 KB report
buffer. Not a core app; a card-less machine has the kernel's `report`
([report.md](report.md)).

## Testing

```
$ sudo sysctl -w vm.mmap_min_addr=0
$ make -C sw/apps/checkup test         25 checks, 3 pictures in /tmp/checkup-render
```

`tests/render.c` is the real `checkup.c` against a scripted kernel:
the memory pool, an arena mapped at a fixed address, the key/value
store, a card with a manifest read in small chunks, a RAM disk and a
USB stick, the network driver, and a console log. It checks:

- every check runs to the end, in many steps, and the arena is given
  back;
- a memory word that will not hold its value is found, by address;
- a crash in the log is named, and the saved report carries it whole;
- the report has the results and the log; the save dialog starts on the
  USB stick, or the RAM disk without one; a file already there is
  replaced only after asking;
- on a bare machine (no card, RAM disk, network, store or memory
  statistics) what is not there is skipped, not failed;
- F5, Tab and Enter.

There is no TRNG or RTC on the build machine, so those two report skip
there.

### On a board

1. `run checkup`: every line passes or notes; Random and Clock run.
2. After one of the programs that crash on purpose
   ([mpu.md](mpu.md#test-programs)): Crashes names it.
3. Save report to a USB stick; read it on another computer.
4. Pull the card and run it again: Memory card skips, the rest runs.
