# ULX3S

[ULX3S](https://radiona.org/ulx3s/) with an ECP5 -- 85F tested, and
the same LPF covers 12F/25F/45F. Build with the device you have:

```
make BOARD=ulx3s DEVICE=85k prog       # bitstream to SRAM
make BOARD=ulx3s DEVICE=85k flash_os   # kernel to SPI flash at 1MiB
```

`DEVICE` defaults to `25k`. `PNR_SEED` defaults to 6 on this board:
the design sits close to 48MHz on the 85F and which placement seed
meets it is luck -- the spread is roughly 45-50MHz and nextpnr's own
default is among the ones that miss. A bitstream that misses timing
programs fine and then misbehaves intermittently, so a seed known to
meet it is pinned in the Makefile. Re-check with `make BOARD=ulx3s
DEVICE=85k timing` after changing RTL or pins.

Close the serial port before programming -- the FT231X does not share
JTAG and UART. The console is 1000000 8N1.

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
