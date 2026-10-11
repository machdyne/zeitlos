# Lakritz with a Kabellos WiFi PMOD in port A.
#
# The ULX3S's ESP32 network link (docs/esp32link.md) on a board that has
# no ESP32 of its own: the kabellos PMOD spec adds `UART1, `ESP32_LINK
# and `ESP32_PMOD, and sw/apps/net picks the ESP32 backend from the
# feature CSR exactly as it does on the ULX3S. Same net.bin, same ZNIC
# protocol, same remote desktop (docs/remote_desktop.md) -- served from
# the C3 -- and the same NAPT limits. docs/kabellos.md has the rest.
#
# The module needs zeitlos-nic built for the C3, not the ESP-AT
# firmware it ships with. The release carries it
# (zeitlos-nic-kabellos.bin); esp32/kabellos_flash.py writes it over
# the module's own USB with the module still in this port.
#
# -- -SPI_ETH --
#
# boards/lakritz_v0.lpf assumes a Langkatze in port A, which is why the
# base board defines `SPI_ETH. Kabellos occupies that port, its balls
# are released, and the define has to go with them -- the same
# reasoning as lakritz_gpio and lakritz_katze. Leaving it would also be
# two NICs, which release/lib/spec.py's nic() rejects.
#
# -- ESP32_RXFIFO_BITS=13 --
#
# 8 KiB, the ULX3S 45F/85F value: a time slice away from the CPU at 3
# Mbaud (~1200 bytes) with room for a credit burst of ~5 MTU frames
# (rtl/boards.vh, BOARD_ULX3S). Four DP16KD. Lakritz has them: 38 of 56
# in use on lakritz_gpio, and the SPI master this target drops cost
# none. Only the 12F ULX3S, short of block RAM, goes down to 11.
#
# -- The fabric --
#
# A full 16550 (UART1) plus the receive FIFO and the control register,
# in place of the SPI master `SPI_ETH builds. The 25F is the tight one
# here (lakritz_gpio is at 96% of LUT4s), so this is the target to
# re-check with `make timing` after anything that grows the SOC;
# docs/boards.md has its numbers. UART1 is used for TX only (receive is
# rtl/esp32_rxfifo.v), so if it ever stops fitting, a TX-only UART in
# its place is the obvious place to find the room.
#
# -- The console --
#
# Not an issue: `USB_CDC is on for Lakritz, so the console is the USB-C
# socket and Kabellos taking port A costs nothing there. With both USB
# cables in, the host sees two serial devices -- the Lakritz console and
# the C3's -- which is why esp32/kabellos_flash.py finds the C3 by USB
# ID rather than by guessing.

description = Lakritz + Kabellos ESP32-C3 WiFi PMOD

base  = lakritz
pmods = kabellos@a

defines =
	-SPI_ETH
	ESP32_RXFIFO_BITS=13
