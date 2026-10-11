# Kabellos -- ESP32-C3-WROOM-02 WiFi PMOD (V2).
#
# github.com/machdyne/kabellos, machdyne.com/product/kabellos-wireless-pmod/
#
# The ULX3S's network link, on a PMOD. The ULX3S has an ESP32 soldered
# to it and Zeitlos reaches the network through it over UART1
# (docs/esp32link.md); Kabellos is the same idea as a module, so this
# spec adds exactly what the ULX3S board block has -- `UART1 and
# `ESP32_LINK -- plus `ESP32_PMOD, which declares the module's control
# pins as ports here instead of as ULX3S board pins (rtl/sysctl.v).
# sw/apps/net picks the ESP32 backend from the feature CSR as it does on
# the ULX3S; nothing in software knows which board it is on.
#
# The module must run zeitlos-nic built for Kabellos, NOT the ESP-AT
# firmware it ships with: esp32/nic_build.py kabellos, flashed with
# esp32/kabellos_flash.py. docs/kabellos.md.
#
#   pin   Kabellos     C3       port          dir   note
#   ---   ----------   ------   -----------   ---   ----------------------------
#   1     ESP_EN       EN       wifi_en       out   0 = reset; 10k pull-up, 1uF
#   2     ESP_BOOT     GPIO9    wifi_gpio0    out   boot strap, 1 = from flash
#   3     ESP_TX       GPIO21   --            --    the C3's UART0 TX; see below
#   4     ESP_RX       GPIO20   ESP32_U0RX    out   UART0 RX, tied to mark (1)
#   7     ESP_GPIO4    GPIO4    --            --    ESP-AT RTS; unused
#   8     ESP_GPIO5    GPIO5    --            --    ESP-AT CTS; unused (see below)
#   9     ESP_GPIO6    GPIO6    UART1_TX      out   ZNIC, FPGA -> C3, 3 Mbaud
#   10    ESP_GPIO7    GPIO7    UART1_RX      in    ZNIC, C3 -> FPGA; pull-up
#
# (5 and 11 are GND, 6 and 12 are 3V3.) From the pinout table in the
# Kabellos README and checked against kabellos_v2.pdf: the four ESP-AT
# pins are wired straight to the module, and the firmware puts its UART1
# on GPIO6 (RX) and GPIO7 (TX) -- the same pins ESP-AT uses for its AT
# port, so the module's own documentation describes this wiring too.
#
# -- Why 9 and 10, and not the console pair on 3 and 4 --
#
# Pins 3/4 are the C3's UART0: the ROM bootloader's port and, under
# ESP-AT, a log console. zeitlos-nic's console is the C3's USB instead
# (the module's micro-USB socket), which is where a person can read it
# and where flashing happens. Keeping the data plane off UART0 also
# keeps it clear of the ROM's boot messages, which come out on UART0 at
# every reset at 115200 -- straight into the receive FIFO, had ZNIC been
# there.
#
# -- Pin 4 is DRIVEN, high --
#
# An ECP5 pin with no constraint is not left floating: it gets a weak
# pull-down (the same thing rtl/sysctl.v's ULX3S block works around for
# flash HOLD#/WP#). Unconstrained, pin 4 would hold the C3's UART0 RX
# low, which a UART reads as a continuous break. The module has no
# pull-up of its own on that line (schematic: ESP_RX goes straight to
# U0RXD). So the gateware ties it to 1 -- idle -- and the ROM and the
# firmware see a quiet port.
#
# -- Pins 3, 7 and 8 are deliberately absent --
#
#   3  is an OUTPUT from the module (U0TXD). Constraining it as an
#      FPGA output would be two drivers on one net.
#   7  is GPIO4. The firmware leaves it alone; the ECP5's pull-down on
#      an unused pin is harmless on a pin nothing reads.
#   8  is GPIO5, the module's CTS input. Hardware flow control from the
#      receive FIFO's fill level would go here; the link does not need
#      it (RX_POLL carries a credit, docs/esp32link.md) and it is not
#      built yet. When it is, `8=ESP32_CTS` is the whole change here.
#
# -- The pull-up on pin 10 --
#
# UART1_RX is the C3's GPIO7, and the C3 does not drive GPIO7 until the
# firmware has configured its UART -- in reset, and for the first ~100ms
# of every boot, the line floats. rtl/esp32_rxfifo.v samples it
# regardless, and every falling edge on a floating line is a start bit:
# noise bytes in the FIFO and, worse, its "not empty" raising cpu_irq[8]
# and waking `net` over and over. PULLMODE=UP holds it at mark until the
# C3 takes over. boards/ulx3s.lpf makes the same choice for the same
# pin (UART1_RX, PULLMODE=UP) for the ULX3S's ESP32.
#
# -- Power, and the jumper --
#
# The module draws its transmit peaks from the host's 3V3 through pins
# 6/12. J1 on the module must be OPEN in a PMOD socket (Kabellos README:
# shorting it powers the module from its USB socket, into the host's
# rail). The firmware's default TX power for Kabellos is 15 dBm for the
# same reason; docs/kabellos.md, "Power".
#
# -- The receive FIFO's size is NOT here --
#
# `ESP32_RXFIFO_BITS is block RAM, which is a property of the board's
# budget rather than of the PMOD -- the same rule katze.spec states for
# its MAC buffers. It goes in the target spec.

description = Kabellos ESP32-C3 WiFi PMOD

pins =
	1=wifi_en   2=wifi_gpio0   4=ESP32_U0RX
	9=UART1_TX  10=UART1_RX

io_type = LVCMOS33

io_type.10 = LVCMOS33 PULLMODE=UP

defines = +UART1 +ESP32_LINK +ESP32_PMOD

notes = Requires a Kabellos WiFi PMOD running zeitlos-nic (zeitlos-nic-kabellos.bin in this release; flash it with esp32/kabellos_flash.py --image, which needs only esptool). J1 on the module must be open. WiFi credentials go in /data/net/net.cfg on the sdcard.
