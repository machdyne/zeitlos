# USB CDC-ACM console

A console on the USB-C socket, so that the serial console stops needing
a PMOD connector and stops needing a USB-UART adapter.

Enabled per board with `` `USB_CDC `` in `rtl/boards.vh`. Off
everywhere by default.

```
$ minicom -D /dev/ttyACM0
```

The baud rate does not matter and is not asked for. Any value works
because there is no line to run at a rate.

## Why

Obst and Lakritz wire their USB-C socket straight to the FPGA through
22R series resistors, with a 1.5k pull-up on D+ switched by a third
pin. The DFU bootloader uses that to accept a bitstream over
`dfu-util`, and then hands over -- after which the socket does nothing
at all except supply power.

Meanwhile the console occupies a PMOD connector. Obst has two of them
and Lakritz has one, so on Lakritz "console" and "GPIO port" were the
same eight pins and `lakritz_gpio` had to give up the console to get
them (see `docs/gpio.md`). Moving the console onto a socket the user
has already plugged a cable into makes that a non-choice.

It also removes a prerequisite. A new Obst or Lakritz previously
needed a USB-UART PMOD before it would say anything; now the cable
that powers the board is the cable that carries the console.

## What software sees

Nothing. That is the design.

`rtl/usb_cdc_uart.v` presents a 16550 register map at `0xf000_0000` --
the same window `rtl/uart.v` answers -- so `sw/bios/bios.c`,
`sw/os/uart.c`, `sw/os/sh.c` and everything reaching the console
through `sw/common/zeitlos.h`'s `reg_uart0_*` macros are byte-for-byte
identical on a `` `USB_CDC `` board. `xf` uploads, the kernel shell,
`term`, VT100 escapes: all unchanged.

`Z_FEATURE_UART0` (`CSR_FEATURES` bit 12) is SET on such a board,
because the question that bit answers is "is there somewhere for my
output to go" and there is. `Z_FEATURE2_USB_CDC` (`CSR_FEATURES2` bit
2) says which kind it is -- see "Line settings are not real" below for
the one case that cares.

## Enabling it

Uncomment one line in the board's block in `rtl/boards.vh`:

```
`define USB_CDC
```

That is the whole edit. `` `UART0 `` is removed for you by the
`` `undef `` at the bottom of that file -- the two answer the same
address window and only one can, and leaving `` `UART0 `` defined
would also declare `UART0_TX` with nothing driving it, which
synthesises to a pin held at a constant rather than an absent one.

The three USB balls are already in `boards/obst_v0.lpf` and
`boards/lakritz_v0.lpf` unconditionally. Without `` `USB_CDC `` they
name ports that do not exist, which nextpnr reports as a warning and
ignores.

| board  | D+ | D- | pull-up |
|--------|----|----|---------|
| Obst   | M1 | M2 | R1      |
| Lakritz| T6 | R6 | R7      |

Lakritz's are not in the board's published `lakritz_v0.lpf`, which
never declares this port. They were read out of the board's own KiCad
netlist: nets `USB0_DP`/`USB0_DN`/`USB0_DP_PU` land on U1 pads
T6/R6/R7, with R7 and R8 the 22R series resistors and R6 the 1K5
pull-up.

To also free the PMOD for something else, uncomment `` `GPIO_PORT0 ``
and the matching PMOD block in the board's `.lpf`.

## The boot banner blocks. That is deliberate.

A USB device does not exist until a host enumerates it, and a CDC-ACM
port is not drained until something opens it -- Linux's `cdc_acm`
driver only submits read URBs from its `open()` path. The BIOS prints
its banner within microseconds of reset, long before either has
happened.

There is no buffer here to hold that output. Obst sits at 52 of 56
DP16KD before this block exists, and every byte of storage in
`rtl/ext/usb_cdc` is flip-flops, so a 2KB holding FIFO was not
available at any price worth paying.

So instead of buffering, the backpressure is allowed through. THRE
(LSR bit 5) reports "no room" while the byte in the holding register
has not been taken by the USB side, and

