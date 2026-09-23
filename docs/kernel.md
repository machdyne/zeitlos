# The kernel

`sw/os`. Processes, the scheduler, memory, and the syscall boundary.

This file is the entry point for the kernel's own design; the
subsystems it owns have their own documents and are linked from here
rather than duplicated:

| | |
|---|---|
| `docs/messaging.md` | mailboxes, `z_msg_t`, payload lifetime |
| `docs/filesystem.md` | FatFs, the syscall API, preempt deferral |
| `docs/app_runtime.md` | the app side of the boundary, and its traps |
| `docs/executables.md` | ZEXE, and how a process image is loaded |
| `docs/boot.md` | the memory budget everything sits in |
| `docs/ports.md` | the connection protocol built on messaging |

## Processes

A process is a contiguous block of main memory, a register file, and a
mailbox. There is no MMU: the MTU (`rtl/mtu.v`) translates the single
window at `0x8000_0000` to whichever physical block the running process
owns, and every other address passes through untranslated.

Three consequences worth stating together, because each is obvious
alone and the combination is what actually governs:

- **Every app is linked at the same address**, so an executable needs
  no relocation and the loader is a `memcpy` (`docs/executables.md`).
- **A process can reach the kernel and the peripherals directly.** A
  wild pointer below `0x8000_0000` is not a fault, it is a write. The
  isolation is a courtesy, not a boundary.
- **A second bus master does not get the translation.** The blitter
  takes physical addresses and says so in its own header; anything
  else that becomes a master inherits that.

### The process table

`z_proc` (`sw/os/kernel.h`): base, size, flags, wake tick, CPU ticks,
32 saved registers, and the GPU scissor. **188 bytes.**
`z_procs[Z_PROCS_MAX]` lives in kernel `.bss` with an explicit
`__attribute__((section(".bss")))` -- see "The `gp` hazard" below for
why that annotation is load-bearing rather than decorative.

### The scissor is part of the context

The GPU's clip registers -- the rasterizer's
`gpu_clip_{x0,y0,x1,y1,enable}` and the blitter's
`gpu_blit_clip_{x0,y0,x1,y1}` -- are global hardware with no privilege
attached, so an app writes them directly and the kernel cannot
intercept that. They are also *per drawing*, and a context switch can
land between a process setting its clip and using it.

What the kernel can do is what it already does for registers: read them
on the way out and put them back on the way in.
`k_gpu_clip_save()`/`k_gpu_clip_restore()` do exactly that, from
`k_sched_switch()`, and `gpu_clip_valid` is 0 until a process has been
switched away from once -- a brand-new process has never written the
GPU, so it is restored to the hardware reset (raster clip off, blitter
scissor the full screen) rather than to another process's rectangle.

