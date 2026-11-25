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

# Stays on picorv32 for now: with +CPU_ZEITLOS32PS only 1 of 4 tested
# seeds closed 48 MHz on the 85F die (seed 1: 48.64 MHz PASS; seeds
# 10/2/3: 34.84 / 32.57 / 36.68 MHz FAIL, critical path from
# resetn_counter routing 8.87 ns across the chip). +CPU_ZEITLOS32P
# closes 4/4 seeds on 85F (48.50-49.28 MHz) as a future option.