```c
while ((reg_uart0_lsr & 0x20) == 0);
```

in `bios.c`'s `putchar()` blocks exactly where it should. Start
minicom and the entire banner from `ZB` onward comes out in order with
nothing lost.

This is better than a buffer would have managed, because it has no
size limit. The machine simply waits.

### ...and it gives up after ten seconds

If nothing ever opens the port, the above is a hang -- and not a
degraded console but a dead machine: `load_zeitlos()` never runs, so
there is no kernel, no desktop, and nothing on the video output
either. A board plugged into a phone charger would never boot.

A byte stuck for `` `USB_CDC_STALL_CYCLES `` (default 480,000,000 --
ten seconds at 48MHz) therefore gives up. THRE starts reporting ready
and further writes are **discarded** until the USB side accepts
something again.

Recovery is automatic. The moment the host opens the port and the
endpoint FIFO drains, the stuck byte is taken and normal backpressure
resumes. Output written during the gap is gone, which is the same
bargain a real UART with nothing plugged into it makes on every
character.

**What this looks like in practice:** power the board, plug in USB-C,
start minicom within ten seconds, and you see everything. Start it
later and you see the machine from wherever it had got to, exactly as
you would with a serial cable.

## What is not emulated

**Line settings are not real.** DLL, DLM and LCR are stored and read
back -- DLAB in particular must work, or the BIOS's divisor writes
would be transmitted as characters -- but they control nothing. A
CDC-ACM link runs at USB speed and its line coding is advisory.
Software that would offer the user a baud rate should check
`Z_FEATURE2_USB_CDC` and decline, rather than presenting a control
wired to nothing.

Both **FCR resets** are real. Bit 2 clears the transmit holding
register; bit 1 drains `usb_cdc`'s `out_fifo` by holding its ready
line high, since that FIFO has no flush input of its own. See "The
terminal greeting problem" below for why bit 1 is not housekeeping.

**Modem control.** MSR reads a constant with DCD, DSR and CTS
asserted. MCR is stored and read back and drives nothing. There is no
cable to be disconnected from.

**Overrun cannot happen,** so LSR bit 1 reads 0 and means it.
`rtl/ext/usb_cdc` NAKs an OUT transaction when its FIFO is full and
the host retries, so a slow reader makes the sender wait rather than
losing bytes. This is strictly better than the 16550 it replaces --
see `sw/common/zuart.h`'s header for what 16 bytes of hardware FIFO
cost a polled reader at 1 Mbaud.

## Cost

Measured on ECP5, `-abc9`, at `` `USB_CDC_MPS `` 8:

| | LUT4 | FF | DP16KD |
|---|---|---|---|
| `rtl/ext/usb_cdc` core | 1144 | 395 | 0 |
| whole `usb_cdc_uart` block | 1276 | 524 | 0 |
| `rtl/uart.v` it displaces | 256 | 152 | 0 |

**Zero block RAM at any packet size.** That is the property that made
this core the right choice; every byte of buffering in it is
flip-flops, which is also why the packet size is the dominant knob:

| `USB_CDC_MPS` | LUT4 | FF |
|---|---|---|
| 8 | 1257 | 435 |
| 16 | 1463 | 571 |
| 32 | 1901 | 835 |
| 64 | 3038 | 1355 |

8 is the default and is worth roughly what the 1 Mbaud console it
replaces was worth, which is all a console needs to be. Raise it only
on a board with fabric to spare, and re-check `make timing` after.

Whole-SoC effect on Obst (ECP5 12F), before and after:

