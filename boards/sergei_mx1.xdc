# sergei_mx1: a Sechzig MX1 module (Artix-7 XC7A35T-FTG256) in a Sergei carrier.
#
# GENERATED, not hand-written, from three sources that agree by
# construction:
#   - the ports of the synthesized netlist (every one constrained, and
#     nothing constrained that is not a port),
#   - boards/sergei_ml1.lpf, which says what each port is on this
#     carrier, mapped through the Sechzig ML1 schematic to the 60-pin
#     edge signal it uses (github.com/machdyne/sechzig, pcb/ml1_v2),
#   - the MX1 schematic, edge signal -> Artix-7 ball (pcb/mx1_v1).
#
# One LOC and one IOSTANDARD line per pin: the form proven with
# nextpnr-xilinx on Kirsch, whose XDC reader is a small Tcl subset.
# The console, the flash pins and the DDMI negative halves come straight
# from the MX1 schematic. The console is UART0 on the module's own
# UART_TX/UART_RX (L2/L3), which the carrier's RP2040 bridges to USB --
# not XC/XD, where the ML1 .lpf files put it: XA-XD reach the RP2040 too,
# but its firmware does not use them. Regenerate rather than edit if the module or carrier
# changes.
#
# Banks 14, 15 and 35 are 3.3 V on the MX1 (solder jumpers JP2/JP3/JP7,
# bridged as shipped), so every single-ended pin is LVCMOS33.
#
# BOTH halves of every TMDS pair are constrained, P on IO_L<n>P and N on
# the matching IO_L<n>N: nextpnr-xilinx places an unconstrained N port
# wherever it likes and then rejects the OBUFDS.
#
# SYS_CLK48 comes from the carrier's RP2040 (the dirtyJtag firmware) on
# F5, a clock-capable MRCC pin. The flash clock (CCLK, E8) has no port:
# rtl/sysctl.v reaches it through STARTUPE2. SYS_RST_N goes to
# PROGRAM_B (L9), which the gateware cannot drive.

create_clock -period 20.833 [get_ports {CLK_48}]


