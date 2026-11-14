/*
 * Zeitlos SOC GPU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * TMDS encoder, pipelined, with a character enable -- for the screen
 * modes' character clock (rtl/gpu/gpu_vchar.v, docs/screen_modes.md).
 *
 * The same characters as rtl/gpu/tmds_encoder.v, which the 640x480-only
 * builds keep: video by DVI 1.0's transition minimising and DC
 * balancing, control by the HDMI control table, TERC4 and the two guard
 * bands for data islands. What differs is when the work is done. That
 * encoder does it in one clock at 25.2 MHz; this one runs at 75.6 MHz
 * (720p's characters), so the work is split over three stages, each
 * advancing only on `ce`, once a character:
 *
 *   1  the transition-minimised word q_m from the pixel (the popcount
 *      of D and the XOR chain), and the character for a control,
 *      TERC4 or guard-band period
 *   2  q_m's disparity, N1 - N0 of its eight data bits
 *   3  the DC balance: the running count, and the final character
 *
 * Only stage 3 has a loop (the running count), and it is an add and a
 * compare against values stage 2 has ready. Every input goes through
 * all three, so the output is three characters late and everything
 * stays in step with everything else.
 *
 * Plain Verilog, no local declarations.
 */

module tmds_enc_p #(
	parameter CH = 0
) (
	input clk,
	input ce,
	input de,
	input [7:0] d,
	input c1,
	input c0,
	input [1:0] mode,
	input [3:0] t,
	output reg [9:0] q = 10'd0
);

	function [9:0] terc4;
		input [3:0] v;
		begin
			case (v)
				4'h0: terc4 = 10'b1010011100;
				4'h1: terc4 = 10'b1001100011;
				4'h2: terc4 = 10'b1011100100;
				4'h3: terc4 = 10'b1011100010;
				4'h4: terc4 = 10'b0101110001;
				4'h5: terc4 = 10'b0100011110;
				4'h6: terc4 = 10'b0110001110;
				4'h7: terc4 = 10'b0100111100;
				4'h8: terc4 = 10'b1011001100;
				4'h9: terc4 = 10'b0100111001;
				4'hA: terc4 = 10'b0110011100;
				4'hB: terc4 = 10'b1011000110;
				4'hC: terc4 = 10'b1010001110;
				4'hD: terc4 = 10'b1001110001;
				4'hE: terc4 = 10'b0101100011;
				default: terc4 = 10'b1011000011;
			endcase
		end
	endfunction

	localparam [9:0] VIDEO_GB = (CH == 1) ? 10'b0100110011 : 10'b1011001100;
	localparam [9:0] DATA_GB  = 10'b0100110011;

	function [3:0] ones;
		input [7:0] v;
		begin
			ones = { 3'd0, v[0] } + { 3'd0, v[1] } + { 3'd0, v[2] } + { 3'd0, v[3] } +
				{ 3'd0, v[4] } + { 3'd0, v[5] } + { 3'd0, v[6] } + { 3'd0, v[7] };
		end
	endfunction

	// -- stage 1 --
	wire [3:0] n1d = ones(d);
	wire use_xnor = (n1d > 4'd4) || (n1d == 4'd4 && !d[0]);
	wire [7:0] x_xor = { ^d[7:0], ^d[6:0], ^d[5:0], ^d[4:0], ^d[3:0], ^d[2:0], ^d[1:0], d[0] };
	// XNOR chain: q_m[i] = q_m[i-1] ~^ d[i], so bit i is the XOR of
	// d[0..i] inverted once for every odd i
	wire [7:0] x_xnor = x_xor ^ 8'b10101010;
	wire [8:0] qm_n = use_xnor ? { 1'b0, x_xnor } : { 1'b1, x_xor };

	reg [9:0] ctl_n;
	always @(*) begin
		if (mode == 2'd1)
			ctl_n = terc4(t);
		else if (mode == 2'd2)
			ctl_n = VIDEO_GB;
		else if (mode == 2'd3)
			ctl_n = DATA_GB;
		else case ({ c1, c0 })
			2'b00: ctl_n = 10'b1101010100;
			2'b01: ctl_n = 10'b0010101011;
			2'b10: ctl_n = 10'b0101010100;
			default: ctl_n = 10'b1010101011;
		endcase
	end

	reg de1 = 1'b0;
	reg [8:0] qm1 = 9'd0;
	reg [9:0] ctl1 = 10'd0;

	// -- stage 2 --
	reg de2 = 1'b0;
	reg [8:0] qm2 = 9'd0;
	reg [9:0] ctl2 = 10'd0;
	reg signed [5:0] disp2 = 6'sd0;      // N1 - N0 of qm1[7:0]: -8..8

	// -- stage 3 --
	reg signed [5:0] cnt = 6'sd0;        // the running disparity: -16..16

	wire signed [5:0] disp_n = $signed({ 1'b0, ones(qm1[7:0]), 1'b0 }) - 6'sd8;

	wire bal = (cnt == 6'sd0) || (disp2 == 6'sd0);
	wire inv = (cnt > 6'sd0 && disp2 > 6'sd0) || (cnt < 6'sd0 && disp2 < 6'sd0);
	wire signed [5:0] d6 = disp2;
	wire signed [5:0] cnt_bal = qm2[8] ? (cnt + d6) : (cnt - d6);
	wire signed [5:0] cnt_inv = cnt + (qm2[8] ? 6'sd2 : 6'sd0) - d6;
	wire signed [5:0] cnt_pass = cnt - (qm2[8] ? 6'sd0 : 6'sd2) + d6;

	always @(posedge clk) begin
		if (ce) begin
			de1 <= de;
			qm1 <= qm_n;
			ctl1 <= ctl_n;

			de2 <= de1;
			qm2 <= qm1;
			ctl2 <= ctl1;
			disp2 <= disp_n;

			if (!de2) begin
				cnt <= 6'sd0;
				q <= ctl2;
			end else if (bal) begin
				q <= { ~qm2[8], qm2[8], qm2[8] ? qm2[7:0] : ~qm2[7:0] };
				cnt <= cnt_bal;
			end else if (inv) begin
				q <= { 1'b1, qm2[8], ~qm2[7:0] };
				cnt <= cnt_inv;
			end else begin
				q <= { 1'b0, qm2[8], qm2[7:0] };
				cnt <= cnt_pass;
			end
		end
	end

endmodule
