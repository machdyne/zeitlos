# ESP32 network link (ULX3S, Kabellos)

The same link runs on two kinds of hardware: the ESP32 soldered to the
ULX3S, which this page describes, and an ESP32-C3 on a
[Kabellos](kabellos.md) PMOD (`lakritz_kabellos`). The protocol, the
gateware blocks, `net` and the firmware source are shared; pins,
console, flashing and a few firmware settings differ, and
[kabellos.md](kabellos.md) covers those.

The ULX3S has no Ethernet. It has an ESP32 wired to the FPGA over two
UARTs and a few GPIOs, so on this board Zeitlos's `net` uses a third
PHY backend: the ESP32 runs its own firmware
(`esp32/zeitlos-nic`) and hands Ethernet frames back and forth over
UART1. Zeitlos keeps its own MAC, IP stack, ARP, DNS, TFTP and telnet;
802.11 exists only on the ESP32. This is the same shape as the ENC28J60
and RMII backends (`docs/networking.md`), with a serial link instead of
SPI or RMII underneath.

Tested on a ULX3S 85F v3.0.8 (2026-08): association, DNS through the
gateway, TFTP in both directions (256 KiB into the SD card), HTTPS
downloads via the gateway's TFTP server.

## Hardware

| FPGA pin | ESP32 | Use |
|----------|-------|-----|
| L1 / N3 (`UART1_TX` / `UART1_RX`) | GPIO16 / GPIO17 (UART1) | ZNIC data, 3 Mbaud 8N1 |
| F1 `wifi_en`, L2 `wifi_gpio0` | EN, GPIO0 | reset / boot strap, software controlled |
| K3 / K4 | UART0 | flashing and the `ZTEST` CLI (only with a passthru bitstream) |
| SD card lines | GPIO 2/4/12-15 | left as inputs by the firmware |

`rtl/boards.vh` defines `UART1` and `ESP32_LINK` for `BOARD_ULX3S`.

On Kabellos the ZNIC pair is the C3's GPIO6/7 on PMOD pins 9/10, EN and
the GPIO9 boot strap are PMOD pins 1/2, and the console is the C3's own
USB; the PMOD spec adds `ESP32_PMOD` as well, which declares those
control pins as ports instead of ULX3S board pins. The full pinout is
in [kabellos.md](kabellos.md).

## SOC side

- `UART1`: a second 16550 at `0xf000_0100` (TX only in practice).
- `0xf000_0200`: ESP32 control register, bit 0 = EN, bit 1 = GPIO0.
  Reset value `en=0, gpio0=1`: the module is held in reset until `net`
  starts, so it never fights the FPGA for the SD card bus. (On Kabellos
  bit 1 is the C3's GPIO9 strap, with the same meaning, and there is no
  shared bus -- but the module is held in reset until `net` the same
  way.)
- `0xf000_0300`: `rtl/esp32_rxfifo.v`, an 8 KiB block-RAM receive FIFO
  on the UART1 RX pin. `+0` = `{overrun, count[13:0]}`, `+4` pops a
  byte, a write to `+8` flushes. The 16550's 16-byte FIFO cannot hold a
  frame while another process owns the CPU (one time slice away from
  the CPU is ~400 bytes at 1 Mbaud), so the driver reads replies from
  here with interrupts enabled and never loses a late one.
- CSR feature bit 30 (`Z_FEATURE_ESP32_LINK`, `rtl/csrs.vh`,
  `sw/common/zsoc.h`). `net_phy_select()` (`sw/apps/net/net_phy.c`)
  picks this backend when the bit is set, exactly as it picks RMII or
  the ENC28J60 elsewhere -- one `net.bin` runs on every board.

## ZNIC protocol

UART is a byte stream, so frames are delimited:

```
0x7E 0x5A | ver:u8 | type:u8 | n:u16le | payload[n] | crc16-ccitt(ver..payload)
```

