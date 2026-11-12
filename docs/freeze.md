# 1.0: freezing the interfaces (plan)

Status: **a plan, being prepared**, November 2026. Nothing here is
frozen yet. The system is 0.1.0
(`sw/common/zversion.h`); this is what has to be true before a version
called 1.0 can promise that **a program written for 1.0 runs on every
1.x release, on every board that has what it needs** -- the promise the
name Zeitlos makes, written down and kept by tests.

## What is frozen

Everything a program on the card depends on and does not carry inside
itself. A Zeitlos program is linked statically: the C library and
`sw/common` are inside each binary, so their code is not the interface.
What a binary does not carry is:

| | what | where it is today |
|---|---|---|
| **1. syscalls** | the 72 numbers, their arguments and results | `sw/common/syscalls.def` (append-only already), `zeitlos.h` |
| **2. the process** | the address apps run at (`0x8000_0000`, the MTU), the stack and heap sizes, the executable format, launch arguments | `riscv-app.ld`, `zexec.h` (ZEXE v1), `zwin.h` |
| **3. messages** | the envelope, every subject number, every payload's layout | `zmsg.h`, `zsubjects.h`, `zwm.h`, `zfsproto.h`, `znet.h`, `zterm.h`, `zusbproto.h`... |
| **4. hardware registers** | every register a binary touches itself -- the blitter, the line rasterizer, socctl, game mode, the palette, audio, the RTC, the virtual mouse, GPIO, T2 | `zeitlos.h`, `zsoc.h`, `zrtc.h`, `zgfx.c`, `zaudio`... (all `static inline` or library code: **inside every binary**) |
| **5. the Scheme API** | 282 names in 182 entries, and what each does | `zapi.c`, `zapi_scheme.txt`, `docs/zapi.md` |
| **6. files** | where things are (`/apps`, `/data/<app>`, `/home`, `/sys`), `zeitlos.cfg`'s keys, the formats apps share (`.zss` sheets, slide decks, draw and pix files, `.zic`, ZAR) | `zpaths.h`, `docs/config.md`, each app's doc |

`tools/gen_abi.py` already fingerprints 1-3 (24 files): any change gives
a new id, and `update` refuses a card for another flash. That is
"same", not "compatible". 1.0 needs **compatible**: 1.0's id accepted by
every 1.x.

Item 4 is the one the fingerprint does not see, and the largest. A
binary that draws with the blitter has the blitter's register map
compiled into it. Freezing the gateware's register map for those
blocks, with additions only, is part of the promise. The other way --
every register behind a syscall or a driver -- would cost the speed
that makes these machines feel fast, and is not proposed.

## The rules after 1.0

1. **Append only.** A syscall, a message subject, a Scheme procedure, a
   register, a `zeitlos.cfg` key or a file format field is added after
   the last one. It is never renumbered, never removed and never given
   a new meaning.
2. **Reserved means zero.** Every unused bit and field is written as 0
   and ignored when read. A new meaning comes with a value that 0 does
   not have (as `VMOUSE`'s TOUCH bit did: an older bitstream reads 0, a
   mouse).
3. **Sizes travel with structs.** A struct passed to the kernel or a
   core app carries its size (or a version) as its first field, so a
   1.0 program's shorter struct is still understood.
4. **Ask, don't assume.** Anything a board may lack -- colour, game
   mode, T2, sound, a network, more memory -- is discovered through the
   FEATURES registers (`z_soc_has_feature*()`), never inferred from the
   board.
5. **Deprecated is not gone.** A call that is replaced keeps working,
   marked in the docs.
6. **Provisional is labelled.** Anything not ready for the promise is
   `Z_PROVISIONAL` in its header and **(provisional)** in its doc entry:
   usable, not promised. The fingerprint leaves it out.

## Before the freeze: what to fix

Each of these would be expensive to change after 1.0, because the
change would break the promise. Found by reading the interfaces
against where the machines are going.

### Must fix