# USBD_P
set_property LOC B2 [get_ports {AUD_OPTICAL}]
set_property IOSTANDARD LVCMOS33 [get_ports {AUD_OPTICAL}]
# SYS_CLK48
set_property LOC F5 [get_ports {CLK_48}]
set_property IOSTANDARD LVCMOS33 [get_ports {CLK_48}]
# CSPI_DQ1
set_property LOC J14 [get_ports {CSPI_MISO}]
set_property IOSTANDARD LVCMOS33 [get_ports {CSPI_MISO}]
# CSPI_DQ0
set_property LOC J13 [get_ports {CSPI_MOSI}]
set_property IOSTANDARD LVCMOS33 [get_ports {CSPI_MOSI}]
# CSPI_SS
set_property LOC L12 [get_ports {CSPI_SS_FLASH}]
set_property IOSTANDARD LVCMOS33 [get_ports {CSPI_SS_FLASH}]
# DDMI_CK_N
set_property LOC B1 [get_ports {DDMI_CK_N}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_CK_N}]
# DDMI_CK_P
set_property LOC C1 [get_ports {DDMI_CK_P}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_CK_P}]
# DDMI_D0_N
set_property LOC D1 [get_ports {DDMI_D0_N}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_D0_N}]
# DDMI_D0_P
set_property LOC E2 [get_ports {DDMI_D0_P}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_D0_P}]
# DDMI_D1_N
set_property LOC E1 [get_ports {DDMI_D1_N}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_D1_N}]
# DDMI_D1_P
set_property LOC F2 [get_ports {DDMI_D1_P}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_D1_P}]
# DDMI_D2_N
set_property LOC G1 [get_ports {DDMI_D2_N}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_D2_N}]
# DDMI_D2_P
set_property LOC G2 [get_ports {DDMI_D2_P}]
set_property IOSTANDARD TMDS_33 [get_ports {DDMI_D2_P}]
# ETH_CRS_DV
set_property LOC H3 [get_ports {ETH_CRS_DV}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_CRS_DV}]
# ETH_CLK50
set_property LOC D4 [get_ports {ETH_REFCLK}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_REFCLK}]
# ETH_RST_N
set_property LOC H4 [get_ports {ETH_RST_N}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_RST_N}]
# ETH_RX0
set_property LOC F3 [get_ports {ETH_RXD[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_RXD[0]}]
# ETH_RX1
set_property LOC F4 [get_ports {ETH_RXD[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_RXD[1]}]
# ETH_TX0
set_property LOC D3 [get_ports {ETH_TXD[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_TXD[0]}]
# ETH_TX1
set_property LOC E3 [get_ports {ETH_TXD[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_TXD[1]}]
# ETH_TXEN
set_property LOC G4 [get_ports {ETH_TX_EN}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_TX_EN}]
# LED
set_property LOC R16 [get_ports {LED_B}]
set_property IOSTANDARD LVCMOS33 [get_ports {LED_B}]
# SD_MISO
set_property LOC T7 [get_ports {SD_MISO}]
set_property IOSTANDARD LVCMOS33 [get_ports {SD_MISO}]
# SD_MOSI
set_property LOC T8 [get_ports {SD_MOSI}]
set_property IOSTANDARD LVCMOS33 [get_ports {SD_MOSI}]
# SD_SCK
set_property LOC R6 [get_ports {SD_SCK}]
set_property IOSTANDARD LVCMOS33 [get_ports {SD_SCK}]
# SD_SS
set_property LOC T5 [get_ports {SD_SS}]
set_property IOSTANDARD LVCMOS33 [get_ports {SD_SS}]
# UART_RX
set_property LOC L3 [get_ports {UART0_RX}]
set_property IOSTANDARD LVCMOS33 [get_ports {UART0_RX}]
# UART_TX
set_property LOC L2 [get_ports {UART0_TX}]
set_property IOSTANDARD LVCMOS33 [get_ports {UART0_TX}]
# DRAM_A0
set_property LOC D15 [get_ports {sdram_a[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[0]}]
# DRAM_A10
set_property LOC C16 [get_ports {sdram_a[10]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[10]}]
# DRAM_A11
set_property LOC D5 [get_ports {sdram_a[11]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[11]}]
# DRAM_A12
set_property LOC E5 [get_ports {sdram_a[12]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[12]}]
# DRAM_A1
set_property LOC D16 [get_ports {sdram_a[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[1]}]
# DRAM_A2
set_property LOC E15 [get_ports {sdram_a[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[2]}]
# DRAM_A3
set_property LOC E16 [get_ports {sdram_a[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[3]}]
# DRAM_A4
set_property LOC C9 [get_ports {sdram_a[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[4]}]
# DRAM_A5
set_property LOC D9 [get_ports {sdram_a[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[5]}]
# DRAM_A6
set_property LOC D8 [get_ports {sdram_a[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[6]}]
# DRAM_A7
set_property LOC C7 [get_ports {sdram_a[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[7]}]
# DRAM_A8
set_property LOC E6 [get_ports {sdram_a[8]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[8]}]
# DRAM_A9
set_property LOC D6 [get_ports {sdram_a[9]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_a[9]}]
# DRAM_BS0
set_property LOC B9 [get_ports {sdram_ba[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_ba[0]}]
# DRAM_BS1
set_property LOC A8 [get_ports {sdram_ba[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_ba[1]}]
# DRAM_CAS
set_property LOC A10 [get_ports {sdram_cas_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_cas_n}]
# DRAM_CKE
set_property LOC C4 [get_ports {sdram_cke}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_cke}]
# DRAM_CLK
set_property LOC C8 [get_ports {sdram_clock}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_clock}]
# DRAM_CS
set_property LOC A9 [get_ports {sdram_cs_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_cs_n}]
# DRAM_LDQM
set_property LOC B12 [get_ports {sdram_dm[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dm[0]}]
# DRAM_UDQM
set_property LOC A7 [get_ports {sdram_dm[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dm[1]}]
# DRAM_DQ0
set_property LOC B16 [get_ports {sdram_dq[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[0]}]
# DRAM_DQ10
set_property LOC A5 [get_ports {sdram_dq[10]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[10]}]
# DRAM_DQ11
set_property LOC B5 [get_ports {sdram_dq[11]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[11]}]
# DRAM_DQ12
set_property LOC A4 [get_ports {sdram_dq[12]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[12]}]
# DRAM_DQ13
set_property LOC B4 [get_ports {sdram_dq[13]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[13]}]
# DRAM_DQ14
set_property LOC C3 [get_ports {sdram_dq[14]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[14]}]
# DRAM_DQ15
set_property LOC A3 [get_ports {sdram_dq[15]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[15]}]
# DRAM_DQ1
set_property LOC A15 [get_ports {sdram_dq[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[1]}]
# DRAM_DQ2
set_property LOC B15 [get_ports {sdram_dq[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[2]}]
# DRAM_DQ3
set_property LOC A14 [get_ports {sdram_dq[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[3]}]
# DRAM_DQ4
set_property LOC B14 [get_ports {sdram_dq[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[4]}]
# DRAM_DQ5
set_property LOC A13 [get_ports {sdram_dq[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[5]}]
# DRAM_DQ6
set_property LOC C13 [get_ports {sdram_dq[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[6]}]
# DRAM_DQ7
set_property LOC A12 [get_ports {sdram_dq[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[7]}]
# DRAM_DQ8
set_property LOC B6 [get_ports {sdram_dq[8]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[8]}]
# DRAM_DQ9
set_property LOC C6 [get_ports {sdram_dq[9]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_dq[9]}]
# DRAM_RAS
set_property LOC B10 [get_ports {sdram_ras_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_ras_n}]
# DRAM_WE
set_property LOC B11 [get_ports {sdram_we_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {sdram_we_n}]
# USBH0_N
set_property LOC H1 [get_ports {usb_host_dm[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usb_host_dm[0]}]
# USBH1_N
set_property LOC J1 [get_ports {usb_host_dm[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usb_host_dm[1]}]
# USBH0_P
set_property LOC H2 [get_ports {usb_host_dp[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usb_host_dp[0]}]
# USBH1_P
set_property LOC K1 [get_ports {usb_host_dp[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {usb_host_dp[1]}]
