# ULX3S

[ULX3S](https://radiona.org/ulx3s/) with an ECP5 -- 85F tested, and
the same LPF covers 12F/25F/45F. Build with the device you have:

```
make BOARD=ulx3s DEVICE=85k prog       # bitstream to SRAM
make BOARD=ulx3s DEVICE=85k flash_os   # kernel to SPI flash at 1MiB
```

`DEVICE` defaults to `25k`. `PNR_SEED` defaults to 10 on this board:
the design sits close to 48MHz on the 85F and which placement seed
meets it is luck -- the spread is roughly 45-50MHz and nextpnr's own
default is among the ones that miss. A bitstream that misses timing
programs fine and then misbehaves intermittently, so a seed known to
meet it is pinned in the Makefile. Re-check with `make BOARD=ulx3s
DEVICE=85k timing` after changing RTL or pins.

Close the serial port before programming -- the FT231X does not share
JTAG and UART. The console is 1000000 8N1.

## Building, programming and bringing up

`make BOARD=ulx3s DEVICE=85k flash` does the lot -- gateware, BIOS,
kernel, boot splash and the core apps, all written to the SPI flash --
after which the board boots to a desktop with nothing attached. The
pieces, for when you want one at a time:

| target | writes | where |
|--------|--------|-------|
| `prog` | the bitstream | the FPGA's configuration **SRAM** |
| `flash_soc` | the bitstream | flash, offset 0 |
| `flash_logo` | the boot splash | flash, just below 1MB |
| `flash_os` | `sw/os/kernel.bin` | flash, **1MB** |
| `flash_apps` | `output/ulx3s/apps.zar` -- `wm`, `net`, `term` | flash, **1.25MB** |

`dev-flash` rebuilds and rewrites only the kernel and the core apps,
which is the loop for working on the OS. See
[flash_apps.md](flash_apps.md) for the archive format, and for why
`repl` is not in it.

### The bitstream in SRAM takes the ESP32 with it

`prog` writes configuration SRAM, and **every power cycle erases it**.
Unplugging the USB cable counts, and so does changing the sdcard, since
the card cannot be pulled live. The board comes back blank.

On this board that is not merely "no Zeitlos". The ESP32's EN and GPIO0
are driven by the FPGA (`wifi_en` at F1 and `wifi_gpio0` at L2,
`boards/ulx3s.lpf`), and they reset to "held in reset" so the module
never fights the FPGA for the shared sdcard pins. A blank FPGA is
therefore an ESP32 in reset: no WiFi, no remote desktop, nothing
answering on the LAN.

So: **a board that "vanished from the network" after being unplugged
has almost certainly just lost its bitstream.** Load it again before
looking at anything else -- DHCP, the access point, the subnet. It is
the cheapest check and nearly always the answer.

`flash_soc` ends that once the board is doing something you want to
keep. During bring-up, SRAM is faster and cannot leave you with a board
that boots into something broken.

### The console

The 1 Mbaud console on the FT231X is the **SOC's** UART0 (`UART0_TX` at
L4, `UART0_RX` at M1) -- the BIOS, then the kernel shell. It is not the
ESP32's console: that is the ESP32's own UART0 on K3/K4, which reaches
USB only under a passthru bitstream ([esp32link.md](esp32link.md)).
Opening this port cannot reset the ESP32, and cannot reset the FPGA
either -- nothing on this board wires DTR or RTS to it. What it can do
is cancel autoboot, because the BIOS reads any byte arriving during its
countdown as a keypress; see the `minicom -o` note in the README.

With no display attached the console is how you find the board: `net`
prints the address the access point gave the ESP32 (`esp32link: LINK up
rssi=... ip=...`, and the firmware's own `esp_netif_handlers: sta ip:`
before it), which is where the [remote desktop](remote_desktop.md)
lives. It also writes it to `NET.IP` at the root of the card.

`pr` lists the processes that started, which answers "did the desktop
come up at all" without a display; `mount` and `ls` answer the same for
the card.

### The sdcard

Optional -- the core apps are in flash -- and otherwise the standard
image ([prebuilt](../README.md#quick-start-prebuilt-images), or
`tools/mkfatimg.sh`): apps in `apps/`, `zeitlos.cfg` at the root
([config.md](config.md)), `web/roots.der` for TLS, and `docs/`, `ark/`,
`libz/`, `user/`.

The one file this board wants that others do not is **`NET.CFG`** at
the root, holding the WiFi credentials `net` hands the ESP32. Format
and the rest of the link in [esp32link.md](esp32link.md); flashing the
ESP32 itself is `esp32/zeitlos-nic/README.md`, and is its own
procedure, because the card has to come out for it.

## What works

SDRAM (32MB), HDMI, the microSD card, both USB HID ports, audio over
S/PDIF and networking through the onboard ESP32
([esp32link.md](esp32link.md)) -- the board has no Ethernet at all, so
`net` uses the ESP32 as its NIC.

## USB HID ports

| Port | Where | D+ | D- |
|------|-------|----|----|
| 0 | onboard **US2** micro-USB | D15 | E15 |
| 1 | **J2** header, GP26/GN26 | B13 | C13 |

Port 0 needs a micro-B OTG adapter (micro-B male to USB-A female, with
data lines -- not a charge-only cable); the board supplies VBUS. The
`usb_fpga_pu_dp/dn` pins hold US2's own resistors in the host
configuration, which the SOC does for you.

Port 1 has no connector on the board. A USB-A socket wired to four J2
pins gives you one:

| USB-A | J2 pin | Label | FPGA |
|-------|--------|-------|------|
| 1 VBUS | 40 | 5V OUT | -- |
| 4 GND | 38 | GND | -- |
| 3 **D+** | 34 | GP26 | B13 |
| 2 **D-** | 33 | GN26 | C13 |

Plus a **15k pulldown from each data line to GND**: that is how a USB
host sees a device arrive, and which line goes high is how it learns
the device's speed. No pullups.

Two details are deliberate. GP26 and GN26 are the two halves of one
differential pair, routed as a pair on the PCB and facing each other on
the header (pins 33 and 34), so they carry USB better than two pins of
the same row would -- not that it matters much at 1.5 Mbps. And D+ has
to be the GP side: `usb_hid_host` reads the device's speed from which
line the device pulls high, and it only speaks low speed, where that
line is D-.

That core takes exactly one device per port, no hubs, and only
boot-protocol HID: an ordinary wired keyboard or mouse enumerates, a
2.4GHz receiver or anything full-speed (a Logitech MX Master, say)
does not.

## Power

Bus power through a hub port is not enough for the FPGA, the radio and
sustained SD card writes at the same time -- the board drops off the
USB bus mid-transfer. Use a port that can supply the current, or
external 5V.