- **The screen is 640x480 in the interface.** `Z_SCREEN_W` is 640, and
  every packed coordinate -- `Z_WM_PACK_MOUSE`, `Z_WM_PACK_PLACE`, the
  virtual mouse -- is 10 bits, so 1023 at most. A panel of 800x480
  (Kaugummi), 1024x600 or 1280x720 (HDMI) is a different machine to
  every app that knows the number. **Fix**: a call that says the
  screen's size (`z_screen_size()`), coordinates of 12 bits (or 16) in
  every packed message, and apps written against the call. The
  hardware can stay 640x480 for as long as it likes; the interface
  must not say so.
  **Done in software** ([screen.md](screen.md)): `z_screen_size()` and
  `(screen-size)`, reported by the kernel (640x480; `system.screen.size`
  to test); wm's packs 12 bits, compatibly; wm, dialogs, the keyboard
  and slide moved onto it. The floor is 640x360 -- 640 wide as every
  app assumes, 16:9 within today's VRAM. The virtual mouse and the cursor stay 10 bits
  in the gateware until a board needs more -- no LUTs spent now. Some
  apps still clamp to 640x480 (listed in screen.md).
- **Time.** `z_rtc_seconds()` and `z_time_to_tm()` are unsigned 32-bit
  seconds: they run out in 2106. `z_uptime_ticks()` (732 a second) wraps
  every 68 days. **Fix**: 64-bit seconds in the interface
  (`z_time_now64()`), the RTC's register pair read as one 64-bit value,
  and a monotonic 64-bit tick. For a system called Zeitlos, 2106 is not
  far enough.
  **Done** ([rtc.md](rtc.md), "64-bit time" and "Ticks"): the 64-bit
  calls, through a syscall whose kernel keeps the high word, so no
  gateware (the RTC is set at every boot). No 64-bit tick: tick
  differences in unsigned 32 bits are right across the wrap, and the
  tree was audited for comparisons that were not.
- **The executable has no ABI level.** ZEXE v1's header has a format
  version but nothing that says which interfaces the program was built
  for. **Fix**: an ABI level in the header (`abi = 1` for 1.0), so a
  later kernel knows a 1.0 program and keeps its promises to it, and a
  1.0 kernel can say clearly that a newer program needs a newer system
  (rather than fail somewhere later).
  **Done** (below, "The ABI level").
- **Compatible, not same.** `gen_abi.py`'s id changes on any change.
  **Fix**: a frozen 1.0 baseline (the reduced text it hashes, committed)
  and a check that every later change only adds to it; `update` then
  accepts any 1.x card on any 1.x flash.
  **The tool is done** (below, "Checking a change"); the baseline and the
  failing check wait for the freeze.

### Should decide

- **The ISA.** Programs are `rv32im`. Freeze that as the baseline. A
  later core with compressed instructions or floating point is
  welcome, but a program that uses them must check FEATURES first, or
  it is not a 1.x program.
- **Colour.** The desktop is 1 bit per pixel and apps draw into it
  directly. Desktop colour and game mode are additions, behind
  features. Freeze the 1-bit desktop as what every board has, and the
  colour paths as what some do.
- **Input.** Key events use bits 0-26 now (26 is the on-screen
  keyboard's); the pointer word carries TOUCH and a wheel. Freeze the
  layouts with the rest reserved; decide now whether a key event needs
  room for a future key code page (it has 8 bits of usage).
- **Networking.** The socket interface is IPv4. Make addresses 16 bytes
  in the interface now, with IPv4 as a case of it, before anything is
  frozen around 4.
- **Limits.** 32 processes, 16 windows, a 96-byte launch argument,
  127-character names in ZAR: state each as "at least", so a later
  system may give more without breaking a program that assumed less.
- **The Scheme API.** 282 names. Go through them once for names and
  argument orders that would be regretted (the window procedures take
  640x480 coordinates, see above), mark what is provisional, and freeze
  the rest. ms is R4RS-like; say which parts of R4RS (and later RnRS)
  are promised.
- **Configuration.** `zeitlos.cfg`'s keys are an interface too: freeze
  their names and values, with unknown keys ignored.
- **The files apps share.** `.zss`, decks, draw and pix files: each
  format gets a version field and a written description before 1.0.

### Fine as it is

The message envelope, the syscall table's append-only rule, the ZEXE
header's size and flags, the FAT filesystem and its paths, UTF-8
everywhere, the key/value store's interface, the MTU's address model,
and `CORE_FILES`' places. All are already written as frozen in their
own docs.

## The ABI level

Bits 7:4 of the ZEXE header's flags ([executables.md](executables.md),
"The ABI level"): the interfaces a program was built for,
`Z_ABI_LEVEL` in `sw/common/zexec.h`, written by `mkexec.py`. It is 0
now and stays 0 until the freeze; 1.0 makes it 1. It changes only when
a release changes the interfaces in a way programs would notice --
something to avoid, a handful of times over decades if at all. Four
bits hold 0-15; past that, a new ZEXE version.