| type | direction | payload |
|------|-----------|---------|
| `0x03` RX_POLL | Z -> ESP32 | optional credit byte: how many frames the FPGA's FIFO can take |
| `0x01` DATA | both | one 802.3 frame (<= 1518 bytes) |
| `0x22` DATA_ACK | ESP32 -> Z | -- |
| `0x02` NOP | ESP32 -> Z | nothing queued |
| `0x10` HELLO | ESP32 -> Z | MAC[6], flags = `esp_reset_reason()`, fw version (2) |
| `0x20` STA | Z -> ESP32 | `ssid_len, ssid[], psk_len, psk[]` |
| `0x21` STA_ACK | ESP32 -> Z | status (0 = ok) |
| `0x11` LINK | ESP32 -> Z | up, rssi, disconnect reason, scan found, station IP (4 bytes, big-endian) |
| `0x30` LOG | ESP32 -> Z | one ESP_LOG line (shown as `esp32: ...`) |
| `0x31` BURST | ESP32 -> Z | `{n}`: exactly n framed messages follow back to back; sent when the poll carried credit >= 2 and n things were queued |
| `0x32` INPUT | ESP32 -> Z | `{usage, mods, pressed}`: one synthetic key event from a remote desktop viewer, injected into the HID ring |
| `0x33` MOUSE | ESP32 -> Z | `{x_lo, x_hi, y_lo, y_hi, buttons}`: pointer position in framebuffer coordinates. `buttons[2:0]` = left/right/middle; bit 7 = the pointer left the viewer |
| `0x34` VIEWERS | ESP32 -> Z | `{n}`: how many WebSocket clients `screend` holds. Sent whenever the number changes and once a second regardless, so net's idea of whether anybody is watching is right within a second of a lost message or a restart. Older firmware never sends it, and net then falls back to counting `screend`'s `viewer connected/gone` log lines |

Every ESP32 -> Zeitlos frame is a reply (fw version 2): `RX_POLL` is
answered with the oldest queued control message (HELLO, LINK, LOG),
else a DATA frame, else NOP; `DATA` gets `DATA_ACK`; `STA` gets
`STA_ACK`. The driver (`sw/apps/net/esp32link.c`) drains anything
already in the FIFO before sending and matches replies in order, so a
reply that arrives late (the ESP32's tcpip and wifi tasks can delay
the link task by tens of ms) is simply the next thing read.

`INPUT` and `MOUSE` are the two exceptions, and they are exceptions
because they are *input*: a keystroke that waited for the next poll
would be a keystroke with visible latency. They go out unsolicited, and
they can, for the same reason the burst machinery can -- the 8 KiB
receive FIFO holds them, and an arrival there raises the same
`cpu_irq[8]` a wired MAC raises, which wakes `net` at once. See
[remote_desktop.md](remote_desktop.md) for where they come from.

Two things keep the wire busy instead of waiting on round trips.
`RX_POLL` carries a credit -- how many frames the receive FIFO can
take, worst-case MTU each -- and with credit to spare the ESP32
answers with one `BURST` of everything it has queued rather than one
frame per poll. In the other direction DATA is windowed, not
stop-and-wait: up to four frames in flight, each still answered by a
`DATA_ACK`, and a window that times out is written off as lost -- an
ethernet link is allowed to drop frames, and TCP or TFTP retransmit.
Note TFTP itself is lockstep either way (one 512-byte block per
acknowledgement end to end), so its throughput barely notices any of
this; the machinery is for streaming consumers.

The driver adds one optional entry to `net_phy_t`, `poll_wifi()`:
wired backends leave it NULL, and `net`'s main loop calls it when it
is set, so association happens without blocking `phy_init()` (an
association takes seconds and `wm` has to keep running).

## Bring-up from Zeitlos

1. Bitstream holds the ESP32 in reset; `init` starts `net` last (after
   every SD load).
2. `net` reads `/data/net/net.cfg` on the SD card:
   ```
   ssid=MyAP
   psk=secret
   dhcp=0
   ip=192.168.4.2
   mask=255.255.255.0
   gw=192.168.4.1
   dns=8.8.8.8
   ```
3. `net` releases EN, gets `HELLO`, sends `STA`, and reports
   `esp32link: LINK up rssi=...` when the ESP32 has an address. The
   ESP32's own log lines appear on the console as `esp32: ...`, and a
   counter line (`esp32link: hello=... polls=... crc_err=...`) every
   15 s.

The ESP32 is the gateway (192.168.4.1) and masquerades Zeitlos behind
its station address (esp-lwip NAPT is enabled on the *inside* netif --
`ip4_forward()` translates when the output netif has `napt == 0`).
Outbound TCP/UDP/ICMP work; inbound connections to Zeitlos do not, and
TFTP against a server on the LAN does not either (NAPT is symmetric and
TFTP replies come from a fresh port). Use the gateway's own TFTP
server instead:

```
tget 192.168.4.1 test.bin test.bin                      32 KiB pattern
tget 192.168.4.1 big.bin big.bin                        256 KiB pattern
tput 192.168.4.1 somefile somefile                      upload, counted
tget 192.168.4.1 https://www.ietf.org/rfc/rfc20.txt rfc20.txt
```

