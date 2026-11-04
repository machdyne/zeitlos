/*
 * Zeitlos SOC GPU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * gpu_vmux.v -- two GPU masters on one port of the VRAM arbiter: the
 * line rasterizer (a) and the triangle engine (b), on
 * rtl/arbiter_vram.v's master 1 (docs/gpu_tri.md, "In the SOC").
 *
 * A master keeps the port from its cyc to the cycle it drops it. Then
 * the port stays idle for GAP cycles before either master may have it
 * again: rtl/mem/vram.v acks every cycle stb is held, and the arbiter
 * registers that ack, so an ack can still be on its way for a cycle or
 * two after the master that asked has let go. The gap lets it land on
 * nobody. The rasterizer goes first when both ask (it serves the
 * desktop; the engine runs in game mode, where the rasterizer is
 * quiet).
 */

module gpu_vmux #(
	parameter GAP = 3
) (
	input             clk,
	input             rst,

	input             a_cyc, a_stb, a_we,
	input      [3:0]  a_sel,
	input      [31:0] a_adr, a_dat_o,
	output     [31:0] a_dat_i,
	output            a_ack,

	input             b_cyc, b_stb, b_we,
	input      [3:0]  b_sel,
	input      [31:0] b_adr, b_dat_o,
	output     [31:0] b_dat_i,
	output            b_ack,

	output            m_cyc, m_stb, m_we,
	output     [3:0]  m_sel,
	output     [31:0] m_adr, m_dat_o,
	input      [31:0] m_dat_i,
	input             m_ack
);
	reg [1:0] own;            // 0 none, 1 a, 2 b
	reg [2:0] gap;

	always @(posedge clk) begin
		if (rst) begin
			own <= 2'd0;
			gap <= 3'd0;
		end else begin
			case (own)
			2'd1: if (!a_cyc) begin own <= 2'd0; gap <= GAP; end
			2'd2: if (!b_cyc) begin own <= 2'd0; gap <= GAP; end
			default: begin
				if (gap != 3'd0) gap <= gap - 3'd1;
				else if (a_cyc) own <= 2'd1;
				else if (b_cyc) own <= 2'd2;
			end
			endcase
		end
	end

	wire sa = (own == 2'd1);
	wire sb = (own == 2'd2);

	assign m_cyc   = (sa && a_cyc) || (sb && b_cyc);
	assign m_stb   = (sa && a_stb) || (sb && b_stb);
	assign m_we    = sa ? a_we : b_we;
	assign m_sel   = sa ? a_sel : b_sel;
	assign m_adr   = sa ? a_adr : b_adr;
	assign m_dat_o = sa ? a_dat_o : b_dat_o;
	assign a_dat_i = m_dat_i;
	assign b_dat_i = m_dat_i;
	assign a_ack   = sa && m_ack;
	assign b_ack   = sb && m_ack;
endmodule
