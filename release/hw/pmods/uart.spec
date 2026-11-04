# The serial console on a PMOD: UART0 on pins 2 (TX) and 3 (RX), the
# balls the board build's console used before it moved to the USB-C
# socket (USB_CDC). For a target without USB_CDC that still wants a
# console: lakritz_console, where USB_CDC's logic went to T2. Any
# 3.3 V USB-serial adapter on those pins, 1 Mbaud as UART0 always is.

description = Serial console (UART0) on a PMOD

pins =
	2=UART0_TX  3=UART0_RX

io_type = LVCMOS33

io_type.3 = LVCMOS33 PULLMODE=UP

defines = +UART0

notes = The serial console is UART0 on PMOD pins 2 (TX) and 3 (RX), 3.3 V: plug in a USB-serial adapter.