**Restoring a different clip has to wait for the engines.** The
rasterizer samples the clip live, per pixel, because FIFO entries do
not carry it, and a glyph blit samples the vertical scissor live per
row -- so changing either while such an operation is in flight changes
the operation. Fills latch their clip at the start and are safe. Both
spins are bounded, because this runs inside the interrupt handler,
where an unbounded wait is unrecoverable and a `printf` is a hang (see
`sw/os/uart.c`'s `_write()`).

It costs 40 bytes per process slot, which at `Z_PROCS_MAX` of 32 is
1,280 bytes of `.bss` -- and therefore 1,280 bytes of the 256KB flash
image, for the reason "The 256KB image budget" below explains.

### Memory for a process

`k_proc_create(size, stack_size)` takes one allocation of
`image + stack_size` from `k_mem_alloc()`. There is no separate heap:
`_sbrk()` (`sw/common/zeitlos.c`) grows the heap up from `_end` and
refuses to pass the stack pointer, so an app's heap and stack share the
`stack_size` allowance and eat toward each other.

Stack sizes are per-app, chosen by name in `z_proc_stack_size_for()`.
That function is the policy and `sw/os/kernel.h` carries the reasoning
for each tier; the important one is `Z_PROC_STACK_SIZE_HUGE` (4MB, for
`zcc` and `posix`), which is a different KIND of tier from the others
and explains itself there.

**Allocation failure is a clean refusal.** `k_mem_alloc()` returns
NULL, `k_proc_create()` returns 0, and every caller treats 0 as "did
not start". That path was not always trustworthy -- see the `Z_FAIL`
bug in `docs/app_runtime.md` -- and two things now lean on it
deliberately: the 4MB tier on a small board, and any future dynamic
per-process allocation.

## The scheduler

Round-robin over runnable processes, driven by the KTIMER interrupt at
`Z_TICK_HZ` (~732Hz at 48MHz). `Z_PROC_RUNNABLE()` is "active and not
blocked".

A process blocks by calling `z_proc_wait(ticks)`, and is woken either
by the tick deadline or by a message arriving (`k_proc_unblock()`,
called from `msg.c` on every delivery).

**Blocking yields on the spot.** It used to not: the caller kept
whatever remained of its timeslice and spun in its own loop until the
next KTIMER tick noticed it was blocked. One partial timeslice per
block is a small waste in isolation, and a large one for a process that
blocks and wakes many times a second, which is what every well-behaved
app in this system now does.

The obstacle was the syscall boundary. A syscall is a plain `jalr` with
no register save (see "The syscall boundary" below), so the kernel
cannot switch away from inside one -- there is no saved frame to come
back to. Writing one would mean a second context-switch path to keep in
step with `irq_vec`, for about 800-1200 cycles per wait.

So `k_proc_yield_blocked()` uses the path that already exists: after
marking itself BLOCKED it fires picorv32's `timer` instruction with a
count of 1, which raises IRQ 0 as soon as the syscall returns. `irq_vec`
saves all 32 registers exactly as it does for any other interrupt, and
`k_sched_switch()` -- extracted from the KTIMER path for this, so there
is still only one switch implementation -- sees the caller is not
runnable and picks somebody else. If nobody else is runnable it
`waitirq()`s instead, sleeping the core until any interrupt rather than
spinning.

That needs `ENABLE_IRQ_TIMER=1` in the bitstream (`rtl/sysctl.v`);
without it the instruction is illegal, which raises IRQ 1 and retries
the faulting PC. A UART-THRE pulse was tried first, so that the
bitstream would not have to change, and does not work: writing
`IER.THRE` is a no-op when THRE is already enabled, which is the
common case during `printf`, and the `waitirq` in that hole froze every
runnable process until the next interrupt.

`k_sched_switch()` deliberately does **not** advance `z_kernel_ticks`
or sweep `wake_tick`; those stay the KTIMER caller's job. A yield must
not look like time passing.

**Blocking rather than spinning is worth more than it looks.**
`docs/app_runtime.md` records a `view` JPEG decode going from 7.7s to
3.1s once the other resident processes stopped busy-waiting. On a
single core with no idle state, one spinning process is a tax on every
other one.

`cpu_ticks` is the only CPU-time measurement in the system: the timer
handler increments the counter of a process once per tick. Sampled
accounting, so work that starts and finishes between two ticks is
invisible.

**Which process, exactly, is not obvious.** It used to be "whoever was
interrupted", which charges idle time as work: a process sitting in
`waitirq` is the one the timer interrupts, so the moment `wm` also went
to sleep, `net` inherited every idle tick in the system and `ps` showed
it busy. The charge now happens *after* the wake sweep and only if the
process is `Z_PROC_RUNNABLE()` -- a blocked waiter is idle and is
billed nothing. A process looping on `z_proc_wait(1)` is still billed,
correctly: the sweep unblocks it first.

`z_irq_census[]` counts interrupt entries by source line, reported at
the end of `k_proc_dump()`. It is measurement only, and the reason it
is worth the nine counters is that `cpu_ticks` cannot distinguish a
process doing work from a process being woken constantly by something
that has nothing to say -- two `ps` runs bracketing a workload give the
interrupt rate by source, which does.

## Memory

One pool at `Z_MEM_BASE` (`0x4000_0000`), sized from the capability
CSRs at boot (`docs/csrs.md`) with `Z_MEM_SIZE_DEFAULT` as the
pre-CSR fallback. First-fit over a linked list of blocks.

| constant | value | |
|---|---|---|
| `Z_MEM_ALIGNMENT` | 4096 | every allocation is page-aligned |
| `Z_MEM_MIN_BLOCK_SIZE` | 32768 | every allocation is rounded up to this |
| `Z_MEM_MAX_BLOCKS` | 256 | `mem_blocks[]` is a fixed array; `alloc_metadata()` returns NULL when full |

The ramdisk takes a SHARE of the pool rather than a fixed size
(`Z_RAMDISK_DIVISOR`, an eighth, capped at 4MB) -- scratch space should
be proportional to the machine, and a figure that suits 32MB is most of
1MB. That instinct is worth reusing whenever something new wants a
fixed budget.

## The syscall boundary

Not a trap. The kernel installs a function pointer at `0x0000_000c`
(`reg_kernel`) and an app calls through it: a plain `jalr` from the
app's own code into kernel code, with the app's `gp` still in place.

### The `gp` hazard

Because a syscall is an ordinary call, kernel code reached that way
executes with **the calling app's `gp`**, not the kernel's. `gp` is not
part of the C ABI's caller/callee-saved convention, so nothing restores
it.

Any kernel global reached `gp`-relative therefore reads the wrong
address. The mitigation is the explicit `__attribute__((section(".bss")))`
on `z_procs[]`, `z_mailboxes[]`, `block_list` and `mem_blocks[]` --
which forces absolute addressing.

`docs/messaging.md` and `sw/os/mem.c` both carry the chase; it took
three separate bugs to identify, and it does not announce itself -- a
global read through the wrong `gp` returns another process's memory
without complaint.

The same hazard, from the other side, is why `libz` is built
`-mno-relax -msmall-data-limit=0` (`docs/libz.md`).

### Preempt deferral

FatFs is not reentrant, so a process inside a filesystem syscall is not
preempted (`k_no_preempt`, capped at `K_NO_PREEMPT_MAX_TICKS`, 64
ticks / ~87ms). See `docs/filesystem.md`.

That cap is a real one, not a formality: past it the deferral gives up
and the switch happens anyway, which is the safe direction but means
the protection stops. A syscall that could exceed it should use the
chunked API instead of a whole-file call.

## The 256KB image budget

**Every byte of kernel `.bss` costs a byte of flash image.** This is
the least obvious constraint in the system and it is easy to walk into.

`sw/os/Makefile` links `kernel.bin` with `objcopy --pad-to=_end`, so
`.bss` is present in the file as zeros rather than stopping at
`_edata`. That is deliberate and load-bearing: the BIOS copies a
**fixed** 256KB from flash (`ROM_OS_SIZE`, `sw/bios/bios.c`) into main
memory, and **that copy is what zeroes `.bss`**. Stop padding and the
kernel starts with `.bss` full of whatever sits in flash after the
image.

So:

| | |
|---|---|
| flash layout | kernel at `0x100000`, 256KB, core-app archive (`Z_ZAR_FLASH_OFFSET`) immediately after at `0x140000`; the ZAR's room ends at `0x1D0000`, where the jumploader is (`docs/zboot.md` section 5) |
| the limit | `_end` must be under 256KB |
| what spends it | text, data, **and every static array in the kernel** |
| failure mode | the BIOS truncates; the kernel boots and then misbehaves |

That last row is why `sw/os/Makefile` now **fails the build** if
`kernel.bin` exceeds `KERNEL_MAX_SIZE`, prints the free figure when it
does not, and lists the largest symbols when headroom drops below
`KERNEL_WARN_FREE`.

```
make -C sw/os kernel-size
```

gives `text`/`data`/`bss` and the twelve largest symbols. **Use it
rather than reasoning about struct sizes.** Two attempts to predict
this from arithmetic gave two wrong answers: the struct sizes were
right both times (`z_msg_envelope_t` is 24 bytes, a mailbox 780, and a
`z_proc` was 148 then and is 188 now) and the totals were not, which
means the model was missing something the model could not see.

The build also no longer runs `strip` on `kernel.elf` in place. It
could never have affected the output -- `objcopy -O binary` emits
allocated section contents and nothing else -- and what it did do was
delete the only record of what is in the image, which is exactly what
is needed when the image is too big. Raising `Z_PROCS_MAX` to 64 put the image at 269,784 bytes
-- 7,640 over -- and it was found by flashing a board. A link-time
error is the right place for that; a kernel that boots and then
misbehaves is the worst one.

### Where the space actually went

Measured with `make -C sw/os kernel-size`, which is why it exists:

```
   text    data     bss     dec
 200164    2668   50964  253796
```

(Those are the figures from the round that found this. After the
`sscanf`, printf and padding fixes below, and with the USB ethernet
driver added, `kernel.bin` is 210,292 bytes with 51,852 free.)

`.bss` is 51KB and `z_mailboxes` is 24,960 of it -- exactly the
32 x 780 the arithmetic predicted. **`.bss` was never the problem.
`.text` was**, and about 80KB of that 200KB is newlib's formatting
machinery:

| symbol | bytes | |
|---|---|---|
| `_vfprintf_r` | 13,708 | float-capable printf |
| `_svfprintf_r` | 13,440 | ...and again, for the string variants |
| `categories` | 13,788 | locale tables |
| `__ssvfscanf_r` | 8,396 | scanf |
| `d02f4`, `b02cf` | 12,710 | decimal power tables for float conversion |
| `_strtod_l` | 6,532 | string to double |
| `_dtoa_r` | 6,460 | double to string |
| `_vfiprintf_r` | 6,620 | integer-only printf |

**This is the trap `docs/app_runtime.md` documents for apps**, in the
one place nobody had applied it. Its standing advice -- format by hand
rather than call printf -- was written about a 100KB cost in an 8KB
app, and the same cost had been sitting in the kernel unnoticed because
nothing reported it.

#### Fixed: three `sscanf` calls

`sh.c` parsed an address and a pid with `sscanf(arg, "%lx")` and
`sscanf(arg, "%ld")`. A generic `scanf` has to be able to handle `%f`,
so those three calls pulled in `__ssvfscanf_r`, `_strtod_l`, the locale
tables and the decimal power tables -- on the order of 40KB for parsing
two integers.

Replaced with `parse_uint()`, twenty lines. **Rebuild and re-run
`kernel-size` to see what it actually returned** rather than trusting
the estimate; the estimates in this document have a poor record.

#### Fixed: integer-only printf

`_vfprintf_r` and `_svfprintf_r` are the float-capable formatters, and
a formatter that can print a double drags in `_dtoa_r`, the
multi-precision helpers and libgcc's soft double arithmetic behind it.
Nothing in `sw/os` formats a float: there is no `%f`, `%g`, `%e` or
`%a` in any format string it links, and the one `%g` in
`../common/zobj.c` (`z_obj_print`) is in a function section GC already
drops from this kernel.

`sw/os/kruntime.c` now defines `printf`, `snprintf` and `vsnprintf`
itself, forwarding to `viprintf` / `vsniprintf` -- the integer-only
engine, which was already linked because `assert` uses it. The libc
members holding the full versions are then never pulled in. **Measured:
kernel.bin 256,224 -> 210,260 bytes, 45,964 saved.**

Defining the three functions rather than `#define`-ing the names keeps
gcc's `-Wformat` checking working at every call site. It is guarded
`#ifndef __PICOLIBC__`, because picolibc (`sw/common/arch.mk`, when the
newlib prefix is absent) does not name the integer variants the same
way.

**The trap this leaves:** a `%f` added to kernel code later compiles,
`-Wformat` accepts it, and it prints nothing useful. Print fixed point
instead, as `fs/sdbench.c` does ("cycles per byte, x100, so one decimal
prints without floating point").

#### Fixed: a page of linker padding

`../common/riscv-os.ld`'s `DATA_SEGMENT_ALIGN` puts the data segment on
a fresh page, or at the same offset in the next page, whichever the
linker thinks is cheaper -- so depending on where `.rodata` happens to
end, up to 4096 bytes of zeros sit between text and data **in the
image**. That is right for an ELF a loader maps with an MMU, and pure
waste for a flat image the BIOS copies to `0x40000000` with no MMU.

It was found the hard way: a 5KB code change made `kernel.bin` grow by
9KB, and the extra 4KB was this gap crossing a boundary. `sw/os/Makefile`
now links with `-z max-page-size=16 -z common-page-size=16`
(`SEG_PAGE_LDFLAGS`). Every section keeps its own alignment; only the
page-sized rounding between segments goes.

### If more room is ever needed

In order of how much they disturb:

1. **`Z_MAILBOX_DEPTH`** (`sw/common/zmsg.h`) is 84% of a process
   slot's cost -- 780 bytes against `z_procs[]`'s 148. Halving it to 16
   halves that.
2. **A per-board `Z_PROCS_MAX`**, which needs a board define in the
   kernel build; `sw/os/Makefile` passes only `ARCH_DEFS` today.
3. **Moving the flash layout** -- `ROM_OS_SIZE` and
   `Z_ZAR_FLASH_OFFSET` together. This moves the core-app archive, so a
   board flashed with the old layout and a new BIOS finds neither. It
   is the only option that actually creates space rather than
   redistributing it.

**Not on the list: dropping the padding.** It is what zeroes `.bss`.

## Exit status, and the exit ring

`z_exit()`'s argument used to be **discarded**. It took a `z_obj_t`,
called `k_proc_kill()`, and dropped it -- so every exit status in the
system was thrown away at the first step, and `sw/apps/zcc`'s entry
stub packed one carefully for nobody.

Two things needed it, both in `docs/posix.md`'s Phase 5: a shell cannot
make `a && b` mean anything without a status (the best it can do is
report whether `a` *started*), and it cannot hand the terminal to a
full-screen program and take it back without knowing when that program
ended.

### Why a ring rather than a zombie

The textbook answer is to keep the process slot until someone reaps it.
That leaks here: **nothing in this system is obliged to reap anything**,
so an unreaped child holds a slot forever -- and the shell that forgets
to ask is exactly the shell that will run many children.

So `k_exit_ring[]` holds the last 16 `(pid, status)` pairs.
`Z_SYS_PROC_STATUS` answers RUNNING from the process table, EXITED from
the ring, and UNKNOWN when it finds neither.

A ring cannot leak. It can only forget, and it forgets oldest-first,
which is the order nobody is still waiting on. Sixteen because a shell
asks within a few hundred milliseconds and nothing else asks at all; if
something ever misses, the symptom is UNKNOWN rather than a wrong
answer.

**UNKNOWN is a successful reply, not a failure.** "I do not know" is
information, and a caller that got `Z_FAIL` could not tell it from a
malformed call. A shell treats it the same as EXITED with status 0,
which is what it assumed about every child before there was a status at
all.

An entry for a pid already in the ring is replaced rather than
duplicated: pids are reused, and a stale entry would answer a question
about the NEW process with the OLD one's status -- worse than UNKNOWN.

## The limits, and raising them

`docs/posix.md`'s Phase 5 wants pipes as processes, which is what
prompted looking at these.

| | value | |
|---|---|---|
| `Z_PROCS_MAX` | **32** (was 16) | raised; see below |
| `Z_MAILBOX_DEPTH` | 32 | 24 bytes per envelope per slot |
| `Z_MEM_MIN_BLOCK_SIZE` | 32KB | not yet changed; see below |

### `Z_PIDREG_MAX` 32 -> 64 -- DONE, and it was nearly missed

Raising `Z_PROCS_MAX` had a consequence one layer up that the first
pass did not consider: `Z_PIDREG_MAX` (`sw/os/pidreg.h`) is a total
across every process and every NAME, and it was 32 -- equal to the old
process count, and already tight, because a process may register more
than one name and nine apps in this tree register at least one.

At 32 processes the registry, not the process table, would have become
the binding limit. The failure is quiet: `z_pid_register()` returns
false, the app runs on, and nothing can find it by name -- `term`
cannot open a port to a provider that never registered, and `posix`
cannot hand a terminal to a child whose name does not resolve.

64 entries, about 2KB.

**A limit raised in one place tends to move the binding constraint
somewhere else.** Worth looking one layer out each time.

### `Z_PROCS_MAX` 16 -> 32 -- DONE (64 was tried and did not fit)

Two arrays scale with it, 968 bytes a slot between them:

| | per slot | x16 | x32 | x64 |
|---|---|---|---|---|
| `z_procs[]` | 188 B | 2.9 KB | 5.9 KB | 11.8 KB |
| `z_mailboxes[]` | 780 B | 12.2 KB | 24.4 KB | 48.8 KB |
| | | 15.1 KB | **30.3 KB** | 60.5 KB |

64 was the first attempt and it overran the image budget above by
7,640 bytes, when a slot was 148 bytes rather than 188 -- the saved
GPU scissor has since added 40 to it, so the same attempt would now
overrun by about 10,200. **32 costs 15.1KB more than 16** and leaves
comfortable headroom -- and it is more than Phase 5 needs, which is one
process per pipeline stage against about a dozen resident.

The `.bss` figure is a RAM cost on every board and a FLASH cost on
every board, which is the part that was missed the first time.

The scheduler scans the table each tick, so the scan grows from 16 to
32 entries at 732Hz -- a few hundred instructions a second.

Raised **flat, not per-board**. The kernel is not
currently built with a board define (`sw/os/Makefile` passes only
`ARCH_DEFS`), so per-board scaling would mean adding that machinery for
a number whose real limit is memory -- and memory is already
board-scaled.

### Mailboxes stay static

An earlier draft of this proposed allocating mailboxes from the pool at
process creation, on the grounds that they were ~7.2KB each and 112KB
in total.

**That was wrong by a factor of nine, and the mistake is worth
recording.** `z_msg_t` is 216 bytes, and it is the APP-side structure:
it carries `_tables[4]` and `_items[16]` inline as scratch space for
resolving `Z_LIST`/`Z_MAP` payloads on receipt. The kernel's ring holds
`z_msg_envelope_t`, which is `to`/`from`/`subject`/`tag`/`obj` and
**24 bytes**. `zmsg.h` says so plainly next to `Z_MAILBOX_DEPTH`.

So a mailbox is 780 bytes and all sixteen are 12KB, not 112KB. There is
nothing there worth trading an invariant for.

And the invariant is a real one. `docs/messaging.md` states it as **"no
dynamic allocation in the kernel for messaging"**, and the incident
behind that rule is on record in the same file: `wm` built a five-key
`Z_MAP` per window move, 384 bytes at a time against an 8KB budget;
about twenty moves exhausted the heap, `z_obj_map()` wrote through the
NULL that `malloc()` returned, and the machine went down with nothing
in the symptom pointing at either the leak or the allocator.

That rule is really about the per-message path, which a
per-process mailbox allocation would not have touched. But the honest
reading is the other way round: the reason to keep mailboxes static was
never mainly the hot path, it is that **the kernel's own bookkeeping
should not be able to fail at a moment when reporting the failure needs
the thing that failed.** 43KB is a cheap price for that, and the
argument for spending it evaporated once the arithmetic was right.

### `Z_MEM_MIN_BLOCK_SIZE` 32KB -> 4KB is the one with a real risk

Every allocation is rounded up to 32KB, so a small filter -- 8KB of
stack, a few KB of image -- costs 32KB and `ls | grep | wc` costs 96KB
before doing any work. At 4KB (which is already `Z_MEM_ALIGNMENT`, so
no new rounding rule) the same pipeline costs about 48KB.

`Z_MEM_MAX_BLOCKS` has to rise with it, or the block table becomes the
new limit. 1024 entries is 16KB of `.bss` against today's 4KB.

**This is the change to be least confident about.** A smaller minimum
means more, smaller blocks and more external fragmentation, and
`k_mem_alloc()` is first-fit. `zcc`'s 4MB request is exactly what
suffers: it needs one contiguous run, and a shell that has started and
exited fifty small processes is the situation most likely to deny it.

So it should land **with a measurement**. `free` (`k_mem_dump()`)
already reports fragmentation: run a realistic session -- several
pipelines, then `zcc` on a real file -- before and after.

If it does bite, the answer is not to revert but to place small
requests from one end of the pool and large ones from the other, so
compiler-sized runs stay contiguous. Bigger change; not to be done
pre-emptively.

### Order

1. ~~`Z_PROCS_MAX` to 32~~ -- **done**. 14.5KB more `.bss`, which is
   also 14.5KB more flash image; see "The 256KB image budget".
2. Block size and count, with `free` output either side. **Not done**;
   this is the one that could regress `zcc`.

## See also

Beyond the subsystem documents listed at the top:

- `docs/csrs.md` -- how the kernel learns the machine's real memory size
- `docs/icache.md` -- why instruction fetch dominates CPI here
- `docs/console.md` -- the console log ring every console byte is
  recorded in (`k_uart_putc()`, `kprint()`), and the two syscalls that
  let `sw/apps/console` show it in a term window
- `docs/mpu.md` -- crash reports (`k_fault()`), the kernel panic, and
  the memory protection unit the kernel programs at boot and on every
  context switch
- `docs/dcache.md` -- the data cache the kernel enables at boot, and
  why no kernel code needs to flush or invalidate it
- `docs/sdcard.md` -- the SD path, and the bus behaviour a stalling
  peripheral has against the arbiter

## Reboot

`Z_SYS_REBOOT` (`k_reboot`, `z_reboot()` for apps; `reboot` in the
serial shell and in `posix`) syncs every open write handle of every
process, then reconfigures the FPGA through `rtl/socctl.v`'s RECONFIG
register and the PROGRAMN pin. On a board whose gateware cannot pull
PROGRAMN it fails and changes nothing. `docs/zboot.md` section 6.

`Z_SYS_JUMP` (`k_jump`, `z_jump()`; `jump` in the serial shell and in
`posix`) does the same from any flash address, through the jumploader
at `0x1D0000`, which `sw/os/jumpapi.c` re-points in place; `reboot` is
a jump to 0. `k_boot_to()` in `kernel.c` is both, and lists why either
refuses. `docs/zboot.md` section 5.

## Writing the flash

`Z_SYS_FLASH` (`sw/os/flashapi.c`; apps use `sw/common/zflash.h`)
erases and programs the configuration flash through `rtl/spiflash.v`:
one process at a time holds a write session, released when it exits,
and no core app is launched from flash while one is open. Nothing below
`0x040000` can be written, which the controller enforces in hardware.
The serial shell has `flash` (ID, size, lock, status) and `flashtest`
(the on-board test). `docs/spiflash.md`.
