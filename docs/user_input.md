# Zeitlos User Input Developer Guide

## Overview

Zeitlos reads keyboard and mouse input from two independent USB HID
host ports (`rtl/usb_hid.v`'s `usb_hid_wb`, two instances in
`rtl/sysctl.v`) -- Obst and Lakritz both break out two USB host ports
(`boards/*.lpf`'s `usb_host_dp[1:0]`/`usb_host_dm[1:0]` -- on the
ULX3S port 1 is a pair of header pins rather than a connector, see
[ulx3s.md](ulx3s.md)), and there's
no fixed port-to-device mapping: either port can be a keyboard, a
mouse, a gamepad, or nothing at all, and software decides which is
which at runtime, every time it needs to know.

This guide covers the whole stack, kernel up to app:

- The two USB HID host ports themselves, their register layout, and
  the hardware cursor sprite.
- `sw/os/hid.c` -- the kernel's interrupt-driven keyboard capture.
- `sw/common/zkbd.h/.c` -- USB HID usage code → keysym translation.
- `Z_WM_KEY` -- the window manager's keyboard delivery protocol (see
  also `docs/window_manager.md`, which owns the wm's app protocol as
  a whole; this document only covers the keyboard-specific parts).

Mouse input (`reg_usbN_cursor`, click hit-testing, dragging) is
covered here only where it interacts with the dual-port story --
`docs/window_manager.md` has the rest (focus, z-order, drag mechanics).

## The two USB HID ports

Each `usb_hid_wb` instance exposes four registers, word-addressed
(`wb_adr_i[2:0]` inside the module selects among them):

| Offset | Register | Contents |
|---|---|---|
| 0x00 | info | `{ report[31], 5'b0[30:26], typ[25:24], 16'b0[23:8], modifiers[7:0] }` |
| 0x04 | keys | `{ key1[31:24], key2[23:16], key3[15:8], key4[7:0] }` |
| 0x08 | mouse | `{ 8'b0[31:24], mouse_btn[23:16], mouse_dy[15:8], mouse_dx[7:0] }` |
| 0x0c | cursor | see below -- narrower than it looks |

`typ` is `0`=none, `1`=keyboard, `2`=mouse, `3`=gamepad (matching
`usb_hid_host.v`'s device-type recognition). **It's at bits
`[25:24]`, not `[23:22]`** -- easy to get wrong by two bits when
eyeballing the concatenation, and this project did, for a while (see
"Debugging notes" below).

The `cursor` register's RTL concatenation (`{ 11'd0, uhh_mouse_btn,
curs_y, curs_x }`) is actually 11+8+10+10 = 39 bits wide, assigned
into a 32-bit `wb_dat_o` -- Verilog truncates from the top when the
right-hand side is wider than the left, so the *actual* bits landing
in the register are `{ 4'b0[31:28], mouse_btn[27:20], curs_y[19:10],
curs_x[9:0] }`, not the naive 11/8/10/10 split the concatenation
looks like at a glance.

Neither port has hub or multi-device support -- each is a single-
device low-speed USB host core (`rtl/ext/usb_hid_host`), so exactly
one device can be attached per port. Neither port can turn on a
connected keyboard's Num Lock/Caps Lock LEDs either -- the host core
never issues a HID `Set_Report` (output report), so those LEDs stay
under the keyboard's own power-on state regardless of anything
software does here. Adding that would mean extending
`usb_hid_host.v` with an OUT-transfer capability it doesn't currently
have at all -- not attempted.

### Register addresses

Port 0's registers are at their original, pre-dual-port addresses
(`sw/bios/bios.c` and `sw/apps/gpu3d/gpu3d.c` both have their own
private copies of these four `#define`s and don't need updating for
any of this):

```c
reg_usb0_info   0xc0000000
reg_usb0_keys   0xc0000004
reg_usb0_mouse  0xc0000008
reg_usb0_cursor 0xc000000c

reg_usb1_info   0xc0000020
reg_usb1_keys   0xc0000024
reg_usb1_mouse  0xc0000028
reg_usb1_cursor 0xc000002c
```

Port 1 sits at `+0x20`, not `+0x10` -- `rtl/sysctl.v`'s `cs_usb0`/
`cs_usb1` discriminate on address bit 5, not bit 4. This isn't an
arbitrary choice: `wb_adr_i` (what `usb_hid_wb` actually receives) is
`wbm_adr_sel_word = wbm_adr_sel[27:2]`, a **word-shifted** address, so
byte-address bit 4 lands on `wb_adr_i[2]` -- exactly one of the three
bits (`wb_adr_i[2:0]`) the module uses internally to select among its
own four registers above. Discriminating on bit 4 made port 1's
addresses decode to an internal register-select value of `4`, which
matches none of the module's four cases -- it still acked, but never
drove `wb_dat_o`, so every port-1 register read back as stale/zero
regardless of what was actually plugged in. Bit 5 (word bit 3) sits
safely above that 3-bit field.

