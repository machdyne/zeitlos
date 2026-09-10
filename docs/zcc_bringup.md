# Testing zcc under posix on hardware

Everything in `docs/zcc.md`, `docs/libz.md` and `docs/posix.md` was
developed and checked under `sim/`, whose filesystem is a host
directory, whose CPU is an interpreter, and which has no `wm` and no
second process. This is the list of what has never run on a board, and
what to do about it.

**Nothing here has been executed on hardware.** Treat it as a plan to
follow and correct, not as a procedure that is known to work.

## 1. Build

The container this was developed in has a RISC-V toolchain with **no
newlib**, so it could not link any ordinary Zeitlos app. That is why
`libz` exists at all, and it means `posix.bin` in particular has never
been linked anywhere. Expect the first build to surface something.

Both `posix` and `zcc` are in `sw/apps/Makefile`'s `APPS` list now, so
the ordinary whole-tree build covers them:

```
make -C sw/apps
```

`zcc`'s own Makefile builds `libz` as a dependency, so `libz.bin` and
`libz.sym` appear without anything having to know the order.
Individually:

```
make -C sw/apps/zcc      # builds libz first, then the compiler
make -C sw/apps/posix
```

`-Werror` applies to each app's **own** sources only, not to the
`sw/common` objects they compile. That distinction was learned the
hard way: `-Werror` on the whole of `posix`'s CFLAGS broke the build
on `zeitlos.c`, which has warnings that vary by toolchain version and
which this app has no business failing over. The reason `-Werror` is
there at all is in `docs/zcc.md` — `zcc` shipped for several days with
three implicit declarations nobody read.

## 2. What goes on the card

```
/wm  /term  /repl          as now
/posix                     sw/apps/posix/posix.bin
/zcc                       sw/apps/zcc/zcc.bin
/libz/libz.bin            sw/apps/zcc/libz/libz.bin
/libz/libz.sym            sw/apps/zcc/libz/libz.sym
/include/                  sw/apps/zcc/include/*.h
/common/                   sw/common/*.h and syscalls.def
```

`tools/tftp-dist.sh` publishes all three (both apps and the two
runtime files), so `make tftp-dist` after a build is enough to fetch
them with `tget`.

Their names are 8.3-valid on purpose. They were `libz.blob` and
`libz.syms` until it turned out that FatFs here is built with
`FF_USE_LFN 0` — eight characters of base and **three** of extension —
so a four-character extension cannot be written to the card at all.
The original names were ones the machine that needs the files could
never hold.

The two `libz` files are **not** optional and are **not** the copy
inside `zcc.bin`. `zcc.bin` links libz for its own use; `libz.bin` is
what it embeds into the programs it builds. They cannot be shared,
because each lands at a different process's `0x8000_0000`. See
`docs/zcc.md`, "Where libz comes from".

`/include` and `/common` are only needed to compile programs that
include headers. A test program that includes nothing needs neither.

## 3. Memory

`zcc` asks for `Z_PROC_STACK_SIZE_HUGE`, 4MB (`sw/os/kernel.h`). With
the kernel, `wm`, `term`, `posix` and a ramdisk also resident:

- **32MB board** (Lakritz, mozart_ml1, Kölsch, ulx3s): comfortable.
- **8MB board** (sergei_ml1): expect it to be tight. Run `free` before
  starting `zcc` — `k_mem_alloc()` is first-fit over 4KB-aligned
  blocks, so a 4MB request can fail on fragmentation while 4MB is
  nominally free.
- **1MB board** (Obst): will not work and is not meant to. The
  allocation fails, `k_proc_create()` returns 0, and the app does not
  start.

**Try a 32MB board first.** A failure there is a real bug; a failure
on 8MB might only be the memory budget.

## 4. Bring-up, in order

Each step is worth confirming before the next, because a failure in an
early one will present as a confusing failure in a later one.

**a. The compiler alone, from the kernel shell.** No `posix` involved.

```
> run zcc
```

With no arguments it should print its usage and exit. If it hangs or
faults, the problem is the app itself — memory, the ZEXE image, or
libz's heap start — and nothing to do with the shell.

