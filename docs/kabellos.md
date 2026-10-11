# Kabellos WiFi PMOD

[Kabellos](https://github.com/machdyne/kabellos) is an ESP32-C3-WROOM-02
on a PMOD. With it, a board that has no network hardware of its own
gets the same network link the ULX3S has with its soldered-on ESP32
([esp32link.md](esp32link.md)): WiFi, a NAPT gateway, the gateway's
TFTP and HTTPS fetcher, and the browser [remote desktop](remote_desktop.md)
served by the module.

The first target is **`lakritz_kabellos`**: Lakritz with Kabellos in
its one PMOD port. Nothing in it is Lakritz-specific beyond the target
spec, so other boards with a PMOD port are a few lines each (see
"Later" at the end).

| | |
|---|---|
| release target | `lakritz_kabellos` (`release/targets/lakritz_kabellos.spec`) |
| PMOD spec | `release/hw/pmods/kabellos.spec` |
| gateware | `UART1`, `ESP32_LINK`, `ESP32_PMOD` -- the ULX3S link plus PMOD ports |
| software | unchanged: `net` picks the ESP32 backend from the feature CSR |
| firmware | `esp32/zeitlos-nic`, built for the C3: `zeitlos-nic-kabellos.bin` |
| flashing | over the module's micro-USB, **in the PMOD socket**: `esp32/kabellos_flash.py` |

## What you need

- A **Kabellos V2** (ESP32-C3-WROOM-02). V1 used the ESP32-C3-MINI-1;
  this was written against the V2 pinout and schematic and has not been
  checked against V1.
- **J1 on the module open.** That is the default (J1 is unpopulated).
  Shorted, the module powers itself from its USB socket into the host's
  3V3 rail; the Kabellos README says never to plug it into a PMOD
  socket that way, and nothing here needs it.
- The module running **zeitlos-nic**, not the ESP-AT firmware it ships
  with. ESP-AT speaks AT commands on the same pins; Zeitlos speaks ZNIC
  and will see nothing it understands ("Flashing" below).
- WiFi credentials in `/data/net/net.cfg` on the sdcard -- the same file
  and format as the ULX3S ([esp32link.md](esp32link.md), "Bring-up").
- A micro-USB **data** cable, for flashing and for the module's console.

## Pinout

| PMOD | Kabellos | C3 | Zeitlos port | use |
|---|---|---|---|---|
| 1 | ESP_EN | EN | `wifi_en` | reset, 0 = held. Software-controlled (`0xf000_0200` bit 0) |
| 2 | ESP_BOOT | GPIO9 | `wifi_gpio0` | boot strap, 1 = boot from flash (`0xf000_0200` bit 1) |
| 3 | ESP_TX | GPIO21 (U0TXD) | -- | the C3's UART0 TX; not constrained (an output from the module) |
| 4 | ESP_RX | GPIO20 (U0RXD) | `ESP32_U0RX` | tied high, so UART0 idles instead of reading a break |
| 7 | ESP_GPIO4 | GPIO4 | -- | ESP-AT RTS; unused |
| 8 | ESP_GPIO5 | GPIO5 | -- | ESP-AT CTS; unused, reserved for flow control |
| 9 | ESP_GPIO6 | GPIO6 | `UART1_TX` | ZNIC, FPGA to C3, 3 Mbaud |
| 10 | ESP_GPIO7 | GPIO7 | `UART1_RX` | ZNIC, C3 to FPGA, weak pull-up |

On Lakritz those are balls B11, B12, (B13), B14, (A11), (A12), A13 and
A14 -- `release/hw/boards/lakritz.spec` owns that map. The reasoning for
each pin, including why pin 4 is driven and why pin 10 has a pull-up, is
in the comments of `kabellos.spec`.

The ZNIC pair is the module's ESP-AT "AT port" (GPIO6/7), not its
console pair (pins 3/4). The console UART is also where the C3's ROM
prints at every reset; keeping the data plane off it keeps those bytes
out of the receive FIFO, and leaves pins 3/4 free for the ROM
bootloader (see "Later").

## How it differs from the ULX3S

Same protocol, same `net.bin`, same `net.cfg`, same remote desktop, same
NAPT limits (outbound only; use the gateway's TFTP server at
192.168.4.1). The differences are all below the protocol:

| | ULX3S | Kabellos |
|---|---|---|
| chip | ESP32, 2 cores at 240 MHz, 520 KB SRAM | ESP32-C3, 1 RISC-V core at 160 MHz, 400 KB SRAM |
| ZNIC pins (ESP side) | GPIO16 / 17 | GPIO6 / 7 |
| console | UART0, reached through a passthru bitstream | USB Serial/JTAG, the module's micro-USB |
| flashing | passthru bitstream, sdcard out, two power cycles (`flash.py`) | over USB, in place (`kabellos_flash.py`) |
| shared pins | the microSD (GPIO 2/4/12-15), parked at boot | none; GPIO12-17 are the C3's own flash and are never touched |
| default TX power | 19.5 dBm | 15 dBm (see "Power") |
| WebSocket relay | one frame per update | the same message in 8 KiB fragments (see "Memory") |
| app partition | 1 MB (default) | 1.5 MB (`single app large`) |
| optimisation | `-Og` (IDF default) | `-O2` |

The board is a compile-time choice that follows from the IDF target
(`esp32/zeitlos-nic/main/Kconfig.projbuild`): building for `esp32c3`
*is* building for Kabellos. Target-specific settings are in
`sdkconfig.defaults.esp32c3`, which ESP-IDF reads after the common
`sdkconfig.defaults`.

## Building the firmware

ESP-IDF v5.4, as for the ULX3S. Installing it -- prerequisites, the
clone, `install.sh esp32,esp32c3`, and what to do when it fails -- is in
[esp32link.md](esp32link.md), "Installing ESP-IDF". To flash a
prebuilt image you need only esptool (`pip install esptool pyserial`).

```
python3 esp32/nic_build.py kabellos     # just Kabellos
python3 esp32/nic_build.py              # both boards
```

From an exported IDF shell, or with `IDF_PATH` set, or with
`--idf <path>`. The script keeps the two boards apart:

| board | target | build directory | sdkconfig |
|---|---|---|---|
| `ulx3s` | `esp32` | `esp32/zeitlos-nic/build/` | `esp32/zeitlos-nic/sdkconfig` |
| `kabellos` | `esp32c3` | `esp32/zeitlos-nic/build-kabellos/` | `build-kabellos/sdkconfig` |

The ULX3S keeps the paths it always had, so `idf.py build`, `flash.py`
and `nic_selftest.py` work as before. Kabellos has its own sdkconfig as
well as its own build directory because `idf.py set-target` rewrites the
sdkconfig it is given: sharing one would turn every switch between
boards into a full clean.

By hand, the equivalent is:

```
cd esp32/zeitlos-nic
idf.py -B build-kabellos -DSDKCONFIG=build-kabellos/sdkconfig set-target esp32c3
idf.py -B build-kabellos -DSDKCONFIG=build-kabellos/sdkconfig build
```

Each build also writes one merged image for flashing at offset 0:
`build-kabellos/zeitlos-nic-kabellos.bin` (and
`build/zeitlos-nic-ulx3s.bin`). That is the file a release ships.

## Flashing

**The module is flashed where it sits: in the Lakritz PMOD port, through
its own micro-USB socket.** The C3 has a USB Serial/JTAG controller
wired to that socket (whatever J1 says), and esptool can reset the chip
into its ROM bootloader through that controller alone -- without the
BOOT pin, which the FPGA holds high. No passthru bitstream, no sdcard
to remove, no power cycles.

```
python3 esp32/kabellos_flash.py --build        # build, then flash
python3 esp32/kabellos_flash.py                # flash the existing build
python3 esp32/kabellos_flash.py --image zeitlos-nic-kabellos.bin
                                               # a release image; needs
                                               # only `pip install esptool`
python3 esp32/kabellos_flash.py --info         # what is running now?
```

### The one precondition: the C3 has to be running

The C3 is only on USB while it runs, and **the FPGA holds it in reset
until `net` starts** (PMOD pin 1; `rtl/sysctl.v` resets the control
register to `en=0`). So:

1. Load a `lakritz_kabellos` image and let it boot to the desktop. `net`
   starts last and releases the module; the console says
   `esp32link: ESP32 released, HELLO/STA in main loop`.
2. Plug the micro-USB cable into the Kabellos. The host sees an
   Espressif device, USB ID `303a:1001` (`/dev/ttyACM*` on Linux,
   `/dev/cu.usbmodem*` on macOS).
3. Run `kabellos_flash.py`.

On Linux, the port needs `dialout` group membership and the project's
udev rules (`tools/70-zeitlos.rules`), which keep ModemManager from
probing the C3 -- its probe toggles DTR/RTS, which on this chip resets
it, and can leave it in its ROM bootloader. The rules also give it the
stable name `/dev/kabellos`. [esp32link.md](esp32link.md), "Serial port
permissions".

The tool finds the C3 by that USB ID rather than by "the only ACM
device", because with both cables in, the Lakritz's own USB console is a
CDC-ACM device too.

What it does, in order:

- refuses an image that is not zeitlos-nic for an ESP32-C3 (the ULX3S
  image is the obvious mistake, and is named as such), and a local build
  that is older than any of its sources (`--force-stale` overrides);
- writes it with esptool (`--chip esp32c3 --before default_reset
  --after hard_reset`): the offsets the build recorded in
  `build-kabellos/flash_args`, or the merged image at 0x0 with `--image`;
- waits for the C3 to reappear on USB (it re-enumerates on every reset);
- asks the firmware `info` and compares the ELF hash it reports with the
  one in the image just written. **A match is the only "done"** -- it
  proves the chip runs that image, not merely that esptool finished.

Zeitlos does not need to be told. The reset after flashing looks to
`net` like a module that rebooted: it sees a second `HELLO`
(`esp32link: HELLO again ... ESP32 reset? resending STA`) and sends the
WiFi credentials again. The link comes back by itself in a few seconds.

Note: `--image` writes the merged file from 0x0, which includes the
empty NVS partition. The firmware keeps nothing there that matters
(credentials come from Zeitlos at every boot); the radio recalibrates
once on the next start. Flashing a local build writes only the
bootloader, partition table and app, and leaves NVS alone.

### When it goes wrong

| symptom | almost always |
|---|---|
| `no Kabellos ... found on USB` | the C3 is in reset: Zeitlos has not started `net` yet, or the bitstream is not `lakritz_kabellos`. Or a charge-only cable |
| `more than one ESP32-C3 on USB` | another C3/S3 board is attached; say `--port` |
| `is an image for esp32, not esp32c3` | the ULX3S build or image; use `zeitlos-nic-kabellos.bin` |
| `the build is older than ...` | rebuild (`--build`), or `--force-stale` if that is intended |
| flashed, but no answer to `info` | a firmware older than this change has no `info` command; or the board was restarted and Zeitlos holds the C3 in reset again. `--info` once the desktop is back |
| esptool cannot connect | something has the port open (a serial monitor). Close it |
| `Permission denied` on `/dev/ttyACM*` (Linux) | not in the `dialout` group; see "Serial port permissions" in [esp32link.md](esp32link.md) |
| the C3 drops into its bootloader by itself just after being plugged in (Linux) | ModemManager probed it. Install `tools/70-zeitlos.rules` |

### Without a lakritz_kabellos bitstream

Any other bitstream drives PMOD port A for whatever PMOD *it* expects,
so pins 1 and 2 (EN and BOOT) end up in an unknown state -- the
`lakritz_langkatze` image, for example, puts the ENC28J60's chip select
and MOSI on them. Two images leave them harmless:

- **`lakritz_kabellos`** -- the normal case, above.
- **`lakritz_gpio`** -- all eight pins are inputs with pull-ups until
  software configures them, so EN and BOOT both read high and the C3
  boots from flash and stays running. Useful for a first flash of a
  module that still has ESP-AT on it, and for the self-test below: the
  C3 is powered and on USB, and nothing in Zeitlos talks to it.

On the bench, without any host board, the module needs J1 shorted to be
powered from USB -- and then must not go back into a PMOD socket until
J1 is open again. Prefer the `lakritz_gpio` route.

## Self-test without Zeitlos

The firmware's ZTEST command line runs on the C3's USB console. Under
`lakritz_gpio` (or under `lakritz_kabellos` with the desktop up -- the
CLI answers either way):

```
python3 esp32/nic_selftest.py --board kabellos --ssid MyAP --psk secret
```

scans, associates, takes a DHCP address and pings 8.8.8.8, all on the
module. `--flash` writes the local build first. Interactively, any
serial terminal on the C3's port works (`idf.py -p <port> monitor`, or
`python -m serial.tools.miniterm <port> 115200`); type `help`. **Do not
let the terminal toggle DTR/RTS**: on this interface they are the
chip's reset and boot-mode lines. The scripts open the port with both
held low for that reason.