### Interrupts

Both ports' `report` pulses (`rtl/ext/usb_hid_host`, fired for
keyboard, mouse, *and* gamepad reports alike, gated on `typ != 0` so
there's no spurious pulse before a device enumerates) are wired
straight to CPU interrupt lines: `cpu_irq[5]` (`Z_IRQ_HID`, port 0)
and `cpu_irq[6]` (`Z_IRQ_HID1`, port 1). Both are edge-latched
(`rtl/sysctl.v`'s `LATCHED_IRQ` mask) specifically because `report` is
only a single 12MHz-domain cycle wide -- latching catches the edge in
hardware even though it's long gone by the time a slower-clocked ISR
actually gets to run. `sw/os/hid.c`'s ISRs only act on `typ==1`
(keyboard) for their own port; mouse reports are left to the hardware
cursor tracker below and polled by `wm.c`.

### Hardware cursor sprite

`rtl/gpu/gpu_cursor.v` (`GPU_CURSOR`) renders the on-screen mouse
pointer from a single `(x,y)` position, but there are now two possible
mouse sources. `rtl/sysctl.v` muxes `gpu_curs_x`/`gpu_curs_y` between
the two ports' own `curs_x`/`curs_y` outputs, picking whichever port's
own `typ` currently reads `2` (mouse), preferring port 0 if -- unusually
-- both do. An instance that isn't currently a mouse never updates its
own `curs_x`/`curs_y` (`usb_hid_wb` only moves them on a report while
`typ==2`), so if neither port is a mouse yet this just holds whatever
port 0 last had (0,0 after reset) -- the same behavior as before this
was two ports.

## Kernel: interrupt-driven keyboard capture (`sw/os/hid.c`)

`reg_usbN_info`/`reg_usbN_keys` hold the *current* USB HID
boot-protocol report for their own port -- level state, not an event
queue: up to 4 simultaneously-held non-modifier keys plus a modifier
byte, overwritten in place by hardware on every new report. Software
needs press/release *edges*, not level state, so `z_hid_irq0()`/
`z_hid_irq1()` (called from `z_kernel_entry()` on `Z_IRQ_HID`/
`Z_IRQ_HID1`) each diff their own port's new report against that same
port's own previous one -- entirely separate `hid_port_t` state per
port, since either port might be the keyboard independent of the
other -- and push one event per key/modifier that actually changed
into a single **shared** ring buffer. Apps don't care which physical
port a keystroke came from, only that it happened, so there's no
reason to expose two separate queues.

A modifier bit changing (Shift/Ctrl/Alt/Gui, either side) is
synthesized as a press/release of a pseudo "usage code" in the
`0xE0`-`0xE7` range -- these are the real USB HID usage IDs for those
keys, they just never appear in a boot report's own `key1..key4`
fields (only in the modifier byte), so this is a reasonable, spec-
aligned way to fold modifier edges into the same event shape as
ordinary keys, rather than inventing a separate event type for them.

Events are packed into a single `uint32_t`:

```
bit 0     pressed (1) / released (0)
bits 8:1  USB HID usage code
bits 16:9 modifier byte at the time of the event
```

Apps drain the queue via `hid_read_key()` (`Z_SYS_HID_READ_KEY`
syscall) -- non-blocking, returns the packed event above or `-1` if
empty, same "pop or -1" shape as `uart_getc()`. **`Z_SYS_HID_READ_KEY`
was deliberately added at the *end* of `syscalls.def`, not inserted
in the middle** -- see that file's own header comment. Inserting a
new syscall earlier in the list shifts every subsequent syscall's
enum value, and since the enum is regenerated fresh from whichever
`syscalls.def` each binary happens to be built from, a kernel and an
app built from different versions of that file would silently
disagree about what syscall ID 7 (say) even means -- not a crash, just
quietly wrong behavior anywhere past the insertion point. This is a
hard rule for this file specifically, not a style preference.

Deliberately raw at this layer: only USB HID usage codes, no ASCII or
named keys. That translation lives in app-space (`zkbd.h`, next
section) on purpose -- keyboard layout knowledge can change without a
kernel rebuild+reflash, and it keeps both ISRs small and fast.

## Keysym translation (`sw/common/zkbd.h`/`.c`)

`z_kbd_usage_to_keysym(usage, modifiers)` turns a raw USB HID usage
code (plus the report's modifier byte, for shift/ctrl resolution)
into a **keysym**:

- `0x00`-`0x7f`: ordinary ASCII, already shift/ctrl-resolved.
  Ctrl+letter returns the usual control-code mapping (Ctrl+A = `0x01`
  .. Ctrl+Z = `0x1A`), matching every other terminal convention.
- `0x100`+: named keys with no ASCII representation --
  `Z_KEY_UP`/`_DOWN`/`_LEFT`/`_RIGHT`, `_HOME`/`_END`/`_PAGEUP`/
  `_PAGEDOWN`, `_INSERT`/`_DELETE`, `_F1`..`_F12`.
- `Z_KEY_NONE`: no mapping for this usage (media keys, non-US layout
  keys), or a bare modifier press/release (`0xE0`-`0xE7`) -- those
  have no keysym of their own; a caller that wants to react to a
  modifier changing by itself should check the raw usage code before
  calling this.

US QWERTY only -- there's no layout selection mechanism yet. Letters
and the number/punctuation row are table-driven; Enter/Escape/
Backspace/Tab/Space/F-keys/nav cluster are a direct `switch`. Backspace
maps to `0x7f` (DEL), matching `zeitlos.c`'s `readline()`, which
already accepts either `CH_BS` or `CH_DEL`.

This translation happens in `wm.c` (next section), not the kernel --
see `sw/os/hid.c`'s own comment for why.

## `Z_WM_KEY`: keyboard delivery to apps

`wm` already owns turning raw input into per-app messages for the
mouse (click/focus/drag, see `docs/window_manager.md`) -- keyboard
input follows the same pattern rather than inventing a second
input-owning process. `wm.c`'s `dispatch_keys()` (called once per
main-loop iteration) drains every queued event from `hid_read_key()`
(there can be more than one since the last time `wm` got scheduled),
translates the usage code to a keysym via `zkbd.h`, and forwards it to
the **focused** window's owner only, as a packed `Z_UINT32` (`zwm.h`):

```c
#define Z_WM_KEY  106

Z_WM_PACK_KEY(keysym, modifiers, pressed)
Z_WM_UNPACK_KEY_KEYSYM(v)
Z_WM_UNPACK_KEY_MODIFIERS(v)
Z_WM_UNPACK_KEY_PRESSED(v)
```

Packed rather than a `Z_MAP`, for the same reason as `Z_WM_REDRAW`:
this can fire at high frequency (every keystroke, plus a release for
each), and a fresh heap allocation per event is more than this needs
given `wm` never frees the message objects it sends (see
`docs/messaging.md`, "borrowed data has a lifetime"). Demo windows
(owned by `wm` itself) have no app to notify, same check
`notify_moved()` already uses for mouse-driven window moves.

### Which port is the mouse?

Click hit-testing needs a cursor position, and there are now two
ports either of which might be the mouse. `wm.c`'s `mouse_port()`
picks whichever port currently reports `typ==2`, with the same
port-0-preferred tie-break the hardware cursor sprite mux uses (see
above) -- so the software click math and the on-screen pointer never
disagree about which port is "the" mouse. `get_cursor_x()`/
`get_cursor_y()`/`get_mouse_btn()` all read through this instead of a
single fixed register now.

### Focus without a mouse

Before this work, a window only ever became focused via a mouse
click -- fine when a mouse is guaranteed to be present and working,
not fine for a keyboard-only session (no mouse plugged into either
port), which would then have no way to focus *any* window and
therefore no way to receive `Z_WM_KEY` at all (`dispatch_keys()`
drops events with `focused < 0`). `handle_message()`'s
`Z_WM_CREATE_WINDOW` handler now auto-focuses a newly created window
if nothing else is focused yet -- it only fires once, so it doesn't
steal focus from an already-focused window when a second app creates
its own window later.

## Debugging notes

Two real bugs were found and fixed while bringing this up, both worth
keeping in mind if a future change to this area misbehaves in a
similar "reads back as zero/none no matter what's plugged in" way:

- **`typ` bit position.** Early versions of `sw/os/hid.c` and
  `sw/apps/wm/wm.c` read `typ` as `(info >> 22) & 0x3` -- bits
  `[23:22]`, which fall entirely inside the `info` register's constant
  `16'b0` padding field (see the register table above). This always
  read `0` ("none") regardless of what was actually connected --
  which, for the keyboard path, meant `hid.c`'s `if (typ != 1) return;`
  never passed, silently discarding every keyboard event, and for the
  mouse path, `mouse_port()` "worked" only by accident (both ports
  always read `typ==0`, never matched `==2`, so it always fell through
  to its hardcoded port-0 default). Fixed to `>> 24`, matching bits
  `[25:24]`.
- **Port 1 address/register-select collision.** Covered in full under
  "Register addresses" above -- discriminating ports on byte-address
  bit 4 collided with `usb_hid_wb`'s own internal 3-bit register
  select, since the address it actually receives is word-shifted.
  Fixed by moving the port discriminator to bit 5 and port 1's
  registers to `0xc0000020`-`0xc000002c`.

Also **not** specific to input, but found during this work and worth
noting here since it caused input-adjacent symptoms (garbled window
state, unpredictable behavior that looked like memory corruption):
every app Makefile in this project (`wm`, `hello_win`, and, at time of
writing, every other app + the kernel itself) links without
`-march`/`-mabi`, even though each one's own `CFLAGS` correctly sets
them for *compiling*. GCC also uses those flags at *link* time to
choose which multilib's `libc`/`libgcc` to search -- omitting them
makes the link step silently fall back to the toolchain's default
multilib, which may assume the M (multiply/divide) extension the
actual PicoRV32 configuration here doesn't have. Confirmed concretely:
the kernel binary contained 103 real `mul`/`div`/`rem` instructions
(all from newlib internals like `calloc`/`vfprintf`, not
project code) before adding `-march=$(ARCH) -mabi=ilp32` to
`LDFLAGS` fixed it to zero. Fixed so far in `sw/os/Makefile`,
`sw/apps/wm/Makefile`, `sw/apps/hello_win/Makefile` -- every other
app's Makefile has the identical vulnerable pattern and needs the same
one-line fix.

## Known limitations / future work

- **No hub/multi-device support per port.** Exactly one device per
  physical port; a USB hub plugged into either port isn't expected to
  work.
- **No keyboard LED control.** See "The two USB HID ports" above --
  would need real USB HID output-report support added to
  `usb_hid_host.v`, not attempted.
- **No keyboard-driven focus switching.** Auto-focus (above) gets a
  single app a window without a mouse, but there's no keyboard
  equivalent of clicking a different window (an Alt+Tab-style switch)
  once more than one app has a window open.
- **Debug instrumentation left in place, intentionally.** `wm.c`'s
  per-port `usb port N device type ->` print, the `click port=...`
  line, and `repair_region`'s fill/draw logging are all still present
  -- useful for watching device enumeration and click routing live on
  the UART console while this area is still under active development.
  Worth trimming once this whole subsystem (and `term`, built on top
  of it) is more settled.
- **`LDFLAGS` fix not yet applied project-wide.** See "Debugging
  notes" above -- `ping`, `pong`, `blinky`, `gpu3d`, `gpudemo`,
  `bounce`, `bounceblit`, `hello`, and `net` all still link without
  `-march`/`-mabi`.

## Cursor precision

The hardware cursor position (`curs_x`/`curs_y` in `rtl/usb_hid.v`) is
updated by adding the mouse's HID delta once per report. Getting that
"once" right is less obvious than it looks.

`usb_hid_host` emits `report` as a **one-cycle pulse in its own
`usbclk` domain** — 12 MHz (`clk12mhz`, `sysctl.v`). The block that
consumes it runs on `wb_clk_i`, which is `sys_clk` at 48 MHz. Four
times faster. Sampling the pulse with a plain `if (uhh_report)`
therefore saw it high across four consecutive wishbone edges and
applied the same delta on every one of them, so **one count of physical
mouse movement moved the pointer four to five pixels**.

That presented as a sensitivity problem — the cursor felt jumpy and it
was hard to land on an 8x8 titlebar icon — rather than as the missing
clock-domain crossing it was. Worth remembering as a shape: a pulse
generated in a slower domain and consumed in a faster one is repeated,
not lost, and the symptom is a multiplied effect rather than a missing
one.

The fix is two flops to resolve metastability plus a third for rising-
edge detection, giving exactly one update per report regardless of the
clock ratio. The deltas themselves are captured continuously one cycle
behind, because `usb_hid_host` clears `mouse_dx`/`mouse_dy` on the same
`usbclk` edge that drops the pulse — the valid window is only those
four wishbone cycles, and the synchroniser eats two or three of them.

### Pointer acceleration

With the multiply-by-four gone the pointer is 1:1 — one HID count, one
pixel — which is the finest this hardware can resolve and also four
times slower than it used to be. `USB_HID_ACCEL` (top of
`rtl/usb_hid.v`) doubles deltas above `USB_HID_ACCEL_THRESHOLD` so that
small, aiming movements keep full precision while a sweep across the
screen stays quick. Undefine it for a strictly linear pointer, raise
the threshold to make it engage later, or change the `<<< 1` in the
module body to `<<< 2` for a steeper curve.

There is no software-side sensitivity control, and no sub-pixel
accumulator: 1:1 is already the finest the 10-bit cursor registers can
express, so precision below that would need fractional position bits
rather than a different curve.
