/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * One pad of the fast zlink block (rtl/zfast.v): a bidirectional LVDS
 * pair -- docs/zlink.md, "The DS port". Its IO_TYPE (LVDS) and on-chip
 * termination come from the board's LPF; this is the logic at the pad.
 *
 * Both registers sit in the pad's own IOLOGIC, on the 200 MHz clock:
 *
 *   out  ODDRX1F, both halves the same bit: a 200 MHz output register.
 *        rtl/zfast.v changes the bit every other clock (100 Mbit/s).
 *   in   IDDRX1F: two samples a clock, on the rising and the falling
 *        edge -- 400 MS/s, four samples to a 100 Mbit/s bit.
 *
 * Both on ONE clock because they are one IOLOGIC: nextpnr refuses an
 * input and an output register with different clocks on one pad. And
 * an IDDRX1F's input must come straight from the pin (nextpnr enforces
 * that too; rtl/usb/usb_host.v says the same).
 *
 * q[0] is the older sample (the rising edge), q[1] the newer (the
 * falling edge half a clock later), both presented on the rising edge.
 *
 * Without ECP5 (simulation) the same behaviour from plain registers.
 */

module zfast_pad (
	inout pad,
	input clk200,
	input oe,			// drive the pair (else it is an input)
	input d,			// the bit to drive
	output [1:0] q		// samples: q[0] older, q[1] newer
);

`ifdef ECP5
	wire pin_i, od;
	reg t_n = 1'b1;
	always @(posedge clk200) t_n <= !oe;

	TRELLIS_IO #(.DIR("BIDIR")) io (.B(pad), .I(od), .T(t_n), .O(pin_i));
	ODDRX1F oddr (.SCLK(clk200), .RST(1'b0), .D0(d), .D1(d), .Q(od));
	IDDRX1F iddr (.SCLK(clk200), .RST(1'b0), .D(pin_i), .Q0(q[0]), .Q1(q[1]));
`else
	reg od = 1'b0, oe_r = 1'b0;
	always @(posedge clk200) begin
		od <= d;
		oe_r <= oe;
	end
	assign pad = oe_r ? od : 1'bz;

	reg r_pos = 1'b0, r_neg = 1'b0;
	reg [1:0] qq = 2'b00;
	always @(negedge clk200) r_neg <= pad;
	always @(posedge clk200) begin
		r_pos <= pad;
		qq <= { r_neg, r_pos };		// r_pos is the sample a whole clock ago
	end
	assign q = qq;
`endif

endmodule
