# Lakritz with a USB-UART PMOD -- the configuration the top-level
# README's Usage section describes, and the one to reach for if you
# just want a desktop on HDMI with a serial console.
#
# Identical gateware to a plain `make BOARD=lakritz flash`. The
# usbuart PMOD adds nothing; see hw/pmods/usbuart.spec.

description = Lakritz + USB-UART PMOD

base  = lakritz
pmods = usbuart