| | baseline | `` `USB_CDC `` |
|---|---|---|
| TRELLIS_COMB | 17210 (70%) | 16943 (69%) |
| TRELLIS_FF | 7436 | 7663 |
| DP16KD | 52/56 | 52/56 |
| sys_clk | 49.61 MHz | 56.18 MHz |

The flip-flop delta (+227) is the honest measure of what was added.
The LUT total went *down* and the clock got *faster*, which is
`-abc9` remapping the whole design once the 16550's baud divider left
the critical path -- the same global-mapping effect the `Makefile`'s
`ABC9` comment describes, and not a number to rely on holding after
any other change. Re-check both after touching anything.

## USB identity

The device enumerates as **16d0:116d**, which is Machdyne's DFU
bootloader identity on these same boards. That is deliberate.

The PID is not per-board. Every board in `machdyne/tinydfu-bootloader`
-- Obst, Lakritz, Kuchen, Schoko, Minze, Riegel, Konfekt, Kaugummi,
Klinge, Kopflos -- uses `16d0:116d` and they are told apart by their
product strings. So the PID already identifies a *function* rather
than a device, and adding a second one for the console would be a
change of policy rather than a correction.

Nothing breaks on the host side, because hosts bind by interface
class, not by VID:PID:

| | bDeviceClass | interface | matched by |
|---|---|---|---|
| DFU bootloader | 0x00 | 0xFE / 0x01 | `dfu-util` |
| Zeitlos console | 0x02 | 0x02 / 0x02 | `cdc_acm` |

They also happen to differ in `bcdDevice` -- 0.00 for the bootloader,
1.10 here -- so `lsusb -v` can tell them apart.

Override per board if a separate allocation is ever obtained:

```
`define USB_CDC_VID 16'h16d0
`define USB_CDC_PID 16'hXXXX
```

### The cost is on Windows

Windows keys driver binding on the hardware ID
`USB\VID_16D0&PID_116D`. Anyone who has run **Zadig** to install
WinUSB or libusb so that `dfu-util` works has bound that exact ID to
WinUSB -- and when the console enumerates on the same ID a second
later, Windows can apply the cached binding and **no COM port
appears**.

`bcdDevice` does not rescue this. `REV_xxxx` forms a more specific
hardware ID, but Zadig's binding is written against the generic one
and takes effect regardless. This is the classic failure mode of an
application and its bootloader sharing a PID.

Linux and macOS are unaffected. If Windows users hit this, the fix is
a separate PID from MCS and the one-line override above.

### Two things we cannot set

Both are limitations of `rtl/ext/usb_cdc`, which this tree vendors
unmodified.

**`bcdDevice` is hardcoded.** It is a `localparam` inside
`ctrl_endp.v`'s `DEV_DESCR`, fixed at 1.10; only `VENDORID` and
`PRODUCTID` are parameters. The 0.00-versus-1.10 difference from the
bootloader is therefore accidental rather than chosen, and it would
change silently if the vendored core is ever updated. Do not build
anything that depends on it.

**There are no strings.** `iManufacturer`, `iProduct` and
`iSerialNumber` are all 0, because the core's descriptors do not carry
them. So the console shows in `lsusb` as a bare `16d0:116d` CDC
device, next to a bootloader that does say "Lakritz DFU Bootloader".
That is the least helpful arrangement for answering "which of these am
I looking at", and it is the one real argument for a separate PID.

Fixing either means patching the vendored core. Worth doing if the
strings turn out to matter; not worth a fork on day one.

### udev, and why ModemManager matters here

On most Linux desktops ModemManager probes newly-appeared `ttyACM`
devices and sends AT commands at them for several seconds. That window
overlaps exactly with the BIOS stalled on THRE waiting for something
to open the port -- so ModemManager opens it, the endpoint FIFO drains
into ModemManager, **the boot banner is consumed**, and a terminal
started afterwards sees only what came later.

The stall design makes this more likely to bite than it would on an
ordinary UART, so the rule below is worth installing even though the
console works without it.

```
# /etc/udev/rules.d/70-zeitlos.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="16d0", ATTRS{idProduct}=="116d", \
    ENV{ID_MM_DEVICE_IGNORE}="1", MODE="0660", GROUP="dialout", \
    SYMLINK+="zeitlos"
