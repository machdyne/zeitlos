# GPIO -- not a PMOD at all, but the absence of one.
#
# Every other file in this directory describes a module you plug in:
# these are its pins, this is what they do, this is the define that
# makes rtl/sysctl.v drive them. This one says the opposite -- nothing
# in particular is plugged in, and all eight pins of the connector are
# handed to software.
#
# It lives here anyway because the PMOD layer is exactly the right
# mechanism for it. A GPIO port is "what a connector is being used for"
# in the same sense Langkatze is, it has to take over every ball in the
# port the same way (so the base .lpf's idea of what those balls were
# for gets dropped), and it composes with a second PMOD in another port
# through the same target spec. Inventing a parallel mechanism for
# "connector with no module on it" would duplicate all of that.
#
# -- Pin order --
#
#   PMOD pin   1  2  3  4   7  8  9  10
#   GPIO bit   0  1  2  3   4  5  6  7
#
# Bits 0-3 are the top row and bits 4-7 the bottom row, so bit order IS
# pin order as printed on the connector. Not arbitrary: rtl/debug.v's
# DBG[7:0] LED bar already landed on Obst's PMOD B in exactly this
# order, so this is the convention the tree already had, written down.
#
# The consequence worth knowing when wiring something up: bit 4 is the
# pin diagonally below bit 0, not the one next to bit 3.
#
# -- Which port index this is --
#
# GPIO0, the first port. A target that wants GPIO on TWO connectors
# needs a second spec naming GPIO1[0..7] with `+GPIO_PORT1` -- copy
# this file, change the two, and plug it into the other port. The port
# INDEX is a property of the gateware (rtl/gpio.v numbers its ports
# densely from 0 and rtl/sysctl.v declares them in order), not of which
# connector it lands on, so GPIO0 is the first one built whether that
# is PMOD A or PMOD B. See docs/gpio.md.
#
# -- Pull-ups --
#
# PULLMODE=UP, on every pin. It is an IOBUF attribute, fixed at
# place-and-route, and nothing in software can change it afterwards --
# so this is decided once, here, for every board that uses this spec.
#
# NOT the toolchain default. nextpnr writes PULLMODE=NONE for a
# constrained pin that does not say otherwise (ecp5/bitstream.cc:
# str_or_default(ci->attrs, id_PULLMODE, "NONE")), so leaving this line
# off would mean genuinely floating pins. This is a positive choice
# over that, for three reasons:
#
#   1. A floating LVCMOS input is not merely undefined. It sits near
#      the switching threshold, it can oscillate, and the input buffer
#      draws crowbar current while it does. rtl/gpio.v resets every pin
#      to INPUT, so from the moment the bitstream loads until software
#      configures something, all eight pins are in exactly that state.
#
#   2. High is the right idle level for almost everything that gets
#      plugged in here: it is idle for I2C, deasserted for an
#      active-low SPI chip select, and mark for a UART TX. So a module
#      plugged into a machine that has not configured its GPIO yet does
#      not have its bus held down.
#
#   3. It makes an I2C PMOD with no pull-up resistors of its own work
#      when plugged straight in. The pull is spec'd as a current
#      (I_PU in FPGA-DS-02012) rather than a resistance and works out
#      to tens of kOhm at 3V3, which is too weak to meet the 1us rise
#      time 100kHz I2C requires -- but the master here is BIT-BANGED
#      and picks its own clock, and at the 20-50kHz it will realistically
#      reach, a microsecond or two of RC rise is invisible against a
#      20-50us bit period.
#
#      What makes that safe rather than lucky: sw/common/zi2c.c has to
#      poll SCL after releasing it anyway, because that is how clock
#      stretching works, and the same wait absorbs the RC rise. It does
#      the equivalent read-back on SDA before each SCL rising edge. So
#      a weak pull-up costs SPEED, not CORRECTNESS -- the bus gets
#      slower, it does not get wrong.
#
# EXTERNAL RESISTORS ARE STILL THE RIGHT ANSWER for anything past a
# short cable or a couple of devices: 2.2k-4.7k to 3V3. This makes the
# bare case work, not the general case fast.
#
# THE ONE THING TO CHECK when plugging in an unfamiliar module: an
# active-high enable, reset or mode strap now sees a high from the
# moment configuration finishes, and because ECP5 pins are tri-stated
# DURING configuration it sees a low-to-high edge at the end of it,
# which a device that is already powered may sample. A 10k pull-down on
# the module wins comfortably against a pull this weak (~0.5V at the
# pin, inside V_IL), so the fix is easy -- knowing to look is the hard
# part. docs/gpio.md says this too.
#
# DO NOT "SIMPLIFY" THIS TO ECP5's OPENDRAIN IO_TYPE. The sysIO guide
# (FPGA-TN-02032) is explicit that configuring an output as OPENDRAIN
# forces PULLMODE to NONE. Open drain here is done in the fabric
# instead -- park OUT at 0 and move DIR, which is what rtl/gpio.v's
# DIRSET/DIRCLR exist for -- and that keeps the pull-up. Switching to
# the hardware mode would silently turn off the pull-ups for exactly
# the case they were enabled for.
#
# To change this for a board that wants something else, copy this file
# and change the io_type line: it is interpolated straight into each
# IOBUF line, so anything legal in an .lpf IOBUF statement works.

description = GPIO port (8 pins, bidirectional)

pins =
	1=GPIO0[0]  2=GPIO0[1]  3=GPIO0[2]  4=GPIO0[3]
	7=GPIO0[4]  8=GPIO0[5]  9=GPIO0[6]  10=GPIO0[7]

io_type = LVCMOS33 PULLMODE=UP

defines = +GPIO_PORT0

notes = All eight pins of this port are GPIO, with weak internal pull-ups. Fast or loaded I2C still wants external 2.2k-4.7k resistors.