This step took a machine down twice, and the fix was structural
rather than local. `zcc` used to be built freestanding, with its own
linker script and a hand-written `_start` — an artefact of a
development machine that could not link a newlib app at all, not a
design decision. It is now **an ordinary Zeitlos app**: newlib's
crt0, `sw/common/riscv-app.ld`, the same startup path as `repl` and
`term`.

`zcc_start.S` and `zcc.ld` are gone. If this step still fails, the
problem is no longer anything peculiar to how zcc starts.

**b. A compile, still from the kernel shell.** `run` there passes no
arguments, so use the `/zcc.args` stopgap (`docs/zcc.md`):

```
> echo -o /hello /hello.c > /zcc.args      (or write it from a host)
> run zcc
> run hello
```

If this works, the compiler is sound on real hardware and everything
after it is integration.

**c. The shell.**

```
> run wm
> run posix
> run term
```

then in `term`, `port posix0`. You should get the banner and a `$`
prompt. **This is the least-tested code in the whole project**:
`sw/apps/posix/main.c` compiles and has never been executed. The shell
underneath it (`sh.c`, `vfs.c`) is tested, but the port protocol,
connection handling and output batching are not.

**d. The loop.**

```
$ ls
$ cat /hello.c
$ zcc -nolibz /hello.c -o /hello
$ run hello
```

`zcc` at this prompt sets the launch argument, so `/zcc.args` is no
longer involved.

**zcc's output will appear on the kernel console, not in the `term`
window.** That is expected: a spawned program writes to the UART and
the shell writes to its port, and nothing joins them yet. See
`docs/posix.md`. Keep a serial console attached for this step or the
compiler will look silent.

`sw/apps/zcc/examples/` has two programs to try, in order: `hello.c`
needs nothing on the card but itself, and `hello_libz.c` exercises the
runtime. Start with the first — if it works, the compiler, the ZEXE
image and the loader are all sound, and anything that fails afterwards
is the runtime or the shell.

## 5. Where it is most likely to go wrong

Ordered by how likely, not by how bad.

1. **`posix` output batching.** `z_port_send()` refuses once eight
   messages are unacked (`zport.h`), and `repl` learned this the hard
   way — a paste of nine characters lost its tail. `posix` is written
   batched from the start *because* of that note, but the code has
   never run. Symptom: `cat` of a real file printing only its
   beginning.
2. **`z_launch_arg_set` timing.** `z_launch_arg_take()` must be called
   early and blocks on `wm`'s reply. If `zcc` starts before `wm` is
   ready, or if the launch argument is claimed by something else
   first, `zcc` will fall back to `/zcc.args` and appear to ignore its
   command line. Symptom: `zcc foo.c` compiling whatever the file
   says instead.
3. **SD throughput.** Unknown, and Phase 0's `sdbench` still has not
   been run. A compile reads its source and every header off the card.
   If it is slow, `docs/sdcard.md` is where to look, and putting the
   sources in `/ram` is the immediate workaround.
4. **Memory on 8MB.** See above.
5. **`zcc` producing a wrong image.** Least likely of the five: the
   device compiler's output is byte-for-byte identical to the host
   compiler's for all eleven test programs, and the host compiler's
   output matches GCC's. If the same source produces a different image
   on the board, something outside the compiler is wrong — a truncated
   read, a short write, a corrupted blob.

## 6. What would be most useful to report back

- Whether step (a) runs at all, and on which board.
- `free` output before and after starting `zcc`.
- The `-v` line from a real compile: `zcc: N KB of arena used`. Under
  `sim/` that is 1,024KB for a small file with headers and 1,984KB for
  one using `zgfx.h`/`zwin.h`. If it differs on hardware, the
  difference is interesting.
- Roughly how long a compile takes. The estimate is about a second for
  a small file at 12 MIPS, and it is an estimate.
- `sdbench`, if the card is idle — it is the one Phase 0 item still
  outstanding and it affects everything above.

## See also

- `docs/zcc.md` — the compiler, and its device port
- `docs/libz.md` — the runtime blob and the two-copy question
- `docs/posix.md` — the phase plan, and what each phase did and did
  not verify
- `docs/sdcard.md` — the outstanding throughput measurement
