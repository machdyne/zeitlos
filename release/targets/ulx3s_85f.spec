# ULX3S ECP5-85F.
#
# 83640 LUTs. This is the variant the ULX3S port was
# developed and timed on, and the one to trust first if a release is
# going out before the others have been built.
#
# One PCB, one .lpf, one define set -- the whole difference between
# the four ULX3S targets is nextpnr's --85k. A bitstream built for one
# variant will not load on another, so shipping a single "ulx3s" image
# would be wrong for three quarters of owners.
#
# Networking is the on-board ESP32 over UART1 (docs/esp32link.md), so
# there is no PMOD to plug in. The ESP32 needs its own firmware
# flashed separately -- the release image covers the FPGA only. See
# docs/ulx3s.md.

description = ULX3S ECP5-85F

base = ulx3s

make_vars = DEVICE=85k
