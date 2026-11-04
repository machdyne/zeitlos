/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * wb_arbiter_gfx -- three masters onto one port of wb_arbiter_main,
 * fixed priority, for `VRAM_SDRAM (docs/profiles.md, "Lakritz").
 *
 * With the chunky picture in main memory two more masters need the
 * main bus: the scanout's line fetch (rtl/gpu/gpu_ckfetch.v) and T2's
 * output (gpu_t2.v, LINEAR). Rather than widen wb_arbiter_main, which
 * every bitstream relies on, they share its third port with the audio
 * mixer, which used to have it alone, behind this:
 *
 *   m0  the scanout's fetch   a line ahead of the beam: first, always
 *   m1  the audio mixer       eight words a sample period
 *   m2  T2's output           what is left
 *
 * Fixed priority is safe here because the two above T2 are bounded:
 * the fetch reads 80 words a game-mode line (about 3,000 cycles) and
 * then waits for the next, the mixer under 1% of the bus. T2 cannot be
 * starved, only paced; the CPU and the blitter are outside, in
 * wb_arbiter_main's rotation, against this port as a whole.
 *
 * The grant is held while the winner's CYC is up, as in
 * wb_arbiter_main: a burst (the fetch's four-word reads, CTI 010) is
 * never torn. cti_o is the winner's.
 */

module wb_arbiter_gfx (
	input  wire        clk,
	input  wire        rst,

	input  wire [31:0] m0_adr_i,
	input  wire [31:0] m0_dat_i,
	output wire [31:0] m0_dat_o,
	input  wire        m0_we_i,
	input  wire [3:0]  m0_sel_i,
	input  wire [2:0]  m0_cti_i,
	input  wire        m0_stb_i,
	input  wire        m0_cyc_i,
	output reg         m0_ack_o,

	input  wire [31:0] m1_adr_i,
	input  wire [31:0] m1_dat_i,
	output wire [31:0] m1_dat_o,
	input  wire        m1_we_i,
	input  wire [3:0]  m1_sel_i,
	input  wire [2:0]  m1_cti_i,
	input  wire        m1_stb_i,
	input  wire        m1_cyc_i,
	output reg         m1_ack_o,

	input  wire [31:0] m2_adr_i,
	input  wire [31:0] m2_dat_i,
	output wire [31:0] m2_dat_o,
	input  wire        m2_we_i,
	input  wire [3:0]  m2_sel_i,
	input  wire [2:0]  m2_cti_i,
	input  wire        m2_stb_i,
	input  wire        m2_cyc_i,
	output reg         m2_ack_o,

	output reg  [31:0] s_adr_o,
	output reg  [31:0] s_dat_o,
	input  wire [31:0] s_dat_i,
	output reg         s_we_o,
	output reg  [3:0]  s_sel_o,
	output reg  [2:0]  s_cti_o,
	output reg         s_stb_o,
	output reg         s_cyc_o,
	input  wire        s_ack_i,

	output reg  [1:0]  master
);

	localparam [1:0] IDLE = 2'd0, A0 = 2'd1, A1 = 2'd2, A2 = 2'd3;
	reg [1:0] state;

	wire r0 = m0_cyc_i && m0_stb_i;
	wire r1 = m1_cyc_i && m1_stb_i;
	wire r2 = m2_cyc_i && m2_stb_i;

	assign m0_dat_o = s_dat_i;
	assign m1_dat_o = s_dat_i;
	assign m2_dat_o = s_dat_i;

	always @(posedge clk) begin
		if (rst) begin
			state <= IDLE;
			master <= 2'd0;
		end else begin
			case (state)
			IDLE: begin
				if (r0) begin state <= A0; master <= 2'd0; end
				else if (r1) begin state <= A1; master <= 2'd1; end
				else if (r2) begin state <= A2; master <= 2'd2; end
			end
			A0: if (!m0_cyc_i) state <= IDLE;
			A1: if (!m1_cyc_i) state <= IDLE;
			A2: if (!m2_cyc_i) state <= IDLE;
			default: state <= IDLE;
			endcase
		end
	end

	always @(*) begin
		s_adr_o = 32'h0;
		s_dat_o = 32'h0;
		s_we_o  = 1'b0;
		s_sel_o = 4'h0;
		s_cti_o = 3'b000;
		s_stb_o = 1'b0;
		s_cyc_o = 1'b0;
		m0_ack_o = 1'b0;
		m1_ack_o = 1'b0;
		m2_ack_o = 1'b0;
		case (state)
		A0: begin
			s_adr_o = m0_adr_i; s_dat_o = m0_dat_i; s_we_o = m0_we_i;
			s_sel_o = m0_sel_i; s_cti_o = m0_cti_i;
			s_stb_o = m0_stb_i; s_cyc_o = m0_cyc_i;
			m0_ack_o = s_ack_i;
		end
		A1: begin
			s_adr_o = m1_adr_i; s_dat_o = m1_dat_i; s_we_o = m1_we_i;
			s_sel_o = m1_sel_i; s_cti_o = m1_cti_i;
			s_stb_o = m1_stb_i; s_cyc_o = m1_cyc_i;
			m1_ack_o = s_ack_i;
		end
		A2: begin
			s_adr_o = m2_adr_i; s_dat_o = m2_dat_i; s_we_o = m2_we_i;
			s_sel_o = m2_sel_i; s_cti_o = m2_cti_i;
			s_stb_o = m2_stb_i; s_cyc_o = m2_cyc_i;
			m2_ack_o = s_ack_i;
		end
		default: ;
		endcase
	end

endmodule