`info` prints one line:

```
ZTEST info board=kabellos chip=esp32c3 elf=41b4548b3eb83d02 built=Oct 11 2026_13:35:00 idf=v5.4 heap=... min=... txpower=60 sta_ip=1
```

`heap` / `min` are the free heap now and its low-water mark since boot
(see "Memory"); `txpower` is in units of 0.25 dBm and reads 0 until the
radio has started.

## Bring-up checklist

1. **Firmware.** Flash zeitlos-nic for Kabellos (above) and check
   `kabellos_flash.py --info` says `board=kabellos`.
2. **J1 open**, module in Lakritz PMOD port A, component side as the
   PMOD connector dictates (pin 1 to pin 1).
3. **Card.** `/data/net/net.cfg` with `ssid=` and `psk=` (up to four
   networks, tried in order -- [esp32link.md](esp32link.md)).
4. **Boot `lakritz_kabellos`.** On the console (Lakritz USB-C), in this
   order:
   ```
   esp32link: ESP32 released, HELLO/STA in main loop
   esp32link: HELLO fw=2 rst=1 mac 02:..
   esp32link: STA sent, 1 network(s) (try 1)
   esp32link: STA_ACK status 0
   esp32: I (2410) gw: tx power set 60 -> 60 (x0.25 dBm)
   esp32link: LINK up rssi=-52 ip=192.168.1.37
   ```
   `ip` is the module's address on your LAN, and where the remote
   desktop is served: `http://<that address>/`.
