# Gamepads

USB HID gamepad support, on either or both USB host ports, with hot
swapping.

## What was already there

`rtl/ext/usb_hid_host/src/usb_hid_host.v` has decoded gamepads since
the day it was vendored in. It detects `typ == 3`, and its own gamepad
section copes with several different pad report layouts (its author
tested five). It drives `game_l/r/u/d`, `game_a/b/x/y`, `game_sel` and
`game_sta`.

`rtl/usb_hid.v` simply never connected any of them. The information was
being produced every report and thrown away.

So there is **no new USB protocol handling anywhere in this feature**.
What was actually needed was the clock domain crossing around those
signals, the hot-swap handling, and the software API.

## Registers

Word offset 4 in each port's own register block:

| addr | port |
|---|---|
| `0xc000_0010` | USB host port 0 |
| `0xc000_0030` | USB host port 1 |

Reads back `{ 20'b0, typ[1:0], buttons[9:0] }`.

| bit | button | | bit | button |
|---|---|---|---|---|
| 0 | left | | 5 | B |
| 1 | right | | 6 | X |
| 2 | up | | 7 | Y |
| 3 | down | | 8 | select |
| 4 | A | | 9 | start |

Bit positions must stay in sync with `sw/common/zpad.h`'s `Z_PAD_*`
constants. There is no shared source between the Verilog and C halves
— the same hand-maintained split as `zkbd.h`'s HID usage codes.

The device type is in the **same word** as the button state on purpose.
A pad with nothing pressed reads as zero, which is indistinguishable
from no pad at all; asking a second register would leave a window where
a hotplug lands between the two reads. One read, one clock domain, one
answer.

## Why the state is latched

The ten pad bits live in `usb_hid_host`'s 12MHz `usbclk` domain. A
wishbone read that muxed them straight through would be sampling ten
unsynchronised signals, so a read landing while the pad state changes
can return a mixture of two different reports.

For a d-pad that is not a harmless glitch. Left and right are separate
bits, so a torn read during a left-to-right flick can report **both**
pressed or **neither** — and a game that resolves "both" as "stand
still" gets a character that occasionally refuses to move for one
frame. Unreproducibly, because it depends on the phase between two
clocks.

`report_edge` already existed in `usb_hid.v` for the mouse deltas and
solves exactly this problem: one `wb_clk`-wide pulse per report,
correctly synchronised. The pad state is valid at that moment for the
same reason the deltas are, so it is captured there and held until the
next report. Software reads a coherent snapshot of one report, which is
what a pad state *is* — never a blend of two.

`rtl/gpu/bench/tb_gamepad.v` tests this directly: it changes the raw
`usbclk`-domain bits with no report pulse while reading continuously,
and requires the register not to move.

## Hot swapping

Supported, and it needed hardware work to be true rather than nominally
true.

**Unplugging is silent.** Reports simply stop arriving. Three separate
things break if nothing accounts for that:

**1. Frozen pad state.** `game_state` would hold whatever was last
reported. Pull the cable mid-jump and the machine believes RIGHT is
held down forever, with no event ever coming to correct it.

Fixed in `rtl/usb_hid.v` by clearing the pad state whenever the port is
not currently reporting a gamepad. `usb_hid_host` clears `typ` to 0 on
disconnect, so this covers unplug, and it equally covers swapping a pad
for a keyboard on the same port.

**2. No interrupt at all.** The HID interrupt was the report pulse, so
plugging a device in announced itself (reports start) but unplugging
one did not (reports stop). The OS was never told.

Fixed by firing the interrupt on device type change as well as on
reports. Safe to OR the two together because `sysctl.v` marks both HID
interrupts `LATCHED_IRQ` — a one-cycle pulse from either source is
captured in hardware and stays visible until the ISR runs, so two
sources cannot lose each other's edge. That also makes it safe that
`uhh_report` is a `usbclk` pulse while the type change is a `wb_clk`
one: what reaches the CPU is a latch, not the wire.

