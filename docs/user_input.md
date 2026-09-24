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

> **The host core underneath this is being replaced.**
> `rtl/ext/usb_hid_host` is a low-speed (1.5 Mbps) HID-only core, and
> USB 2.0 does not define bulk transfers at low speed at all -- so it
> can never reach a USB stick. [usb_host.md](usb_host.md) is the design
> for a full-speed Zeitlos-native replacement supporting HID, hubs,
> mass storage and CDC. **Nothing in that document is built yet**, and
> everything in *this* document describes what ships today and stays
> accurate: the replacement keeps the `reg_usbN_*` registers and the
> interrupt behaviour below bit-for-bit, specifically so that this
> guide, `sw/os/hid.c`, `sw/apps/wm/wm.c` and `sw/apps/gpu3d/gpu3d.c`
> need no changes.

This guide covers the whole stack, kernel up to app:

- The two USB HID host ports themselves, their register layout, and
  the hardware cursor sprite.
- `sw/os/hid.c` -- the kernel's interrupt-driven keyboard capture.
- `sw/common/zkbd.h/.c` -- USB HID usage code → keysym translation.
- `Z_WM_KEY` -- the window manager's keyboard delivery protocol (see
  also `docs/window_manager.md`, which owns the wm's app protocol as
  a whole; this document only covers the keyboard-specific parts).
- How the reader is woken, and what happens to input that arrives with
  no interrupt behind it -- a keystroke or a pointer position handed
  to the machine by another process rather than by a HID controller.

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
| 0x08 | mouse | `{ wheel_acc[31:24], mouse_btn[23:16], mouse_dy[15:8], mouse_dx[7:0] }` -- `wheel_acc` with the current core only; 0 with `usb_hid_host` |
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

