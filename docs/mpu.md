# Zeitlos Memory Protection

`rtl/mpu.v` (`wb_mpu`), plus crash reporting in the kernel.

**Goal: a bug in one app must not take down the system.** Zeitlos is a
single-user OS and every app is trusted, so this is bug containment,
not a security boundary. A wild pointer or a bad jump in an app ends
that app with a report saying what happened and where, instead of
silently corrupting another app, the kernel, or hardware state that
outlives the app.

Status: **running on mozart_ml1 hardware, enforcing**; the
[test programs](#test-programs) below were used to check it. **Universal**: `MPU` is
defined in the universal section of `rtl/boards.vh`, so every board and
every release target builds it. The kernel starts it **enforcing**: a
violation ends the app with a crash report. `mpu report` in the shell
switches to logging only.

## Summary

- Zeitlos is a **single-address-space OS**: every byte has one physical
  address anyone can use, and the MTU only gives each app a view of its
  own block at `0x8000_0000`. Borrowed, zero-copy messaging depends on
  that. So the answer is protection, not translation: an MPU, not a
  paged MMU. Messaging, the MTU and the address map are unchanged.
- **Privilege comes from where the running code was fetched.** Code
  fetched from BIOS RAM or kernel text is privileged; anything else is
  not. Nothing in software sets or clears a mode.
- **Apps enter the kernel only through a gate**: the syscall entry
  (`reg_kernel`) or the IRQ vector.
- **An app may write only its own memory**, plus the peripherals it
  needs, **execute only its own memory**, and **read anything**.
- **Crashes are reported.** Illegal instructions, misaligned accesses
  and protection faults in an app end that app with a report; the rest
  of the system carries on. A crash in kernel code stops the machine
  with a panic report instead of a silent hang.
- **Cost:** no extra cycles, roughly 450-700 logic cells, no block RAM,
  one register write per context switch. Every release target meets
  timing with it.

## Where it sits

```
                 virtual            (untranslated)
picorv32_wb ---> wb_mtu ----------+--> wb_mpu ---> wb_cache / wb_icache / (none) ---> bus
zeitlos32_wb --(translates itself)|      |  checks            |
                                  |      |                    v
          MTU base register ------+------+          the CPU's direct VRAM path
                                         +--> fault registers, IRQ 10
```

A new module on the CPU's own path, beside the MTU and **before the
cache**:

- **Before the cache**, because with `DCACHE` a store is acknowledged
  as soon as it enters the write buffer, and a load can be served from a
  cache line without touching the bus. Anything downstream would be too
  late.
- **Checks the untranslated CPU address plus the MTU base**, so its
  comparisons run alongside the MTU's adder rather than after it. An
  access through the app window (`0x8xxx_xxxx`) is inside the app's
  block exactly when its offset is below the block size; any other
  address is physical and is compared against the block directly.
  zeitlos32 translates inside the core, so its addresses are always
  physical.
- **Shares the MTU's base register** rather than keeping a copy.
- **Answers its own registers** (`0x9000_01xx`, in the MTU's nibble)
  itself; they never reach the bus.
- **Gates the CPU's direct VRAM path too**: `rtl/sysctl.v` feeds the
  VRAM arbiter from the CPU, bypassing the cache, and it now takes the
  MPU's gated signals.
- **Universal**: `MPU` is defined for every board, in the universal
  section of `rtl/boards.vh` (outside the release `ZSPEC` guard, so
  release specs do not list it). Undefined, the signals are wired
  straight through. It works with no cache, `wb_icache` or `wb_cache`.

Other bus masters (blitter, audio mixer, a future DMA engine) are not
checked; they are programmed by privileged code.

## The rules

**Privileged code** is code fetched from:

- BIOS RAM, `0x0000_0000`-`0x0000_1fff`
- kernel text, `0x4000_0000` up to `KTEXT` (the kernel's `_etext`)

It may do anything. Privilege is decided per instruction, when the
instruction is fetched: both CPUs are non-pipelined, so every load and
store belongs to the most recently fetched instruction.

**Unprivileged code** (everything else) may:

| Access | Allowed if | Blocked (enforcing) |
|---|---|---|
| fetch | inside its own block, or at the gate: `GATE` (the syscall entry, `z_kernel_entry`) or the IRQ vector `0x10` | the CPU gets `0x0000_0000`, an illegal instruction |
| load | its address nibble is enabled in `MASK` | reads 0 |
| store | inside its own block; or a nibble enabled in `MASK` that is not kernel-only (below) | dropped |

On 512MB boards (`MAIN_512MB`, [ddr3.md](ddr3.md#memory-map)) "main
memory" below means nibbles 4 AND 5: the own-block and store rules cover
both, through the `MAIN_512` parameter of `rtl/mpu.v`. Without it, stores
above `0x5000_0000` would be gated only by `MASK`, which allows nibble 5
by default.

**Kernel-only for stores**, fixed in hardware: BIOS RAM (nibble 0),
flash and its write/erase registers (nibble 1), main memory outside the
app's own block, the cache control registers (`0x7000_01xx`), the FPGA
reconfigure key (`0x7000_0218`), and the MTU/MPU registers (nibble 9).

**`MASK`** is 16 bits, one per address nibble (`0x0xxx_xxxx` to
`0xFxxx_xxxx`). The kernel sets it to `0xF7FF`: everything except the SD
card (nibble `0xB`), which only the kernel's filesystem may drive.

> **Nibble `0x6` holds both Ethernet controllers**, one 16MB slot
> each: the RMII MAC at `0x6000_0000` and the ENC28J60 SPI Ethernet at
> `0x6100_0000` (moved from `0x5000_0000` so DDR3 main memory could
> take `0x4000_0000`-`0x5fff_ffff`; see [ddr3.md](ddr3.md#memory-map)).
> Both drivers are apps (`sw/apps/net/rmii_eth.c`, `enc28j60.c`), and
> nibble 6 is in the default mask, so they share one app-accessible
> region -- while the sdcard keeps nibble `0xB` to itself. So
apps keep nearly all the access they had; the mask is there to take a
space away, or to widen what an app may use later, without new
hardware.

**The gate.** Because running kernel code grants privilege, an app bug
that jumps into the middle of the kernel (a corrupted function pointer
that happens to point there) would otherwise become privileged. Entering
kernel code from app code is allowed only at `GATE` and `0x10`.
Returning from the kernel to the app needs nothing special. The kernel
never calls into app code, which would need a second entry point for
the return.

**The MTU register is now kernel-only.** Before, any app could rewrite
the MTU base and map any memory, including the kernel's, into its own
window.

## Examples

### What it catches

- **An overrun into another app or the kernel.** An app writes past the
  end of an array at the top of its memory. Today those bytes land in
  the next app's block or in kernel memory, and something unrelated
  fails later. Now: the store is blocked, and the report names the app,
  the instruction and the address.
- **Null and wild pointer stores.** `*(uint32_t *)0 = x` used to
  overwrite BIOS RAM, where the IRQ vector lives, and the machine died
  at the next interrupt with no trace of why.
- **A receiver writing into a borrowed message.** The payload a
  receiver gets from `z_msg_read()` is a pointer into the sender's
  memory ([messaging.md](messaging.md)). Tokenizing it in place,
  lowercasing it, or calling `free()` on it (which writes allocator
  bookkeeping beside the pointer) used to corrupt the *sender*, which
  then failed somewhere unrelated. Now the receiver's store is blocked
  and the report points at the receiver.
- **Stale pointers.** A pointer into a process that has exited, whose
  memory now belongs to a new app.
- **Execution overruns.** A smashed return address or corrupted
  function pointer sending an app into another app, the kernel, or a
  peripheral.
- **Touching what outlives the app**: the reconfigure key, flash
  erase, the SD card mid-transfer, the MTU base.
- **Plain crashes** (no MPU needed): illegal instructions, `mul` on a
  bitstream without a multiplier, misaligned accesses. These used to
  hang the machine.

### What it cannot catch

- **Reads.** Any app can read anything. Borrowed messaging needs it.
- **Borrowed-message lifetime bugs.** A receiver reading a payload
  after the sender freed it still reads garbage.
- **The kernel writing on an app's behalf** -- by the MPU, that is. The
  kernel is privileged, so a bad buffer an app hands to a syscall is not
  the MPU's to stop. [Syscall pointer checks](#syscall-pointer-checks)
  cover it in software.
- **Corruption inside an app's own memory**: one heap object into
  another, the stack into the heap.
- **Kernel bugs**, and misprogrammed bus masters.
- **Hangs.** An app in an infinite loop is already handled by
  preemption.

## Crash reporting

Independent of the MPU: every build gets it.

picorv32 raises IRQ 1 for an illegal instruction (and `ebreak` and
`ecall`) and IRQ 2 for a misaligned access; the BIOS unmasks every
interrupt at boot. zeitlos32 uses the same numbers.

Both cores **retire** the faulting instruction before raising the
interrupt at the next instruction boundary, so the PC they save is the
address *after* it. Before this, `z_kernel_entry()` counted these
interrupts as "other" and returned to that PC: the faulting instruction
was silently **skipped** and the app carried on with whatever state that
left. (The comment in `k_proc_wait()` saying IRQ 1 "retries the
faulting PC" describes the same thing wrongly.) The reports therefore
show the saved PC minus 4, the faulting instruction itself. A
misaligned *jump* is the exception: there the saved PC is the bad
target, recognisable by not being word-aligned, and is shown as is.

`k_fault()` in `sw/os/kernel.c` now handles IRQ 1, 2 and 10 (the MPU)
before anything else in the interrupt path:

- **An app crashed** (its PC is in the app window, or the MPU blamed
  it): a report on the console, the process ended through the normal
  path (exit status `-128`, so a waiting shell sees it fail), and the
  scheduler switches away. It is never resumed at the faulting
  instruction.

  ```
  *** view (pid 7) crashed: store outside its own memory
      address 4001f2c0 (kernel memory)  at pc 800012a4
      ra 80000f10  sp 8003ff70  gp 80004800  a0 4001f2c0  a1 00000000
      ended; the rest of the system is unaffected
  ```

- **Kernel code crashed**, including inside a syscall: a panic report
  naming the syscall's caller if there was one, then the machine halts
  with interrupts masked. Nothing can be trusted once the kernel itself
  has faulted. The report is flushed to the serial console by polling
  (the UART's interrupt will never come), and then **drawn on the
  screen**: the last 20 rows of the console log
  ([console.md](console.md)) at the top of an otherwise cleared screen,
  the report at the bottom of them, in plain CPU stores to VRAM with the
  5x8 font -- no blitter, no `wm`, game mode switched off. A long line
  counts as the rows it wraps to, so it cannot push the report out of
  view. (It was the last 60 lines at first, filling all 480 lines of the
  framebuffer; on a real display they did not all fit.) That costs the
  kernel about 1.3 KB.

  ```
  *** KERNEL PANIC: misaligned memory access
      pc 40012a0c, during a system call from pid 7 (view)
      ra ...
      system halted
  ```

- **In report-only mode** (`mpu report`), violations print one line and
  let the app run:

  ```
  mpu: view (pid 7) would fault: store 4001f2c0 (kernel memory) at pc 800012a4
  ```

  The first 16 are printed; after that they are counted (`mpu` shows
  the count, `mpu clear` resets the limit).

Causes decoded: MPU faults (fetch/load/store, and why), `ebreak`,
`ecall`, `mul`/`div` on a bitstream without the M extension, other
illegal instructions, misaligned accesses.

Reports go to the kernel console, deliberately rather than to a dialog.
Without a serial cable they are still visible: CONSOLE in `term` shows
the console, history included ([console.md](console.md)).

A process that never registered a name (a program built with zcc, for
instance) is shown as `pid 6` alone.

## Test programs

Small programs that crash on purpose, one way each. Build with zcc on
the device and run from the `posix` shell (or build on the host and copy
them over with tftp):

```
$ zcc t1.c -o t1 && run t1
```

Each should end with a report on the kernel console and the program
gone, while `wm`, `net`, the shell and everything else keep running.
That second part is the actual test. A program built with zcc has no
registered name, so reports call it `pid N`.

### Safe with any `mpu` setting

These are CPU exceptions, reported whether or not the MPU is on.

**Illegal instruction.** Executes a zero word placed in the program's
own memory:

```c
int main(void) { unsigned int code[1]; code[0] = 0; ((void (*)(void))code)(); return 0; }
```

Expect `crashed: illegal instruction 00000000`, with the PC equal to the
address of `code` (also in `a0`).

**Breakpoint:**

```c
int main(void) { unsigned int code[1]; code[0] = 0x00100073; ((void (*)(void))code)(); return 0; }
```

Expect `crashed: breakpoint (ebreak)`.

**Misaligned load:**

```c
int main(void) { unsigned int x = *(volatile unsigned int *)0x80001001; return (int)x; }
```

Expect `crashed: misaligned memory access`.

**Syscall with a misaligned argument pointer.** Calls the syscall entry
directly (the address at `0x0000000c`, as the runtime does), asking
`UPTIME` (9) to write its result one byte into the program's own buffer:

```c
int main(void) {
	unsigned int buf[4];
	unsigned int (*k)(unsigned int, unsigned int *, unsigned int);
	k = (unsigned int (*)(unsigned int, unsigned int *, unsigned int))
		*(volatile unsigned int *)0x0000000c;
	k(9, (unsigned int *)((char *)buf + 1), 0);
	return 0;
}
```

Expect `syscall: pid N passed a misaligned pointer ...; call refused`,
and the program carries on. Before
[syscall pointer checks](#syscall-pointer-checks) this halted the
machine with a kernel panic.

**Syscall writing into kernel memory.** The same, with the argument
pointer aimed at kernel code:

```c
int main(void) {
	unsigned int (*k)(unsigned int, unsigned int *, unsigned int);
	k = (unsigned int (*)(unsigned int, unsigned int *, unsigned int))
		*(volatile unsigned int *)0x0000000c;
	k(9, (unsigned int *)0x40000100, 0);
	return 0;
}
```

Expect `syscall: pid N passed a bad pointer 40000100 (4 bytes); call
refused`. This one is safe with the MPU off too: it is the kernel's own
check, not the MPU's.

### Only while enforcing (the default)

**Do not run these after `mpu report` or `mpu off`.** Then the
access really happens: these would corrupt kernel code or BIOS RAM, and
the MPU would only say so afterwards.

**Store into kernel memory** (`0x40000100` is kernel code):

```c
int main(void) { *(volatile unsigned int *)0x40000100 = 1; return 0; }
```

Expect `crashed: store outside its own memory`,
`address 40000100 (kernel code)`.

**Null-pointer store** (BIOS RAM, where the IRQ vector lives):

```c
int main(void) { *(volatile unsigned int *)0 = 1; return 0; }
```

Expect `store outside its own memory`, `(BIOS RAM)`.

**Store past the end of its own memory:**

```c
int main(void) { *(volatile unsigned int *)0x8FFFFFF0 = 1; return 0; }
```

Expect `(past the end of its own memory)`.

**Jump into the middle of the kernel** (the gate):

```c
int main(void) { ((void (*)(void))0x40000100)(); return 0; }
```

Expect `crashed: fetch a jump into the middle of the kernel`.

**Load from the SD card's address space** (masked for apps):

```c
int main(void) { unsigned int x = *(volatile unsigned int *)0xB0000000; return (int)x; }
```

Expect `crashed: load an address space it may not use`.

### What to check

- Each report names the program, gives a PC inside it (`0x8000_xxxx`),
  and describes the address correctly.
- After each one the system is fully usable: windows, network, `run`.
- Run several back to back; `mpu` shows nothing pending afterwards (the
  handler clears each fault as it reports it).
- With `mpu off`, the first three are still caught: crash reporting does
  not depend on the MPU.
- None of these should cause a `*** KERNEL PANIC`. All of them fault in
  app code. A panic from one of them is a bug worth reporting.

## Syscall pointer checks

The MPU cannot stop the kernel writing on an app's behalf: an app that
hands a syscall an uninitialized buffer pointer gets the kernel to write
there, into another app or the kernel itself. So the kernel checks.

- **At dispatch, every syscall:** the argument pointer (many handlers
  write their results back into it) must be in the calling app's own
  memory, or NULL.
- **In every handler that writes through a pointer in its arguments:**
  file reads, reading a chunk of an open file, directory listings (the
  entries and the optional type array), config lookups (key and value),
  the process list, USB serial reads, USB networking's info and receive,
  and the console log read. Each checks pointer and length.

"In the app's own memory" means inside its block, through its window
(`0x8xxx_xxxx`) or at its physical address: `k_user_ok()` in
`sw/os/kernel.c`. A refused call fails like any other error (the app
sees `Z_FAIL`, and the handler's usual "nothing done" outputs), and the
first 16 are logged:

```
syscall: view pid 7 passed a bad pointer 00001000 (4096 bytes); call refused
```

**Alignment.** Where the kernel accesses an app's memory a word at a
time -- the argument pointer itself, the process list's array of
structures, USB networking's info structure -- the pointer must also be
word-aligned (`k_user_ok_words()`). A misaligned word access is an
exception, and in kernel code an exception is a panic: a program that
passed a syscall a pointer one byte off its own buffer used to halt the
whole machine. It is now refused like any other bad pointer:

```
syscall: pid 6 passed a misaligned pointer 80004fd9; call refused
```

**Messaging.** Receiving a message has the kernel read the *sender's*
objects: blob headers, list and map tables, their item arrays. Those
pointers are checked the same way before they are followed -- word-
aligned and inside the sender's own window (`z_sender_ok()` in
`sw/os/msg.c`) -- and an element that fails reaches the receiver as
`Z_NONE`. Otherwise a corrupted object in one app could panic the kernel
inside another app's syscall. Strings and blob data are read a byte at
a time by whoever uses them, and are not checked.

Everything is in the kernel. Apps and the runtime in `sw/common` are
unchanged: every current caller already passes its own locals or
buffers.

The caller is the running process (`z_pid`), not something recorded at
syscall entry, because syscalls other than the FatFs ones can be
preempted, and `z_pid` is what a switch saves and restores. The
kernel's own process (pid 0) runs from the kernel image, below `_end`;
apps are all above it, so the kernel calling a handler directly is never
refused.

Reads are not checked: any app may read anything (borrowed messaging
depends on it), and the kernel reading on its behalf is no different.

## Printing from interrupt context

While fixing the panic path a latent hang turned up. `k_uart_putc()`
used to block the current process when the UART's transmit buffer was
full, and wait for the UART interrupt to wake it. Inside the interrupt
handler that interrupt cannot be taken, so any `printf` from interrupt
context that found the buffer full (a crash report, the scheduler's own
messages when it cleans up a process) could hang the machine. The
interrupt path now sets `k_uart_polled`, and a full buffer drains by
polling instead, as it already did before the scheduler starts.

## Registers

At `0x9000_0100`, answered inside the MPU. Writes are privileged-only
(the MPU enforces it once enabled).

| Address | Register | |
|---|---|---|
| 0x90000100 | CTRL | bit0 enable, bit1 enforce, bit2 IRQ; reset 0 |
| 0x90000104 | KTEXT | end of kernel text (exclusive) |
| 0x90000108 | GATE | syscall entry address |
| 0x9000010C | SIZE | current app's block size (the base is the MTU's) |
| 0x90000110 | MASK | bit n: apps may use address nibble n |
| 0x90000114 | FAULT_ADDR | first violation: the address |
| 0x90000118 | FAULT_PC | first violation: its instruction's address |
| 0x9000011C | FAULT_INFO | `{valid, reason[27:24], kind[17:16], sel[15:12]}`; write to clear |
| 0x90000120 | COUNT | violations since cleared; write to clear |
| 0x90000124 | INFO | `{0x3A50, version}` |

Kind: 0 fetch, 1 load, 2 store. Reason: 1 outside own memory, 2
kernel-only address, 3 address space masked, 4 jump into the kernel
other than at the gate.

> **Check `z_mpu_present()` before writing any of these.** Without
> `MPU`, the MTU answers the whole `0x9` nibble, so a write here lands
> in the MTU base and remaps the running process. The magic's top
> nibble (3) cannot be an MTU base.

C side: `sw/common/zsoc.h` (`reg_mpu_*`, `Z_MPU_*`, `z_mpu_present()`).
`FEATURES2` bit 10 reports it in the boot inventory (`protect  mpu`).

**Kernel use:** `k_mpu_init()` at boot programs `KTEXT` (`_etext`),
`GATE` (`z_kernel_entry`), `MASK` and enables it, enforcing, with the
IRQ. The context switch writes `SIZE` next to the MTU base it already
writes.

**Shell:** `mpu` (status and the first pending violation),
`mpu report`, `mpu enforce`, `mpu off`, `mpu clear`.

## Cost

No extra cycles: the checks run alongside the cache lookup, and a
permitted access never waits for them (verified with the real CPUs in
`rtl/tb/tb_cache_soc.v`, with and without a cache). About 450-700 logic
cells and no block RAM. Every release target meets timing with it; the
fullest, `lakritz_katze`, is at 90% of its 25F.

Two design choices keep it off the critical path: the return path to the
CPU is an OR selected by registers, not by the address comparisons, and
a completed fetch updates the privilege one cycle later, so the cache's
fast-hit acknowledge only reaches one flip-flop. If a tighter board ever
needs more, the fallback is one extra cycle on uncached accesses only.

## Testing

```
iverilog -g2005 -o tb_mpu rtl/tb/tb_mpu.v rtl/mpu.v && ./tb_mpu
iverilog -g2005 -Ptb_mpu.XLATE=0 -o tb_mpu rtl/tb/tb_mpu.v rtl/mpu.v

make -C rtl/tb/cache_soc PREFIX=riscv64-unknown-elf-
iverilog -g2005 -DCACHE=2 -DBURST=1 -DMPU [-DCPU_Z32] -o tb_soc \
    rtl/tb/tb_cache_soc.v rtl/mpu.v rtl/cache_id.v rtl/cache.v \
    rtl/mem/sdram_kianv.v rtl/tb/sdram_model.v rtl/cpu/picorv32/picorv32.v \
    rtl/cpu/zeitlos32/zeitlos32.v rtl/cpu/zeitlos32/zeitlos32_muldiv.v \
    rtl/arbiter_main.v
vvp tb_soc +prog=rtl/tb/cache_soc/prog.hex +expect=2cf1c77d
```

- **`tb_mpu.v`**: a reference model, written independently of the RTL,
  decides every access; each is checked for whether it reached the
  downstream bus at all (a blocked request must never assert downstream
  strobes), what it returned, and the fault registers. Directed cases
  for every rule, then a random soak enforcing and report-only, with
  and without the MTU on the bus. Eight deliberately broken versions
  all fail it (no gate check, window off by one, reconfigure key
  unprotected, BIOS RAM writable, mask ignored, a blocked fetch
  granting privilege, apps able to write the MPU's registers, enforce
  ignored).
- **`tb_cache_soc.v -DMPU`**: the real picorv32 and zeitlos32 run the
  whole test workload under an enforcing MPU (same checksum), then the
  program acts as a kernel: it copies a three-instruction app into a
  separate block and jumps to it. The app's store into its own memory
  must land, its store into kernel memory must be blocked and recorded
  with the right address and reason, and it must get back in through
  the gate. Both CPUs pass.

The kernel's crash handler needs the whole OS, so it is tested on
hardware, with the [test programs](#test-programs).

## Bring-up

1. Flash the bitstream and kernel together. The inventory shows
   `protect  mpu`, the boot log `- mpu: enforcing`.
2. Run the [test programs](#test-programs).
3. If an app you believe is correct is ended by the MPU, the report says
   what it touched. Either it is a real bug in that app, or something
   legitimate the rules forbid; to investigate without the app being
   ended, `mpu report` logs violations and lets it run.
4. If the MPU itself seems to cause trouble, `mpu off` takes it out at
   runtime.

## Later

- **Protecting an app's own code**: needs the code size, which the
  executable header (`sw/common/zexec.h`) does not carry today. A
  version 2 header, written by `tools/mkexec.py` and zcc.
- **A stack guard** inside each app's block.
