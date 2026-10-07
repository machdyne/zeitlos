`timescale 1ns / 1ps

// Self-checking testbench for monochrome brightness on DDMI
// (rtl/gpu/gpu_video.v ddmi_ink_*, socctl VIDEO bits 11:8).
//
// Checks the 8-bit channel values going into the TMDS encoders for a
// lit pixel, for every phosphor mode at levels 0, 7 and 15:
//
//   - level 7 is EXACTLY the inks DDMI has always had (white 0x80,
//     amber 0xD2/0x9D, green 0x80), so the reset value changes nothing
//   - white is 16 (level + 1), clamped to 255; amber and green scale
//   - paper's white background follows the level too
//   - colour mode ignores the level (its palette is full range)
//   - a change is adopted at the frame boundary, not mid-frame
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -DGPU_DDMI -DTB_STUBS -o /tmp/tb_bright.out \
//       rtl/gpu/bench/tb_brightness.v /tmp/gpu_video_fix.v \
//       rtl/gpu/gpu_ddmi.v rtl/gpu/tmds_encoder.v
//   vvp /tmp/tb_bright.out

module tb_brightness;

	localparam H_START = 160, H_STOP = 800, V_START = 45, V_STOP = 525;

	reg clk = 0, pclk = 0, bclk = 0, resetn = 0;
	reg [1:0] video_mode = 0;
	reg [3:0] video_bright = 4'd7;
	reg [31:0] gb_dat_i = 0;
	reg view_load = 0, color_en = 0;

	wire red, green, blue, hsync, vsync, is_visible, in_vblank;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p, dac;
	wire [15:0] frame_ctr;

	always #10.417 clk  = ~clk;
	always #19.841 pclk = ~pclk;
	always #3.968  bclk = ~bclk;

	gpu_video #(.COLOR_AVAIL(1'b1)) dut (
		.pixel(1'b0), .clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.video_mode(video_mode), .video_bright(video_bright),
		.view_load(view_load), .game_en(1'b1), .game_wrap(1'b0),
		.view_x(10'd0), .view_y(10'd0),
		.color_en(color_en), .color_np(2'd0), .color_off(6'd0),
		.pal_we(1'b0), .pal_idx(4'd0), .pal_rgb(12'd0), .cvbs_mono(1'b0),
		.frame_ctr(frame_ctr), .in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue), .hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p), .dac(dac), .x(x), .y(y), .is_visible(is_visible),
		.gb_adr_o(gb_adr_o), .gb_dat_i(gb_dat_i)
	);

	// every pixel lit, or none, by switching what VRAM returns
	reg lit = 1;
	always @(posedge clk) gb_dat_i <= lit ? 32'hFFFFFFFF : 32'h0;

	integer errors = 0;

	task next_frame;
		begin
			repeat (8) @(posedge pclk);
			while (!(dut.vc == V_STOP - 1 && dut.hc == H_STOP - 1)) @(posedge pclk);
			repeat (4) @(posedge pclk);
		end
	endtask

	task expect_ink;
		input [1:0] mode;
		input [3:0] level;
		input [7:0] er, eg, eb;
		begin
			video_mode = mode;
			video_bright = level;
			lit = (mode != 2'd3);          // paper: unlit pixels are white
			next_frame();
			while (!(dut.vc == V_START + 100 && dut.hc == H_START + 100)) @(posedge pclk);
			@(negedge pclk);
			if (dut.ddmi_red !== er || dut.ddmi_green !== eg || dut.ddmi_blue !== eb) begin
				$display("FAIL: mode %0d level %0d: %h %h %h, want %h %h %h",
					mode, level, dut.ddmi_red, dut.ddmi_green, dut.ddmi_blue,
					er, eg, eb);
				errors = errors + 1;
			end else
				$display("ok:   mode %0d level %2d: %h %h %h", mode, level,
					er, eg, eb);
		end
	endtask

	initial begin
		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;
		@(posedge clk); view_load <= ~view_load;   // game mode on, mono

		// level 7: the inks DDMI has always had
		expect_ink(2'd0, 4'd7,  8'h80, 8'h80, 8'h80);
		expect_ink(2'd1, 4'd7,  8'hD2, 8'h9D, 8'h00);
		expect_ink(2'd2, 4'd7,  8'h00, 8'h80, 8'h00);
		expect_ink(2'd3, 4'd7,  8'h80, 8'h80, 8'h80);
		// the ends
		expect_ink(2'd0, 4'd0,  8'h10, 8'h10, 8'h10);
		expect_ink(2'd0, 4'd15, 8'hFF, 8'hFF, 8'hFF);
		expect_ink(2'd1, 4'd0,  8'h1A, 8'h13, 8'h00);
		expect_ink(2'd1, 4'd15, 8'hFF, 8'hFF, 8'h00);
		expect_ink(2'd2, 4'd15, 8'h00, 8'hFF, 8'h00);
		expect_ink(2'd3, 4'd11, 8'hC0, 8'hC0, 8'hC0);
		expect_ink(2'd0, 4'd11, 8'hC0, 8'hC0, 8'hC0);

		// adopted at the frame boundary: change mid-frame, look later
		// in the same frame
		video_mode = 0; video_bright = 4'd7; lit = 1;
		next_frame();
		while (!(dut.vc == V_START + 50)) @(posedge pclk);
		video_bright = 4'd15;
		while (!(dut.vc == V_START + 300 && dut.hc == H_START + 10)) @(posedge pclk);
		@(negedge pclk);
		if (dut.ddmi_red !== 8'h80) begin
			$display("FAIL: brightness changed mid-frame (%h)", dut.ddmi_red);
			errors = errors + 1;
		end else $display("ok:   a change waits for the frame boundary");

		// colour mode ignores it: entry 1 of the power-on palette is
		// 00a -> 00 00 aa, at any level
		@(posedge clk); color_en <= 1; view_load <= ~view_load;
		expect_ink(2'd0, 4'd0, 8'h00, 8'h00, 8'hAA);

		if (errors == 0) $display("RESULT: PASS");
		else $display("RESULT: FAIL (%0d errors)", errors);
		$finish;
	end

endmodule

module ODDRX1F (input D0, input D1, input SCLK, input RST, output Q);
	assign Q = D0;
endmodule

module SB_IO (
	inout PACKAGE_PIN, input CLOCK_ENABLE, input OUTPUT_CLK,
	input OUTPUT_ENABLE, input D_OUT_0, input D_OUT_1
);
	parameter [5:0] PIN_TYPE = 6'b0;
	parameter IO_STANDARD = "";
endmodule
