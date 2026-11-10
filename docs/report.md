# report

A kernel shell command: the last crash and the console log, to a file
on the RAM disk. `sw/os/sh.c` (`sh_report()`), `sw/common/zcrash.h`.

```
> report
report: /ram/report.txt, 2311 bytes
```

Typed at the serial console or the [rescue console](rescue_console.md)
(Ctrl+Alt+Esc). It writes:

```
Zeitlos 0.1.0, abi 1c6b6ccabdf8b075, up 4127s

The last crash:
*** view (pid 7) crashed: store outside its own memory
    address 4001f2c0 (kernel memory)  at pc 800012a4
    ra 80000f10  sp 8003ff70  gp 80004800  a0 4001f2c0  a1 00000000
    ended; the rest of the system is unaffected

The log:
... the console log, as much as the kernel still holds ...
```

- **Where.** `/ram/report.txt` (`Z_PATH_REPORT`), on the
  [RAM disk](ramdisk.md), not the card: writing a card while something
  is going wrong is how a card is lost. It is gone at the next power
  cycle; copy it to a USB stick (`files`, or `cp` in posix) or read it
  in `text` to keep it.
- **Nothing is sent anywhere.** The owner decides whether anyone sees
  it.
- **The crash** is the last app crash report in the console log
  ([mpu.md](mpu.md#crash-reporting)): from the last line that starts
  `*** ` and has ` crashed: ` in it, through the indented lines after
  it (`z_crash_find()`, `sw/common/zcrash.h`). With none, the file says
  so.
- **The log is the kernel's 4 KB ring** ([console.md](console.md)), so
  a crash long ago, with a lot printed since, may already be gone. A
  kernel panic halts the machine and draws its report on the screen
  instead (mpu.md); there is nothing left to run `report` in.
- **No RAM disk** (a 1MB board has none): it says it could not write
  the file. The log is still on the console.
- `checkup` ([checkup.md](checkup.md)) finds the same crash, and its
  saved report has the same log.

## Cost

About 1.2KB of the kernel (kernel.bin 159,648 -> 160,832 bytes of
163,840 with xPack GCC 14.2): the command, its help line, and
`k_klog_copy()` in `sw/os/uart.c`. It borrows 5.3KB from the kernel's
pool while it runs and gives it back.

## Testing

```
$ make -C sw/common/tests -f Makefile.zcrash      8 checks
```

`sw/common/tests/test_zcrash.c` runs `z_crash_find()` over logs with no
crash, a `***` line that is not one, one crash, two (the last), one at
the very end, one cut off by the end of the buffer.

On a board: run one of the programs that crash on purpose
([mpu.md](mpu.md#test-programs)), then `report`, then open
`/ram/report.txt` in `text`.