Both limitations are gone with the replacement core,
[usb_host.md](usb_host.md) (`rtl/usb/`), which is what bitstreams
built with `USB_HOST` use: hubs are a software class driver (phase 4
there, working on hardware), and the lock LEDs are an ordinary
`SET_REPORT` -- see "Lock keys and keyboard LEDs" below. Everything in
this paragraph and the one above describes the older
`rtl/ext/usb_hid_host` core only.

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
actually gets to run. `sw/os/hid.c`'s ISRs only *queue* events for
`typ==1` (keyboard) on their own port; mouse reports are left to the
hardware cursor tracker below. Either kind wakes the registered
subscriber, so `wm` no longer polls for either -- see "Pointer
wakeups" below.

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
bit 0      pressed (1) / released (0)
bits 8:1   USB HID usage code
bits 16:9  modifier byte at the time of the event
bits 19:17 lock state after the event: Num, Caps, Scroll (Z_KBD_LOCK_*)
```

### Scroll wheel

With the current USB core (`rtl/usb/`), the mouse register's top byte is
`wheel_acc`: a free-running signed 8-bit **accumulator** of the wheel,
not a per-report delta. Read it, subtract the last value you read, and
the difference is the notches since -- none lost to a report you did
not read in time. Positive is away from the user.

It counts byte 3 of the report **only for a mouse in report protocol
whose report descriptor puts the wheel there** (below). The HID spec
defines the boot mouse report as three bytes -- buttons, X, Y -- and
anything after them in boot protocol is undefined: many mice put a
wheel there, some send vendor data, some (a Microsoft 045e:0737) send
nothing. Counting it anyway (rounds 35-37) could have made such a mouse
scroll by itself. The driver marks a confirmed wheel with bit 2 of the
poll slot's mode (`usb_hid_compat.v`, `in_wheel`).
**Boot or report protocol.** Some mice send the wheel only in report
protocol -- a Microsoft 045e:0737 does: in boot protocol its fourth byte
never moved (round 36). So for each mouse the driver reads its HID
report descriptor during enumeration (`usbh_hid.c`,
`z_usbh_hid_rdesc_parse()`) and, if the input report is the simple
layout -- no report IDs, buttons from bit 0, then X, Y and a wheel as
8-bit values in bytes 1, 2 and 3 -- sets report protocol: the hardware
parses those bytes exactly as it parses a boot report, wheel included.
Any other layout (report IDs, 12- or 16-bit axes, ...) or an unreadable
descriptor, and the mouse is put in boot protocol as before: movement
and buttons work, the wheel does not. `lsusb` shows, per mouse, the
protocol chosen and why, the layout the descriptor gave, the register
(wheel counter in the top byte) and the raw bytes of the last report --
in a debug build (`make USBH_DEBUG=1`, `docs/usb_host.md`, "Debug
build"); a normal kernel's `lsusb` leaves that detail out.

Apps do not read it: `wm` does (`dispatch_wheel()`), and sends the
notches to the window that owns the pointer as **`Z_WM_WHEEL`**
(`zwm.h`) -- a signed count, never coalesced, unlike `Z_WM_MOUSE`.
`term` scrolls its history three lines a notch.

### Lock keys and keyboard LEDs

The kernel keeps Num Lock, Caps Lock and Scroll Lock (`hid.c`,
`hid_locks`): each press of the key toggles its bit, the state is
shared by both keyboard blocks, and every event carries it
(`Z_KBD_EV_LOCKS()`, `zkbd.h`). It also goes to every attached
keyboard's LEDs through the USB host driver (`z_usbh_kbd_leds()`), as a
HID `SET_REPORT` output report -- same byte, same bit order. A keyboard
that has just been recognised rolls its LEDs Num -> Caps -> Scroll three
times (about 0.7 s), then shows the real state; `lsusb` shows it as
`leds=N--` and so on.

**With the older `usb_hid_host` core** (`USB_HID` builds) the lock
state and Caps Lock work the same -- they are `hid.c` and `wm`, shared
by both cores -- but the LEDs stay dark: that core cannot send a report
to the keyboard. The USB host driver finds no controller at start-up and
does nothing (`usbh_present`).

Caps Lock is applied by `wm` when translating: for the letters a-z
only, Shift is inverted. All three locks start **off** (round 36). Num
Lock in particular: on a compact keyboard the keyboard's own firmware
turns a block of letter keys into an embedded keypad while the host
says Num Lock is on, so starting it on (round 34) took those keys away.
A full-size keypad still types digits with Num Lock off -- `zkbd` maps
keypad keys to digits regardless -- and Num Lock does not (yet) make
them navigation keys. Scroll Lock is state and an LED only. The lock keys themselves
translate to no keysym, so apps never see them as input.

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
into a **keysym**. Keysyms come in two disjoint ranges:

- `0x000000`-`0x10FFFF`: a character, as its Unicode codepoint,
  already shift/ctrl-resolved. ASCII is the bottom of this range, so
  `keysym >= 0x20 && keysym < 0x7f` still means "printable ASCII".
  Ctrl+letter returns the usual control-code mapping (Ctrl+A = `0x01`
  .. Ctrl+Z = `0x1A`), matching every other terminal convention.
- `0x110000`+ (`Z_KEY_NAMED_BASE`): named keys with no character --
  `Z_KEY_UP`/`_DOWN`/`_LEFT`/`_RIGHT`, `_HOME`/`_END`/`_PAGEUP`/
  `_PAGEDOWN`, `_INSERT`/`_DELETE`, `_F1`..`_F12`. 0x110000 is the
  first value above Unicode, so no character can land here.
- `Z_KEY_NONE`: no mapping for this usage (media keys, keys a layout
  leaves empty), or a bare modifier press/release (`0xE0`-`0xE7`) --
  those have no keysym of their own; a caller that wants to react to a
  modifier changing by itself should check the raw usage code before
  calling this.

`Z_KEY_IS_NAMED(k)` and `Z_KEY_IS_TEXT(k)` tell the ranges apart;
`Z_KEY_IS_TEXT` is the Unicode-aware version of the ASCII printable
test, for apps that store more than ASCII.

The named keys used to be at `0x100`+, which is Latin Extended-A --
harmless while every keysym was ASCII, a collision once a keyboard
layout can type those letters. They moved in the first phase of the
keyboard layout work ([keyboard_layouts.md](keyboard_layouts.md)).
Code that uses the `Z_KEY_*` names is unaffected.

For the raw events themselves, `zkbd.h` has `Z_KBD_EV_PRESSED(ev)`,
`Z_KBD_EV_USAGE(ev)`, `Z_KBD_EV_MODS(ev)` and `Z_KBD_EV_LOCKS(ev)`.
Use them rather than open-coding the shifts: the usage code is at
bits 8:1, not 7:0. `midi`, `play` and `track` had exactly that wrong in
their console (window-less) modes until the same change fixed it.

Which character a key types depends on the keyboard layout. The
translation is table-driven: `z_kbd_translate()` looks the key up in
the active layout (generated from xkeyboard-config, `zkbd_layouts.c`)
and applies Shift, AltGr, Caps Lock and Ctrl; Enter/Escape/Backspace/
Tab/Space, the F-keys, the navigation cluster and the keypad are the
same on every layout. Backspace maps to `0x7f` (DEL), matching
`zeitlos.c`'s `readline()`, which already accepts either `CH_BS` or
`CH_DEL`. The kernel stamps the active layout into every event
(`Z_KBD_EV_LAYOUT`), so `z_kbd_event_to_keysym(ev)` is the one call a
raw-event reader needs; `z_kbd_usage_to_keysym()` is US only, as it
always was. The whole design: [keyboard_layouts.md](keyboard_layouts.md).

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

The keysym field is 23 bits (bits 31:9), wide enough for every
Unicode codepoint and every named key. It was 15 bits (bits 23:9)
before keysyms became Unicode; the low 24 bits are laid out exactly as
they were, so ASCII keys pack to the same word either way. An app
built against the old header still reads characters correctly but sees
named keys as `Z_KEY_NONE` -- rebuild it.

Packed rather than a `Z_MAP`, for the same reason as `Z_WM_REDRAW`:
this can fire at high frequency (every keystroke, plus a release for
each), and a fresh heap allocation per event is more than this needs
given `wm` never frees the message objects it sends (see
`docs/messaging.md`, "borrowed data has a lifetime"). Demo windows
(owned by `wm` itself) have no app to notify, same check
`notify_moved()` already uses for mouse-driven window moves.

### Keys wm keeps

Not every key reaches the focused app. `wm` consumes its own global
keys before forwarding anything: Alt+Tab, Alt+Arrow, Alt+[ and Alt+],
Alt+Esc and Ctrl+Alt+Arrow ([window_manager.md](window_manager.md),
[game_mode.md](game_mode.md)), and the speech keys -- Super+S, A, C, V, W, R, E
and R ([tts.md](tts.md)). The speech keys only match with Super held
and neither Ctrl nor Alt; any other Super+key still reaches the app as
before (as the plain letter -- translation ignores the GUI bit).
Super+Space is wm's too: the next keyboard layout.

A lone Ctrl tap stops speech. It is recognised from the pseudo-usage
events `sw/os/hid.c` synthesises for modifier edges (`0xE0`/`0xE4`),
and costs an app nothing: Ctrl still arrives in the modifier byte of
every key it is held with, exactly as before.

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

- **Older core only (`rtl/ext/usb_hid_host`): no hubs, no keyboard
  LEDs.** Both work with the current USB host (`rtl/usb/`,
  docs/usb_host.md).
- **Num Lock does not change a full-size keypad.** Keypad keys give
  digits either way; the lock state and LED are right, the navigation
  mapping is not written. (A compact keyboard's embedded keypad is the
  keyboard's own doing, and does follow Num Lock.)
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

## Pointer wakeups, and why wm stopped polling

`wm` used to wake 732 times a second whether or not the mouse had
moved, because the pointer was polled from `reg_usbN_cursor` and
nothing would wake it otherwise. `wm.c` said as much in its idle
comment.

That turned out to be a software gap, not a hardware one. **The mouse
interrupt was always firing.** `rtl/usb_hid.v`'s `report` pulse drives
`cpu_irq[5]`/`cpu_irq[6]` for keyboard, mouse *and* gamepad reports
alike (see `sw/os/hid.c`'s header); the ISRs simply acted on `typ==1`
and dropped everything else. No RTL change was needed — `LATCHED_IRQ`
already covers those bits, because `report` is a single 12MHz-domain
cycle and would not otherwise survive to the ISR.

So `hid_irq_common()` now calls `k_proc_unblock()` on a registered
subscriber, and `wm` registers itself through
`Z_SYS_HID_PTR_SUBSCRIBE` (`z_hid_pointer_subscribe()`, `zeitlos.h`).

**For any report, keyboard included** -- which the name does not
suggest, and which the first version did not do. It woke only on
non-keyboard reports, on the reasoning that a keystroke already has
somewhere to go: the ISR pushes it into the ring and the reader picks
it up next time round its loop. That holds while the reader wakes on a
timer. It stops holding the moment the reader sleeps until something
happens, which is the whole point of this mechanism -- a key that only
landed in the ring, with nothing to wake anybody, sits there until some
unrelated event arrives. A wake the subscriber did not need costs it
one pass around its loop; a keystroke that appears when the user
presses the *next* key is the kind of fault that gets blamed on the
keyboard.

**Nothing is delivered.** The cursor is level state in a register and
coalescing is desirable — `zwm.h` already tells apps to act on the
last mouse sample rather than every one — so the ISR wakes the reader
and lets it read the current position. That keeps the existing model,
and means there is no event queue to overflow under fast motion and no
allocation in an ISR.

### What wm passes as a timeout

Not a constant. `wm_idle_ticks()` (`wm.c`) answers with **the nearest
deadline `wm` actually has**, and with **0 -- wait indefinitely** when
it has none.

The fixed 16 ticks (~22ms) it started with was reasoned from the work
driven by neither the pointer nor messages: the dock's launch deadlines
and `check_core_services()` at startup are polled against
`z_uptime_ticks()`, and blocking forever would stall them until the
user happened to move the mouse. That is right about *which* work needs
a timeout, and wrong to pay for it continuously — those deadlines
mostly do not exist. No dock icon is waiting to un-invert unless
something was just launched, and `check_core_services()` stops polling
once `init0` has registered.

So the timeout is computed rather than assumed, and an idle desktop
with nothing pending wakes `wm` only for input or a message. The
pending launch argument is deliberately not among the deadlines: it
expires by being tested when an app asks for it, not on a clock.

The rubber band of a drag does not need a tick either, even though it
follows the pointer: a USB report raises `Z_IRQ_HID`/`_HID1`, and a
pointer that arrives some other way has its own wake (below). Sleeping
a tick at a time while a button is held would burn CPU with the pointer
standing still.

On a kernel predating the syscall the subscription returns false, the
pointer really is poll-only, and `wm` keeps the old one-tick behaviour
— the same binary has to run on both. That path costs 732 wakeups a
second, which is what this whole mechanism exists to avoid, so it is a
fallback rather than a mode anyone should be in.

### Input with no interrupt behind it

Everything above assumes input arrives as a HID report, which raises an
interrupt, which wakes the subscriber. Input that reaches the machine
some other way has neither, and there are two such paths now. Both are
syscalls appended at the end of `syscalls.def`, for the reason that
file's own header insists on.

**`Z_SYS_HID_INJECT`** (`k_hid_inject()`, `sw/os/hid.c`) pushes a
packed event -- the same `pressed`/`usage`/`modifiers` layout the ISRs
build -- into the same shared ring, under the same interrupt mask
`k_hid_read_key()` uses, so it cannot race the port ISRs. It reads back
through `hid_read_key()` like any other keystroke: nothing downstream,
`zkbd.h` translation included, knows the difference. It wakes the
subscriber itself, since there is no interrupt to do it.

**`Z_SYS_WM_WAKE`** (`k_wm_wake()`, `sw/os/kernel.c`) is the pointer
half, and it delivers nothing at all -- it just unblocks the same
subscriber. A software pointer is a register write (below) with no
interrupt attached, so the writer pokes the reader afterwards.
Deliberately not a second registry: there is one pointer as far as a
reader is concerned, whichever wire it came in on.

### A pointer with no hardware behind it

**ULX3S-specific.** `reg_vmouse` (`0xf000_0400`, `rtl/sysctl.v`) is a
software mouse: a plain register another process writes, laid out
exactly like `reg_usbN_cursor`'s x/y/buttons fields plus a `present`
bit at 24, so `wm` reads it with the code it already had and the
hardware cursor sprite mux prefers it while `present` is set. On a
board whose bitstream does not decode that address the register reads
0, `present` is never set, and nothing changes.

It exists because the ULX3S has no video output wired for a desktop
and no keyboard or mouse plugged into it -- the screen and the pointer
both arrive over the network, from a browser (`docs/esp32link.md`).
`net` writes each pointer packet here and calls `z_wm_wake()`; keyboard
packets go through `hid_inject()` instead, because a key is an event
and a cursor is a position.

**`present` is last-writer, not a latch**, and both sides clear it. The
writer drops it on silence (about a second) or when the pointer leaves
the browser window; `wm` drops it the moment the USB cursor moves
(`vmouse_yield_to_usb()`), so the sprite mux and the click arithmetic
cannot end up disagreeing about which pointer is real. Neither source
can freeze the other out, which is the property that matters on a board
where someone might plug a mouse in while a browser is connected.

## pid 0 no longer spins

`readline()` in `sw/os/kruntime.c` called `getch()`, which returns
`EOF` immediately when the UART FIFO is empty, and then `continue`d --
a tight spin at full speed, with no throttle at all, for as long as
the shell sat at its prompt. `wm` at least yielded a tick.

That is not free: the scheduler divides the CPU between RUNNABLE
processes, so an idle serial console was taking a full share out of
whatever was in the foreground.

The UART was already interrupt-driven (`cpu_irq[4]` -> `z_uart_irq()`
-> FIFO), so the wake was almost free: the RDA path now calls
`k_proc_unblock(0)` after pushing bytes. `readline()` blocks with a
`Z_TICK_HZ / 4` timeout, which is a backstop rather than the
mechanism -- the keystroke itself wakes it.

pid 0 is hardcoded rather than made subscribable, unlike the pointer:
there is exactly one serial console and it belongs to the shell,
whereas the pointer has a real choice of consumer.

### The race, and why it is already handled

Both wakeups can fire in the window between a process deciding to wait
and actually being marked `BLOCKED`. `k_proc_unblock()` is explicitly
built for this: on a process that has not blocked yet it records the
wakeup in `Z_PROC_FLAG_WAKE` rather than losing it, and the next
`k_proc_wait()` consumes it and returns immediately. The bounded
timeouts above mean that even if that mechanism were ever broken, a
missed wakeup would degrade to a small latency rather than a hung
console or a dead pointer.
