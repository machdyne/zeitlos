# ESP32 network link (ULX3S)

The ULX3S has no Ethernet. It has an ESP32 wired to the FPGA over two
UARTs and a few GPIOs, so on this board Zeitlos's `net` uses a third
PHY backend, `NET_PHY=ESP32LINK`: the ESP32 runs its own firmware
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
| L1 / N3 (`UART1_TX` / `UART1_RX`) | GPIO16 / GPIO17 (UART1) | ZNIC data, 1 Mbaud 8N1 |
| F1 `wifi_en`, L2 `wifi_gpio0` | EN, GPIO0 | reset / boot strap, software controlled |
| K3 / K4 | UART0 | flashing and the `ZTEST` CLI (only with a passthru bitstream) |
| SD card lines | GPIO 2/4/12-15 | left as inputs by the firmware |

`rtl/boards.vh` defines `UART1` and `ESP32_LINK` for `BOARD_ULX3S`.

## SOC side

- `UART1`: a second 16550 at `0xf000_0100` (TX only in practice).
- `0xf000_0200`: ESP32 control register, bit 0 = EN, bit 1 = GPIO0.
  Reset value `en=0, gpio0=1`: the module is held in reset until `net`
  starts, so it never fights the FPGA for the SD card bus.
- `0xf000_0300`: `rtl/esp32_rxfifo.v`, a 2 KiB block-RAM receive FIFO
  on the UART1 RX pin. `+0` = `{overrun, count[11:0]}`, `+4` pops a
  byte, a write to `+8` flushes. The 16550's 16-byte FIFO cannot hold a
  frame while another process owns the CPU (one time slice away from
  the CPU is ~400 bytes at 1 Mbaud), so the driver reads replies from
  here with interrupts enabled and never loses a late one.
- CSR feature bit 25 (`Z_FEATURE_ESP32_LINK`, `rtl/csrs.vh`,
  `sw/common/zsoc.h`): `net` exits cleanly on a bitstream without it.

## ZNIC protocol

UART is a byte stream, so frames are delimited:

```
0x7E 0x5A | ver:u8 | type:u8 | n:u16le | payload[n] | crc16-ccitt(ver..payload)
```

| type | direction | payload |
|------|-----------|---------|
| `0x03` RX_POLL | Z -> ESP32 | -- |
| `0x01` DATA | both | one 802.3 frame (<= 1518 bytes) |
| `0x22` DATA_ACK | ESP32 -> Z | -- |
| `0x02` NOP | ESP32 -> Z | nothing queued |
| `0x10` HELLO | ESP32 -> Z | MAC[6], flags = `esp_reset_reason()`, fw version (2) |
| `0x20` STA | Z -> ESP32 | `ssid_len, ssid[], psk_len, psk[]` |
| `0x21` STA_ACK | ESP32 -> Z | status (0 = ok) |
| `0x11` LINK | ESP32 -> Z | up, rssi, disconnect reason, scan found |
| `0x30` LOG | ESP32 -> Z | one ESP_LOG line (shown as `esp32: ...`) |

Every ESP32 -> Zeitlos frame is a reply (fw version 2): `RX_POLL` is
answered with the oldest queued control message (HELLO, LINK, LOG),
else a DATA frame, else NOP; `DATA` gets `DATA_ACK`; `STA` gets
`STA_ACK`. The driver (`sw/apps/net/esp32link.c`) drains anything
already in the FIFO before sending and matches replies in order, so a
reply that arrives late (the ESP32's tcpip and wifi tasks can delay
the link task by tens of ms) is simply the next thing read.

## Bring-up from Zeitlos

1. Bitstream holds the ESP32 in reset; `init` starts `net` last (after
   every SD load).
2. `net` reads `NET.CFG` (root of the SD card, 8.3 name):
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

## Power

The ESP32 radio at full power plus the FPGA plus SD card writes exceed
what a bus-powered USB hub port delivers: on a dock's hub the board
dropped off the bus during sustained TFTP writes to the SD card.
Connect the ULX3S to a port that can supply the current (a direct port
on the host, or external 5 V). At 19.5 dBm on a direct port nothing
resets.

## Firmware

`esp32/zeitlos-nic` (ESP-IDF 5.4). See its README for building,
flashing (through a passthru bitstream) and the `ZTEST` self-test
(`esp32/nic_selftest.py`), which exercises scan/association/DHCP/ping
and the gateway (`inject gw|ping|dns`, `poll`, `sta2`) with the ESP32's
console attached.
