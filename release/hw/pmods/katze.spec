# Katze -- LAN8720A 10/100 RMII ethernet PHY PMOD.
#
# github.com/machdyne/katze, machdyne.com/product/katze-ethernet-pmod/
#
# The first PMOD here that is RMII rather than SPI. Langkatze carries a
# whole MAC (the ENC28J60) and the FPGA only speaks SPI to it; Katze
# carries only the PHY, so the MAC is rtl/ethmac_rmii.v in the fabric --
# the same MAC mozart_ml1 and sergei_ml1 use for their on-board
# LAN8720As. Adds `ETH_RMII, which makes rtl/sysctl.v instantiate it and
# declare the eight ports below.
#
#   pin   Katze       port          dir   note
#   ---   ---------   -----------   ---   ------------------------------
#   1     E_TXD0      ETH_TXD[0]    out
#   2     E_TXD1      ETH_TXD[1]    out
#   3     E_TXEN      ETH_TX_EN     out
#   4     E_CRS_DV    ETH_CRS_DV    in    PHY strap MODE2 -- pull-up
#   7     E_RXD0      ETH_RXD[0]    in    PHY strap MODE0 -- pull-up
#   8     E_RXD1      ETH_RXD[1]    in    PHY strap MODE1 -- pull-up
#   9     E_RST_N     ETH_RST_N     out   PHY reset, active low
#   10    CLK50       ETH_REFCLK    in    50MHz, from the PMOD's own osc
#
# (5 and 11 are GND, 6 and 12 are 3V3.) Taken from the pinout table in
# the Katze README, and checked against katze_v1's schematic: the
# LAN8720A's XTAL1/CLKIN and the connector's pin 10 share one net
# (CLK50) driven by a 50MHz KC2520Z oscillator on the module.
#
# -- The reference clock is an INPUT --
#
# Every RMII signal is synchronous to one 50MHz clock that both ends
# share. On Katze the module makes it: the oscillator drives the PHY and
# pin 10 together. So `ETH_RMII_DRIVE_REFCLK -- sergei_ml1's option,
# for a board whose PHY has no oscillator and expects the FPGA to make
# the clock -- must NOT be set for Katze. Setting it would turn
# ETH_REFCLK into an output and put the FPGA and the oscillator on one
# net, driving against each other.
#
# frequency.10 below is what makes nextpnr time the ETH_REFCLK domain
# at all. Without it the MAC's 50MHz logic is unconstrained, and
# release/lib/build.py's timing gate could not see it fail.
#
# -- Why three pins have pull-ups and the others do not --
#
# The LAN8720A samples MODE[2:0] off RXD0, RXD1 and CRS_DV at the moment
# nRST is released. 111 is "all capable, auto-negotiation enabled",
# which is what a MAC with no MDIO needs: this one never talks to the
# PHY's registers (Katze does not even bring MDC/MDIO to the connector),
# so whatever the straps say is the configuration, permanently.
#
# The PHY has weak internal pull-ups on those pins, but the FPGA's input
# buffers are on the same nets, and the reference LiteX platform for
# Katze adds PULLMODE=UP on exactly these three. So does
# boards/mozart_ml1.lpf for the on-board LAN8720A. This is the same
# decision, in the same place, for the same chip.
#
# Pull-ups on the OUTPUTS would be meaningless once the FPGA drives
# them, and on ETH_REFCLK would load an oscillator for no reason, so
# the default io_type carries none and the three straps override it.
#
# -- Why the strap timing works --
#
# rtl/ethmac_rmii.v holds ETH_RST_N low for 2^22 system clocks (~87ms at
# 48MHz) after configuration, so the PHY's straps are sampled long after
# the FPGA's pull-ups are live. During configuration itself the ECP5's
# pins are not yet driven and ETH_RST_N floats; the LAN8720A may leave
# reset then with whatever the straps read, but the FPGA's reset pulse
# that follows re-samples them, so the mode the PHY ends up in is
# always the one the pull-ups set.
#
# -- Buffer sizing is NOT here --
#
# `ETH_RX_SLOTS, and whatever else has to shrink to make room for the
# MAC's buffers, are properties of the board's budget, not of the
# PMOD -- a 45F has block RAM to spare and a 25F does not. They go in
# the target spec.
# See targets/lakritz_katze.spec.

description = Katze LAN8720A RMII Ethernet PMOD

pins =
	1=ETH_TXD[0]  2=ETH_TXD[1]  3=ETH_TX_EN  4=ETH_CRS_DV
	7=ETH_RXD[0]  8=ETH_RXD[1]  9=ETH_RST_N  10=ETH_REFCLK

io_type = LVCMOS33

io_type.4 = LVCMOS33 PULLMODE=UP
io_type.7 = LVCMOS33 PULLMODE=UP
io_type.8 = LVCMOS33 PULLMODE=UP

frequency.10 = 50 MHZ

defines = +ETH_RMII

notes = Requires a Katze RMII ethernet PMOD. The `net` app is included in flash.
