/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/socctl.v's colour window registers, COLWIN (word
 * 16) and COLWINEND (word 17); docs/color.md, "The colour window".
 *
 *   - reset: off, signature "ZW", avail as built;
 *   - COLWINEND stages the corner without flipping view_load; COLWIN
 *     sets x0, y0 and EN and flips it once;
 *   - read back: EN, the corner;
 *   - x on lanes 0/1, y and EN on lanes 2/3;
 *   - a GAME write entering game mode clears EN; leaving it does not;
 *   - without WIN_AVAIL: EN forced low, nothing stored, avail clear,
 *     the corner reads 0;
 *   - words 0..15 unchanged: MAGIC still at 1, CTRL still at 0.
 *
 *   iverilog -g2012 -I.. -o /tmp/tb tb_socctl_colwin.v ../socctl.v && vvp /tmp/tb
 */

`timescale 1ns/1ps
module tb;
	reg clk = 0, rst = 1;
	reg [31:0] adr = 0, dat_w = 0;
	reg [3:0] sel = 4'hf;
	reg we = 0, stb = 0, cyc = 0;

	wire [31:0] dat_r, dat_r0;
	wire ack, ack0;
	wire vload, vload0;
	wire wen, wen0;
	wire [9:0] wx0, wy0, wx1, wy1;
	wire [9:0] nx0, ny0, nx1, ny1;

	integer fails = 0, guard;
	reg [31:0] rdata;
	reg vl;
	reg which = 1;     // 1: the build with the window, 0: without

	socctl_wb #(.GAME_AVAIL(1'b1), .COLOR_AVAIL(1'b1), .GAME_PLANES(8),
		.WIN_AVAIL(1'b1)) dut (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
		.wb_we_i(we), .wb_sel_i(sel), .wb_stb_i(stb && which),
		.wb_ack_o(ack), .wb_cyc_i(cyc && which),
		.view_load(vload),
		.win_en(wen), .win_x0(wx0), .win_y0(wy0), .win_x1(wx1), .win_y1(wy1),
		.frame_ctr(16'd0), .in_vblank(1'b0),
		.vram_we(1'b0), .vram_adr(15'd0));

	socctl_wb #(.GAME_AVAIL(1'b1), .COLOR_AVAIL(1'b1), .GAME_PLANES(8),
		.WIN_AVAIL(1'b0)) dut0 (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r0),
		.wb_we_i(we), .wb_sel_i(sel), .wb_stb_i(stb && !which),
		.wb_ack_o(ack0), .wb_cyc_i(cyc && !which),
		.view_load(vload0),
		.win_en(wen0), .win_x0(nx0), .win_y0(ny0), .win_x1(nx1), .win_y1(ny1),
		.frame_ctr(16'd0), .in_vblank(1'b0),
		.vram_we(1'b0), .vram_adr(15'd0));

	wire a = which ? ack : ack0;
	wire [31:0] d = which ? dat_r : dat_r0;
	wire vlx = which ? vload : vload0;

	always #10 clk = ~clk;

	task tick; begin @(posedge clk); #1; end endtask

	task wbw(input [31:0] word, input [31:0] dw, input [3:0] lanes);
		begin
			adr = word; dat_w = dw; sel = lanes; we = 1; stb = 1; cyc = 1;
			guard = 0;
			tick;
			while (!a && guard < 50) begin tick; guard = guard + 1; end
			stb = 0; cyc = 0; we = 0; sel = 4'hf;
			tick;
		end
	endtask

	task wbr(input [31:0] word);
		begin
			adr = word; we = 0; stb = 1; cyc = 1;
			guard = 0;
			tick;
			while (!a && guard < 50) begin tick; guard = guard + 1; end
			rdata = d;
			stb = 0; cyc = 0;
			tick;
		end
	endtask

	task check(input [8*48-1:0] what, input [31:0] got, input [31:0] want);
		begin
			if (got !== want) begin
				$display("FAIL: %0s: %08x, expected %08x", what, got, want);
				fails = fails + 1;
			end
		end
	endtask

	initial begin
		tick; tick; rst = 0; tick;

		// -- reset --
		wbr(16); check("COLWIN after reset", rdata, 32'h5a57_8000);
		check("win_en after reset", wen, 0);
		wbr(1);  check("MAGIC still word 1", rdata, 32'h5a43_5452);

		// -- the corner is staged --
		vl = vlx;
		wbw(17, { 6'd0, 10'd301, 6'd0, 10'd457 }, 4'hf);
		check("COLWINEND does not flip view_load", vlx, vl);
		check("x1", wx1, 457);
		check("y1", wy1, 301);
		wbr(17); check("COLWINEND reads back", rdata, { 6'd0, 10'd301, 6'd0, 10'd457 });

		// -- COLWIN: x0, y0, EN, one flip --
		vl = vlx;
		wbw(16, { 1'b1, 5'd0, 10'd61, 6'd0, 10'd137 }, 4'hf);
		check("COLWIN flips view_load", vlx ^ vl, 1);
		check("x0", wx0, 137);
		check("y0", wy0, 61);
		check("win_en", wen, 1);
		wbr(16); check("COLWIN reads EN", rdata, 32'h5a57_8001);

		// -- lanes --
		wbw(16, { 1'b0, 5'd0, 10'd99, 6'd0, 10'd200 }, 4'b0011);
		check("lanes 0/1: x0", wx0, 200);
		check("lanes 0/1: y0 kept", wy0, 61);
		check("lanes 0/1: EN kept", wen, 1);
		wbw(16, { 1'b0, 5'd0, 10'd99, 6'd0, 10'd300 }, 4'b1100);
		check("lanes 2/3: x0 kept", wx0, 200);
		check("lanes 2/3: y0", wy0, 99);
		check("lanes 2/3: EN cleared", wen, 0);

		// -- game mode --
		wbw(16, { 1'b1, 5'd0, 10'd61, 6'd0, 10'd137 }, 4'hf);
		wbw(3, 32'h0, 4'hf);
		check("leaving game mode keeps EN", wen, 1);
		wbw(3, 32'h1, 4'hf);
		check("entering game mode clears EN", wen, 0);
		wbw(3, 32'h0, 4'hf);
		check("still clear after leaving", wen, 0);
		wbr(0); check("CTRL bit 31 still set (word 16 is not CTRL)", rdata[31], 1);

		// -- without WIN_AVAIL --
		which = 0;
		wbr(16); check("no window: COLWIN reads", rdata, 32'h5a57_0000);
		wbw(17, { 6'd0, 10'd301, 6'd0, 10'd457 }, 4'hf);
		wbw(16, { 1'b1, 5'd0, 10'd61, 6'd0, 10'd137 }, 4'hf);
		check("no window: win_en forced low", wen0, 0);
		wbr(16); check("no window: EN reads 0", rdata, 32'h5a57_0000);
		wbr(17); check("no window: corner reads 0", rdata, 32'h0);

		if (fails == 0) $display("RESULT: PASS");
		else $display("RESULT: FAIL (%0d)", fails);
		$finish;
	end
endmodule
