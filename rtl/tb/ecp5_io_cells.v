// Behavioural models of the ECP5 I/O cells rtl/ethmac_rmii.v uses at its
// pins, for iverilog only (yosys has them as black boxes). With these,
// the RMII benches can run through the I/O-cell path:
//
//   iverilog -g2005 -DETH_PIN_IO_FF [-DETH_RMII_DRIVE_REFCLK] -I rtl \
//       -o /tmp/tx rtl/tb/tb_ethmac_rmii_tx.v rtl/ethmac_rmii.v \
//       rtl/tb/ecp5_io_cells.v
//
// See docs/networking.md, "Transmit timing when the FPGA drives REF_CLK".

module IFS1P3DX(input SCLK, SP, CD, D, output reg Q);
	initial Q = 0; always @(posedge SCLK) Q <= D;
endmodule
module OFS1P3DX(input SCLK, SP, CD, D, output reg Q);
	initial Q = 0; always @(posedge SCLK) Q <= D;
endmodule
// D0 while SCLK is high, D1 while it is low, both taken at the rising edge.
module ODDRX1F(input SCLK, RST, D0, D1, output Q);
	reg d0 = 0, d1 = 0;
	always @(posedge SCLK) begin d0 <= D0; d1 <= D1; end
	assign Q = SCLK ? d0 : d1;
endmodule
