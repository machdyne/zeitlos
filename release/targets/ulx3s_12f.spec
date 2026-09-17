# ULX3S ECP5-12F.
#
# 12F and 25F are the SAME DIE. prjtrellis exposes the full 24288 LUTs
# on both, so a design that fits one fits the other -- but the IDCODE
# differs, so the bitstreams are not interchangeable and each variant
# still needs its own build.
#
# One PCB, one .lpf, one define set -- the whole difference between
# the four ULX3S targets is nextpnr's --12k. A bitstream built for one
# variant will not load on another, so shipping a single "ulx3s" image
# would be wrong for three quarters of owners.
#
# Networking is the on-board ESP32 over UART1 (docs/esp32link.md), so
# there is no PMOD to plug in. The ESP32 needs its own firmware
# flashed separately -- the release image covers the FPGA only. See
# docs/ulx3s.md.

description = ULX3S ECP5-12F

base = ulx3s

make_vars = DEVICE=12k

# -- block RAM --
#
# THE ONLY THING THAT DIFFERS FROM THE OTHER THREE ULX3S TARGETS.
#
# 56 DP16KD on this die, and the design wants 59 with the defaults.
# The ESP32 receive FIFO is the cheapest three to find: at
# ESP32_RXFIFO_BITS=13 it is 8192 bytes and four blocks, and a 32-bit
# FIFO takes two blocks at any depth down to 1024 entries, so 11
# (2048 bytes) is one block and frees three.
#
# WHAT THIS GIVES UP. rtl/esp32_rxfifo.v sizes 8K for two different
# reasons: holding a frame while a time-sliced process is away from
# the CPU (~4ms, about 1200 bytes at 3 Mbaud), and absorbing a credit
# burst of roughly five MTU frames. 2048 bytes still covers the first
# with about 70% margin -- that is the one whose failure drops bytes.
# It gives up the second, so sustained transfers may dip in throughput
# where the 45F and 85F would not.
#
# Preferred over disabling `ICACHE, which costs the CPU on every
# instruction fetch rather than the network under load only. If three
# blocks turns out not to be enough, `ICACHE_KB=2` is the next lever
# and keeps the cache -- see the cost table in rtl/audio.v's header,
# which measured exactly this trade on Lakritz.
defines = ESP32_RXFIFO_BITS=11