```

```
$ sudo cp tools/70-zeitlos.rules /etc/udev/rules.d/
$ sudo udevadm control --reload-rules
```

Then `minicom -D /dev/zeitlos`, which survives the port being
renumbered.

Note that `SUBSYSTEM=="tty"` only ever matches the console -- the DFU
bootloader has no tty -- so the shared PID does not stop this rule
being precise. Sharing an ID costs less on Linux than it first looks.

## The terminal greeting problem

**Symptom:** the board never autoboots. It prints its banner, then
sits at the BIOS prompt as though somebody had pressed a key.

**Cause:** the console blocks until a terminal opens the port -- and a
terminal opening a port is also the moment it sends its greeting.
minicom's default modem init string is

```
~^M~AT S7=45 S0=0 L1 V1 X4 &c1 E1 Q0^M
```

and ModemManager sends AT probes of its own on a Linux box without the
udev rule above. So the BIOS unblocks, finishes its banner, reaches
its prompt loop and finds bytes already waiting. `bios.c` treats any
byte as "the user interacted" and cancels autoboot **permanently** --
its `ctr == AUTOLOAD_CNT` test is an exact equality, and `ctr` is
never reset, so it never matches again.

On a real UART this was a race you usually won, because the console
was already draining long before anyone opened a terminal. Here the
two events are the *same event*, so it is not a race at all: it
happens every single boot.

**Fixed in two places.** `bios.c` writes FCR once more on the way into
its prompt loop -- after the banner has drained, which is the point at
which the console genuinely becomes usable, rather than in
`uart_init()`, which runs before it and flushes a port nothing has
opened -- and follows it with a `getchar()` drain to catch anything
landing in the gap:

```c
	reg_uart0_fcr = (uint8_t)0b00000111;
	while (getchar() != EOF);
```

Neither half closes the window completely; a byte still on the wire
will arrive. But a terminal sends its greeting at `open()`, which is
squarely inside what this covers.

**Workarounds if you are running an older BIOS:**

```
$ minicom -o -D /dev/ttyACM0      # -o: skip the modem init
```

and install `tools/70-zeitlos.rules` for ModemManager. Both are worth
doing regardless -- `-o` costs nothing and the udev rule prevents
ModemManager eating the banner itself.

## Interaction with the DFU bootloader

They are sequential, not simultaneous. The bootloader owns the socket
for its five-second window; when it hands over, the FPGA is
reconfigured, the pull-up drops, and the host sees a disconnect
followed a moment later by a new device -- this one.

So a `dfu-util` flash cycle ends with the port name changing, and a
terminal left open on the old one will need reopening. That is
inherent in the FPGA being both devices in turn, and is the reason the
two need distinct PIDs.

## Limitations

- **One channel.** `CHANNELS` is 1. The core supports up to 7 CDC
  channels; nothing here needs a second and each costs its own FIFOs.
- **48MHz only.** `BIT_SAMPLES` is 4 and the core is clocked directly
  from `sys_clk`, which is exactly 48MHz on every board that can have
  this -- so there is no PLL output and no clock-domain crossing to
  pay for. A board clocked at anything else needs a dedicated 48MHz
  output and `USE_APP_CLK = 1`.
- **No remote wakeup, no suspend current limiting.** The device draws
  what the board draws and does not implement USB suspend.
- **The pull-up is never released once asserted.** Forcing a
  re-enumeration would be a matter of dropping `usb_ufp_pull` for a
  few milliseconds; nothing does today.

## Files

```
rtl/usb_cdc_uart.v          the 16550 facade and the stall/timeout logic
rtl/ext/usb_cdc/            ulixxe/usb_cdc, MIT, vendored unmodified
rtl/sysctl.v                cs_uart0 window, three-way (`USB_CDC / `UART0 / null)
rtl/boards.vh               per-board `USB_CDC, and the `undef that removes `UART0
rtl/csrs.vh                 CSR_FEATURES bit 12, CSR_FEATURES2 bit 2
sw/common/zsoc.h            Z_FEATURE2_USB_CDC
boards/obst_v0.lpf          M1 / M2 / R1
boards/lakritz_v0.lpf       T6 / R6 / R7
tools/70-zeitlos.rules      udev: quiet ModemManager, /dev/zeitlos symlink
```