5. **Traffic.** `tget 192.168.4.1 big.bin big.bin` (256 KiB from the
   gateway's own TFTP server), then a page in `web`.
6. **Watch the module.** With the micro-USB cable in, the C3's own log
   keeps running on its console even while Zeitlos uses the link (on
   the ULX3S it goes quiet, because UART0 at 115200 cannot keep up; USB
   can). Once a minute:
   ```
   I (60123) nic: stat up=60s polls=... fromZ=... toZ=... drop=0 ctlq=0 ip=1 heap=... min=...
   ```

**If nothing after "ESP32 released":** no `HELLO` means nothing is
coming back on pin 10. Check the module is the right way round and runs
zeitlos-nic (`--info`), not ESP-AT. A module still on ESP-AT boots
fine and simply never answers in ZNIC.

## Power

Kabellos draws everything, including the radio's transmit peaks, from
the host's 3V3 rail through PMOD pins 6 and 12. The ESP32-C3's peaks
are in the low hundreds of mA, on top of what the FPGA, SDRAM and card
already take. The ULX3S's ESP32 taught the lesson: on a bus-powered hub
that board reset during sustained TFTP writes ([esp32link.md](esp32link.md),
"Power").

So the Kabellos build starts at **15 dBm** (`CONFIG_ZNIC_TX_POWER=60`)
rather than the ULX3S's 19.5. Once a Lakritz has been seen to cope --
sustained `tget` to the card with no resets, the `stat` line's `up=`
never going backwards -- raise it with `idf.py -B build-kabellos
-DSDKCONFIG=build-kabellos/sdkconfig menuconfig` ("Zeitlos NIC"), up
to 84 (21 dBm), or per build with `-DZNIC_TX_POWER=n`. Power the Lakritz
from a port that can supply the current, not a bus-powered hub.

## Memory

The C3 has about 120 KB less RAM than the ESP32, and the firmware
was written for the ESP32. Built unchanged for the C3, 106 KiB of DRAM
was left for the heap before WiFi, lwIP and roughly 80 KiB of task
stacks had taken theirs.

Most of the difference was one buffer. `screend` sends a viewer every
changed framebuffer stripe as a single WebSocket message, and built
that message in a 40 KiB buffer (30 stripes of up to 1,344 bytes). On
Kabellos the same message goes out as **WebSocket fragments** from an
8 KiB buffer (`CONFIG_ZNIC_WS_CHUNK`). The browser joins fragments
before the page's `onmessage` runs, so `web/index.html` receives the
same bytes and did not change. Each stripe is still copied whole under
the lock, and the lock is released while a fragment is on the wire.
The ULX3S build sets the option to 0 and keeps the single-frame path,
unchanged.

Static DRAM left for the heap, as built:

| | before | after |
|---|---|---|
| ESP32 (ULX3S) | 50 KiB of 176 | 50 KiB of 176 (identical layout) |
| ESP32-C3 (Kabellos) | 106 KiB of 314 | 140 KiB of 314 |

The `heap` and `min` fields of `info` and of the minute `stat` line
are the run-time answer, which nothing measured off the board can give.
**If `min` drops below roughly 20 KiB under load, report it**: WiFi
allocates its buffers dynamically, and the C3 is where that will show
first.

## Throughput

Not measured yet. The link is the same 3 Mbaud UART with the same
credit-based bursts, so the wire is not what changes; what changes is
that one 160 MHz core now runs WiFi, NAPT, lwIP and the ZNIC task
together where the ULX3S has two at 240. Expect less than the ULX3S's
figure ([net_throughput.md](net_throughput.md)), and treat this as a
measurement to make, with `tget 192.168.4.1 big.bin`.

## Gateware

`kabellos.spec` adds `UART1`, `ESP32_LINK` and `ESP32_PMOD`. The first
two are the ULX3S's link, unchanged: the 16550 at `0xf000_0100` (TX),
the 8 KiB receive FIFO at `0xf000_0300` (`rtl/esp32_rxfifo.v`), the
control register at `0xf000_0200`, feature bit 30. `ESP32_PMOD` only
declares the module's control pins as top-level ports -- on the ULX3S
they are board pins under `BOARD_ULX3S` -- under the same names, so the
one block that drives them serves both, and adds the tie-high for
pin 4.

The target drops `SPI_ETH` (the Langkatze the base Lakritz .lpf assumes
is not in the port) and sets `ESP32_RXFIFO_BITS=13`, the ULX3S 45F/85F
depth.

Utilisation and timing, Yosys 0.69 / nextpnr-ecp5 0.11.1 (YoWASP), one
run, default seed, against `lakritz_langkatze` built the same way:

| target | COMB | FF | DP16KD | `CLK_48` | pixel | bitstream |
|---|---|---|---|---:|---:|---:|
| `lakritz_langkatze` | 22,395 / 24,288 (92%) | 9,911 | 39 / 56 | 55.37 MHz | 49.84 MHz | 515,750 B |
| `lakritz_kabellos` | 22,727 / 24,288 (**93%**) | 10,032 | 43 / 56 | 53.03 MHz | 49.06 MHz | 531,910 B |

So the link costs about 330 LUT4, 120 flip-flops and the four block
RAMs of the 8 KiB FIFO, net of the SPI master it replaces, and closes
48 MHz with 5 MHz to spare on the first seed. Both builds' synthesis
logs carry the same set of warnings. It is still a 93% design on a
25F: re-check `make timing` after anything that grows the SOC, and if
it ever stops placing, UART1 (a full 16550 used only to transmit) is
the first place to look.

## Later

- **Hardware flow control.** Pin 8 is the C3's CTS. Driving it from
  the receive FIFO's fill level would give byte-level backpressure on
  top of the poll credit. Neither link needs it so far; it is the
  first thing to try if `crc_err` counts climb under load.
- **Flashing from Zeitlos.** One more control-register bit could route
  UART1 to pins 3/4, the C3's ROM bootloader, and a Zeitlos app could
  then update the module from the sdcard with no other computer --
  reusing the UART and FIFO already built.
- **Other boards.** Any board spec with a PMOD port takes
  `pmods = kabellos@<port>` and `-SPI_ETH` (or whatever NIC it had),
  plus an `ESP32_RXFIFO_BITS` that fits its block RAM. Obst, Minze and
  Schoko, or a ULX3S with a second WiFi module in J1, are each a target
  spec and a build.
