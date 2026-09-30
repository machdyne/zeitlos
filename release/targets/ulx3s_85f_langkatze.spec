# ULX3S 85F with a Langkatze SPI ethernet PMOD in port j1.
#
# Same board as ulx3s_85f, with the onboard ESP32 link left out of
# the gateware. That is why `-ESP32_LINK` is here.
#
# sw/apps/net/net_phy.c's net_phy_select() takes the ESP32 whenever
# the feature CSR says it is present, and only then looks for
# SPI_ETH. A bitstream that built both would program the ENC28J60
# and never use it. langkatze.spec adds `SPI_ETH; this line takes
# `ESP32_LINK back off. release/lib/spec.py's nic() rejects the two
# together for the same reason, so the line is also what lets the
# spec load.
#
# WHAT THIS COSTS: no remote desktop. That server is the ESP32's.
# HDMI, the serial console and the microSD stay. UART0 is the FTDI
# on L4/M1, not a pin of this header, so unlike lakritz_langkatze
# this target does not say `-UART0`.
#
# WHAT DOES NOT HAVE TO COME OUT WITH IT:
#
#   `ESP32_RXFIFO_BITS stays. rtl/sysctl.v reads it only inside
#   `ifdef ESP32_LINK, as the receive FIFO's DEPTH_BITS. With the
#   link off the FIFO is not built, so the macro is never expanded
#   and costs no block RAM. ulx3s_12f.spec overrides the value
#   because that target still builds the FIFO.
#
#   The wifi_en / wifi_gpio0 constraints stay. Those ports are
#   declared under `ifdef BOARD_ULX3S, which the Makefile passes
#   on every ULX3S build, not under `ESP32_LINK. Dropping them
#   would be a declared port with no LOCATE, and nextpnr rejects
#   that. They are F1 and L2, not balls of port j1, so taking the
#   port over does not release them either.
#
#   The ESP32 stays in reset, and that is already what the
#   gateware does. Under `ifndef ESP32_LINK, rtl/sysctl.v drives
#   wifi_en and wifi_gpio0 low so the ESP32 cannot contend for the
#   SD pins it shares with the FPGA. Nothing in this bitstream
#   writes those pins back, which is the whole of "nobody releases
#   it from reset".
#
# UART1 stays. Its pins (L1, N3) are the ESP32 data pair and are
# not in port j1, so they stay constrained and the port stays
# built. One consequence, from rtl/csrs.vh: FEATURES2 bit 1 means
# "there is a UART1 and it is yours" exactly when `ESP32_LINK is
# absent, so this bitstream advertises a general-purpose UART1
# whose balls are still soldered to an ESP32 held in reset.
# Nothing opens it at boot. `-UART1` would clear the bit; the two
# constraints left behind in the base .lpf are the sort the
# checker already tolerates.

description = ULX3S 85F + Langkatze SPI Ethernet PMOD

base = ulx3s

make_vars = DEVICE=85k

pmods = langkatze@j1

defines = -ESP32_LINK