**3. Stuck keys.** `sw/os/hid.c` kept held keys in its per-port state
until a later report showed them absent — and after an unplug no later
report ever comes. Yank a keyboard mid-keypress and every app
downstream believed that key was still down.

Fixed by flushing on the type-change interrupt: `hid_irq_common()`
synthesises the release events the departed device is no longer around
to send, exactly as if it had reported every key up before leaving.
Consumers see an ordinary release and need no knowledge of hotplug at
all.

## Two pads, and no fixed port assignment

**Which device is on which port is not fixed anywhere.** There is no
"the keyboard port". Either port may hold a keyboard, a mouse, a
gamepad or nothing, independently of the other, and that can change
while the machine is running.

So `zpad.h` does not talk about ports in its main API. It talks about
**pad indices**: pad 0 is the lowest-numbered port currently reporting
a gamepad, pad 1 is the other one. A two-player game asks for pad 0 and
pad 1; if only one pad is plugged in, pad 1 is simply not present,
whichever physical socket the one pad happens to be in. If the pad in
port 0 is unplugged while another sits in port 1, the survivor is
promoted to pad 0.

This is the same approach `sw/os/hid.c` and `wm.c` already take for the
keyboard and mouse — see `wm.c`'s `mouse_port()`.

`z_pad_port()` rescans on every call rather than caching. Two register
reads, not worth caching, and a cache would be exactly the thing that
got hot swapping subtly wrong.

## Software API

`sw/common/zpad.h`, header-only and polled directly from userspace —
same reasoning as `zsoc.h`. Unlike the keyboard, a pad has no event
queue to own and no edge detection that must survive being missed: the
state is a level, and a game reads it once per frame.

```c
#include "../../common/zpad.h"

z_pad_state_t pad;
z_pad_init(&pad);

for (;;) {
    z_pad_update(&pad, 0);          /* pad index, not port */

    player_x += z_pad_axis_x(&pad) * SPEED;
    if (z_pad_pressed(&pad, Z_PAD_A)) jump();

    z_game_wait_frame();
}
```

`z_pad_read()` returns 0 for a pad that is not present — deliberately
the same value as "present, nothing pressed", so a caller that just
wants to move a character does not have to special-case an absent pad
to get the right behaviour, which is that the character stands still.
Use `z_pad_present()` where the difference matters (a "press start"
screen, a player-2 join prompt).

`z_pad_released()` also fires when a pad is unplugged with a button
held, since the state clears. That is correct and useful: a character
that was running should not keep running because the cable came out.

`z_pad_axis_x()`/`_y()` resolve opposite directions to 0 rather than
letting one win. Some pads genuinely report both for a moment during a
fast flick, and a character that briefly stands still reads as a
hesitation, whereas one that briefly reverses reads as a bug.

Call `z_pad_count()` every frame rather than once at startup. It is two
MMIO reads, and doing so is what makes plugging a second pad in
mid-game work with no further effort.

## Testing

`rtl/gpu/bench/tb_gamepad.v` is self-checking and uses a stub
`usb_hid_host` rather than the real one — the real core needs an actual
USB device bit-banging a low-speed link to produce a report at all, and
none of the behaviour under test lives in that core anyway.

Covered: bit order for every button individually (so a transposed bit
cannot hide behind a symmetric mistake), the tearing fix, hot unplug
with a direction held, the interrupt on device type change, hot swap
from pad to keyboard, and a regression guard that the keyboard
registers this change did not touch still work.

```
iverilog -g2005 -DUSB_HID -o /tmp/tb_pad.out \
    rtl/gpu/bench/tb_gamepad.v rtl/usb_hid.v
vvp /tmp/tb_pad.out
```

Prints `RESULT: PASS` or `RESULT: FAIL` plus the offending values.
Currently passes. Runs in about a second, unlike `tb_game_mode.v`.

Note `iverilog` needs the same small patches to `usb_hid.v` that
`bench/README.md` already documents for `glyph.v` and `vram.v` — the
empty `#()` parameter list, the trailing comma in the port list, and
`curs_x`/`curs_y` being declared both as output nets and as regs. yosys
accepts all three; they are the file's existing style and are not worth
changing for a linter.