The last form makes the ESP32 fetch the URL (HTTP or HTTPS, redirects
followed) and stream it as the TFTP file; the local name must be 8.3.
The TFTP client waits ~20 s for the first block to cover the TLS
handshake.

## The remote desktop

The firmware also serves the desktop itself. `net` streams the
framebuffer to the gateway over UDP, `screend.c` relays it to browsers
over a WebSocket, and the page sends keyboard and mouse back as the
`INPUT` and `MOUSE` messages above -- so a ULX3S with no monitor and no
USB HID attached is still usable, over the same link and the same
firmware. See [remote_desktop.md](remote_desktop.md).

## Power

The ESP32 radio at full power plus the FPGA plus SD card writes exceed
what a bus-powered USB hub port delivers: on a dock's hub the board
dropped off the bus during sustained TFTP writes to the SD card.
Connect the ULX3S to a port that can supply the current (a direct port
on the host, or external 5 V). At 19.5 dBm on a direct port nothing
resets.

## Installing ESP-IDF

The firmware (`esp32/zeitlos-nic`, both boards) is built with Espressif's
ESP-IDF, **v5.4 exactly**. Not "5.x": `screend.c` finishes the WebSocket
handshake itself through a header that is private to the IDF's HTTP
server (`main/CMakeLists.txt` says why), and private headers change
between releases. A newer IDF may build, or may not; v5.4 is the one
this tree is built and tested with.

You need it to build the firmware, and for `zrelease build` of a release
with a Kabellos target. You do **not** need it to flash a prebuilt image:
for that, see "Without ESP-IDF" below.

### Prerequisites

Debian / Ubuntu:

```
sudo apt install git wget flex bison gperf python3 python3-pip python3-venv \
                 cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

macOS (Homebrew; Python 3 from Homebrew or python.org):

```
brew install cmake ninja dfu-util
```

Other systems: Espressif's own list, in the ESP-IDF v5.4 "Get Started"
guide for Linux and macOS.

### Install

```
mkdir -p ~/esp
git clone -b v5.4 --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32,esp32c3
```

- **`~/esp/esp-idf`** is where `esp32/nic_build.py` and `zrelease` look
  when nothing else says where the IDF is, so installing there means no
  configuration at all. Anywhere else works too: set `IDF_PATH`, or pass
  `--idf <path>`.
- **`esp32,esp32c3`** installs the compilers for both chips: Xtensa for
  the ULX3S's ESP32, RISC-V for Kabellos's ESP32-C3. `install.sh` with
  no argument installs every chip Espressif makes, several GB more.
- **`--depth 1 --shallow-submodules`** keeps the checkout around 600 MB
  instead of several GB of history. Leave it off if you want the history.
- The toolchains and a Python virtual environment go in
  `~/.espressif`, not in the checkout.

### Use

```
. ~/esp/esp-idf/export.sh            # once per shell; puts idf.py on PATH
idf.py --version                     # should say v5.4
python3 esp32/nic_build.py           # both boards
```

`esp32/nic_build.py` and `esp32/flash.py --build` do not need the
`export.sh` step. With `idf.py` already on `PATH` they use it; otherwise
they find the IDF at `--idf` (nic_build), `IDF_PATH` or `~/esp/esp-idf`,
and take its environment from `idf_tools.py export` -- deliberately not
from `export.sh`, which under the bash 3.2 every stock macOS ships
aborts half way, prints "Done!" anyway and leaves `idf.py` off `PATH`.
`idf.py` run by hand does need `export.sh`; on macOS run it from zsh,
the default shell, rather than `bash`.

If you have more than one IDF version installed, the scripts use the
Python environment install.sh made for the checkout's own version
(`~/.espressif/python_env/idf5.4_*_env`).

### If install.sh fails

**`Failed to download ... espidf.constraints.v5.4.txt`.** `install.sh`
fetches a Python constraints file from `dl.espressif.com`; behind a
proxy or firewall that blocks that host (GitHub, where the compilers
come from, being reachable), install without it:

```
cd ~/esp/esp-idf
python3 tools/idf_tools.py install --targets esp32,esp32c3
python3 tools/idf_tools.py install-python-env --no-constraints
export IDF_PYTHON_CHECK_CONSTRAINTS=no         # in every shell that uses it
```

Without the constraints, pip picks the newest Python packages, and the
newest IDF component manager no longer speaks v5.4's build protocol --
cmake fails with `argument --interface_version: invalid choice: '3'`.
zeitlos-nic uses no managed components, so switch the manager off:

```
export IDF_COMPONENT_MANAGER=0
```

Both variables are needed together, and only on this path; a normal
`install.sh` install needs neither.

**`idf.py: command not found`.** `export.sh` was not sourced in this
shell (or, on macOS under bash 3.2, it was and failed quietly) -- or use
`nic_build.py`, which needs neither.

**`no ESP-IDF 5.4 python environment`.** The checkout is there but
`install.sh` has not been run for it, or was run for a different
checkout. Run `./install.sh esp32,esp32c3` in this one.

**A build that fails in `esp_http_server` headers.** Wrong IDF version:
`idf.py --version` should say `v5.4`. `git -C ~/esp/esp-idf checkout
v5.4 && git -C ~/esp/esp-idf submodule update --init --recursive`, then
`./install.sh esp32,esp32c3` again.

### Without ESP-IDF

Flashing a prebuilt image (a release's `zeitlos-nic-kabellos.bin`) needs
only esptool and pyserial:

```
python3 -m pip install esptool pyserial
python3 esp32/kabellos_flash.py --image zeitlos-nic-kabellos.bin
```

esptool 4.x and 5.x both work. 5.x prints deprecation warnings for the
`write_flash` / `default_reset` spellings the scripts use; they are kept
because 4.x -- the version IDF v5.4 itself installs -- knows no others.

### Serial port permissions (Linux)

The ESP32-C3 on Kabellos appears as `/dev/ttyACM*` (USB ID `303a:1001`),
and the ULX3S's FTDI as `/dev/ttyUSB*`. Both belong to the `dialout`
group (`uucp` on Arch): `sudo usermod -aG dialout $USER`, then log in
again.

For Kabellos also install the project's udev rules:

```
sudo cp tools/70-zeitlos.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

