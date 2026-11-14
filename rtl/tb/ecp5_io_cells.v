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

// ODDRX2F, ECLKSYNCB and CLKDIVF for gpu_video's VMODES serialiser
// (rtl/gpu/gpu_vchar.v). SCLK is ECLK halved, rising with it; D0..D3
// leave in that order over SCLK's two ECLK periods, latency left out.
module ECLKSYNCB(input ECLKI, STOP, output ECLKO);
	assign ECLKO = ECLKI;
endmodule
module CLKDIVF #(parameter DIV = "2.0") (input CLKI, RST, ALIGNWD, output reg CDIVX = 1'b0);
	always @(posedge CLKI) CDIVX <= !CDIVX;
endmodule
module ODDRX2F(input SCLK, ECLK, RST, D0, D1, D2, D3, output Q);
	reg [3:0] h = 4'd0;
	always @(posedge SCLK) h <= { D3, D2, D1, D0 };
	assign Q = SCLK ? (ECLK ? h[0] : h[1]) : (ECLK ? h[2] : h[3]);
endmodule
