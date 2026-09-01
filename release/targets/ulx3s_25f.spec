# ULX3S ECP5-25F.
#
# Same die as the 12F above, same 24288 LUTs, different
# IDCODE.
#
# One PCB, one .lpf, one define set -- the whole difference between
# the four ULX3S targets is nextpnr's --25k. A bitstream built for one
# variant will not load on another, so shipping a single "ulx3s" image
# would be wrong for three quarters of owners.
#
# Networking is the on-board ESP32 over UART1 (docs/esp32link.md), so
# there is no PMOD to plug in. The ESP32 needs its own firmware
# flashed separately -- the release image covers the FPGA only. See
# docs/ulx3s.md.

description = ULX3S ECP5-25F

base = ulx3s

make_vars = DEVICE=25k