They now cover the C3 as well as the Zeitlos console: they keep
ModemManager away from it -- ModemManager probes new ACM devices, and
on this chip toggling DTR/RTS *is* the reset and boot-mode control, so
a probe can leave it sitting in its ROM bootloader -- and add a stable
`/dev/kabellos` name.

## Firmware

`esp32/zeitlos-nic` (ESP-IDF v5.4 -- see "Installing ESP-IDF" above),
built with `idf.py build`, or for
either board or both with `esp32/nic_build.py [ulx3s|kabellos]`. The
board follows from the IDF target (`main/Kconfig.projbuild`): `esp32`
is the ULX3S and keeps `build/` and the project's `sdkconfig`;
`esp32c3` is Kabellos, in `build-kabellos/` with its own sdkconfig.
Each build also writes a merged image for offset 0
(`build/zeitlos-nic-ulx3s.bin`, `build-kabellos/zeitlos-nic-kabellos.bin`).

Settings that differ by board, all in `main/Kconfig.projbuild`:

| | ULX3S | Kabellos |
|---|---|---|
| `ZNIC_UART_TXD` / `RXD` | 17 / 16 | 7 / 6 |
| `ZNIC_SD_PINS_HIZ` | y (GPIO 2/4/12-15 are the microSD) | n (GPIO12-17 are the C3's flash) |
| `ZNIC_TX_POWER` | 78 (19.5 dBm) | 60 (15 dBm) |
| `ZNIC_WS_CHUNK` | 0 (one WebSocket frame per update) | 8192 (the same message, fragmented) |

The ULX3S build's configuration is unchanged by the Kabellos work apart
from those symbols appearing, and its memory layout is identical.

The rest of this section is the ULX3S. Kabellos is flashed over its own
USB, in place, with `esp32/kabellos_flash.py` -- see
[kabellos.md](kabellos.md), "Flashing".

**Reflashing it is a procedure, not a command**, and the order is what
matters: the ESP32's UART0 only reaches USB through a passthru
bitstream, the sdcard has to come out because its DAT0 line is the
chip's boot strap, that means two power cycles, and each of those wipes
the FPGA's SRAM -- which holds the ESP32 in reset until the bitstream
is loaded again. `esp32/flash.py` walks through it, checks what it is
about to write, and checks the result; **its README is the reference**,
including what to do when `esptool` says `Wrong boot mode detected`.

`esp32/nic_selftest.py` is the other half: it drives the `ZTEST` CLI on
the ESP32's own console under the same passthru bitstream, exercising
scan, association, DHCP and ping (`test <ssid> <psk>`) plus the gateway
(`inject gw|ping|dns`, `poll`, `sta2`) with no sdcard and no Zeitlos
running at all. `info` prints the board, chip and the running image's
ELF hash, plus free heap; `kabellos_flash.py` uses it to check what it
flashed. `nic_selftest.py --board kabellos` runs the same test over the
C3's USB console.