The kernel refuses a program of a higher level than its own before it
starts ("built for a newer Zeitlos"). A program of a lower level is the
promise: the kernel keeps what that level had.

## Checking a change

`tools/gen_abi.py --snapshot FILE` writes the interfaces as the
fingerprint reads them; `--compare FILE` lists, by name, what was added,
changed and removed since (a syscall, a `#define`, a struct, a
typedef), and notes a struct that only grew at its end. CI puts the
comparison with `release/abi/snapshot.txt` in every push's summary.

It **reports**; it does not fail anything while the interfaces are being
prepared. A change is not automatically a break -- a changed constant
may be one only the kernel reads -- so the call stays a person's, made
with the list in front of them. At the freeze, `release/abi/snapshot.txt`
becomes 1.0's, and a change or removal against it becomes a failure.

## Keeping the promise: tests

The emulator ([zsys.md](zsys.md)) makes the promise testable.

- **A compatibility card.** At 1.0, a card of 1.0 binaries -- every app
  in the tree, plus small test programs that call every syscall, send
  every message and use every Scheme procedure -- is kept as it is.
  Every later release boots it in `zsys` and runs it: the old binaries
  on the new kernel and core apps. A release that breaks one does not
  ship.
- **The baseline check.** `gen_abi.py --compare` against 1.0's
  snapshot: a build fails if anything in it changed rather than grew.
  (Today it reports.)
- **CI.** On every push ([ci.md](ci.md)): the software built, both
  core archives within budget, the host tests, and `zsys` headless; the
  page and Lakritz's gateware weekly. A break is found the day it is
  made.
- **The registers.** `sim/sys/test_zdev.c` already checks the devices
  register by register against the RTL; its 1.0 checks become the
  hardware half of the baseline.

## Versions

| | |
|---|---|
| 0.1.x | now: interfaces change freely; each change, a new ABI id |
| **0.9** | the freeze candidate: the "must fix" list done, everything labelled frozen or provisional, the compatibility card made. Interfaces change only to fix what 0.9 shows is wrong |
| **1.0** | the promise starts: the 1.0 baseline committed, the compatibility card kept |
| 1.x | additions only; every 1.x runs the 1.0 card |

Calling today's 0.1.0 "1.0" would make the promise before the screen
size, time and the ABI level are in the interface, and the first fix
would break it. A short way to 1.0 is better: fix the four "must fix"
items, decide the "should decide" ones, and call that 0.9.

## The work, in order

1. `z_screen_size()` and 12-bit coordinates in every packed message;
   apps off the 640x480 constant. **Started**: the call, the packs, wm,
   dialogs, the keyboard, slide; the rest of the apps to go
   ([screen.md](screen.md)). The virtual mouse stays 10 bits in the
   gateware until a board needs more.
2. 64-bit time. **Done**, without gateware and without a 64-bit tick.
3. The ABI level in ZEXE. **Done**: flags 7:4, level 0 until 1.0; the
   kernel reads it, the release manifest records it, `update` shows it.
4. `gen_abi.py --compare`. **Done, as a report**; the 1.0 baseline at
   the freeze.
5. Through every interface file: reserved bits and fields documented as
   zero, sizes or versions on the structs that need them, provisional
   marked.
6. The Scheme API reviewed, provisional marked.
7. File formats: versions and descriptions.
8. The compatibility card and 0.9. (CI: **done**, [ci.md](ci.md).)
9. Languages. **Done** in the interfaces ([i18n.md](i18n.md)); the
   remaining apps' strings are translated as they come, with no
   interface change.
