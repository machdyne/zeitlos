# Langkatze -- ENC28J60 SPI ethernet PMOD.
#
# Adds `SPI_ETH, which makes rtl/sysctl.v instantiate rtl/spim.v -- the
# same hardware SPI master the sdcard uses, parameterised
# DEFAULT_DIV=1 because the ENC28J60 takes full speed from reset and
# needs no slow init phase -- and declare the five ports below.
#
#   pin   Langkatze   port        note
#   ---   ---------   ---------   --------------------------------
#   1     CSN         ETH_SS
#   2     MOSI        ETH_MOSI
#   3     MISO        ETH_MISO
#   4     SCK         ETH_SCLK
#   7     INTN        ETH_INT
#   8     RSTN        --          external pull-up; see below
#   9     NC          --          leave floating
#   10    CLK50       --          output FROM the PMOD
#
# (5 and 11 are GND, 6 and 12 are 3V3.)
#
# PINS 8, 9 AND 10 ARE DELIBERATELY ABSENT from the list, and the
# generator only emits constraints for pins named here -- so leaving
# them out is what guarantees nothing on the FPGA side is ever placed
# on them. Each has its own reason:
#
#   8  RSTN is the ENC28J60's reset, active low, and the PMOD carries
#      its own pull-up. spim_wb (rtl/spim.v) drives only CS, SCK, MOSI
#      and reads MISO and INT -- it has no reset output at all -- so
#      the pull-up is what releases the chip from reset and
#      leaving the ball unconstrained is correct rather than merely
#      harmless. Driving it low would hold the NIC in reset
#      permanently, which presents as a MAC that never answers.
#
#   9  Not connected. Safe either way; unlisted for tidiness.
#
#   10 CLK50 is an OUTPUT from the PMOD. Placing anything here would
#      mean two drivers on one net.
#
# If rtl/spim.v ever gains a reset output, pin 8 is where it goes --
# add `8=ETH_RST_N` here and nothing else in this system changes.
#
# NOTHING HERE MENTIONS NET_PHY. sw/apps/net's driver choice is derived
# from `SPI_ETH by release/lib/spec.py's derive_sw(), because the two
# being settable independently is exactly the failure sw/apps/net's own
# Makefile documents at length.

description = Langkatze ENC28J60 SPI Ethernet PMOD

pins =
	1=ETH_SS
	2=ETH_MOSI
	3=ETH_MISO
	4=ETH_SCLK
	7=ETH_INT

io_type = LVCMOS33

defines = +SPI_ETH

notes = Requires a Langkatze SPI ethernet PMOD. The `net` app is included in flash.
