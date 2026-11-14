`timescale 1ns / 1ps
// Screen modes bench (docs/screen_modes.md): rtl/gpu/gpu_video.v built
// with VMODES, its picture captured off the wire, one line per
// character, for tools/ddmi/test_vmodes.py to check.
//
// The clocks are made as rtl/clk/pll1.v makes them: bclk 378 MHz, cclk
// one in five of it (75.6), pclk one in two of that (37.8), all from one
// edge; the wishbone clock is 48 MHz and unrelated. VRAM answers a word
// made from its address (vram_word() below, which test_vmodes.py
// repeats), a clock after the address, as rtl/mem/vram.v does.
//
// The capture is rebuilt from the four ODDRX2F outputs, as a receiver
// would: the clock lane's rising edge is a character's first bit, and
// each data bit is read in the middle of its repeats (three half-clocks
// at 640x480, two at 800x600, one otherwise). So the serialiser, the
// bit repetition and the encoders are all inside what is checked.
//
//   +mode=M     the mode asked for (0 640x480, 1 800x600, 2 1024x768,
//               3 1280x720); the bench starts in 640x480 and asks for M
//               after the first frame, so the switch is in it too
//   +frames=N   whole frames to capture after the switch (default 1)
//   +en=1       data islands on (with -DGPU_DDMI_AUDIO)
//   +out=FILE   the capture
//   -DFAST      no 378 MHz clock: the characters from the encoders,
//               about ten times faster (not the serialiser)
//   -DNATIVE720=0  with WIDE: 1280x720 doubled from 640x360, as on the
//               25F boards (VIDEO_NO_NATIVE_720, Sergei ML2)
//   -DWIDE=1    the 45F build (native 800x600 .. 1280x720); default 0,
//               the 25F build (1280x720 from a 640x360 desktop)
//
// Plain Verilog-2005. Run by `make test_vmodes`.

`ifndef NATIVE720
`define NATIVE720 1
`endif
`ifndef WIDE
`define WIDE 0
`endif

module tb_vmodes;

	reg bclk = 0;
	reg cclk = 0;
	reg pclk = 0;
	reg clk = 0;
	reg resetn = 0;
	reg [2:0] bdiv = 0;
	reg cdiv = 0;

`ifdef FAST
	// -DFAST: no bit clock. cclk on its own, and the capture taken from
	// the encoders, a character at a time (the serialiser is then not
	// in what is checked: run without FAST for that)
	always #6.6138 cclk = !cclk;
`else
	// bclk: 378 MHz, a half period of 1.3228 ns
	always #1.3228 bclk = !bclk;
	always @(posedge bclk) begin
		bdiv <= (bdiv == 3'd4) ? 3'd0 : bdiv + 3'd1;
		if (bdiv == 3'd0) cclk <= 1'b1;
		if (bdiv == 3'd2) cclk <= 1'b0;
	end
`endif
	// cclk is high for two bclk of five and low for three; pclk changes
	// on cclk's rising edge, every second one
	always @(posedge cclk) begin
		cdiv <= !cdiv;
		pclk <= !cdiv;
	end
	always #10.4167 clk = !clk;

	integer mode_arg = 0, frames = 1, en_arg = 0;
	reg [2:0] vmode = 3'd0;
	reg ddmi_en = 1'b0;
	reg [8*256-1:0] outname;
	integer fd;

	function [31:0] vram_word;
		input [14:0] a;
		reg [31:0] h;
		begin
			h = { 17'd0, a } * 32'd2654435761;
			vram_word = h ^ (h >> 15) ^ { a, 17'd0 };
		end
	endfunction

	wire [14:0] gb_adr;
	// one clock, as rtl/mem/vram.v's registered read into gpu_video
	reg [31:0] gb_dat = 0;
	always @(posedge clk)
		gb_dat <= vram_word(gb_adr);

	wire [3:0] dvi_p;
	wire [15:0] frame_ctr;
	wire in_vblank;

	gpu_video #(
		.CURSOR_AVAIL(1'b0),
		.VMODES(1'b1),
		.VM_MASK(8'h7f),
		.WIDE(`WIDE),
		.NATIVE720(`NATIVE720)
	) dut (
		.pixel(1'b0),
		.curs_x(11'd0), .curs_y(10'd0), .curs_alt(1'b0), .curs_hide(1'b1),
		.curs_scr(1'b0), .curs_slots(4'd0), .curs_we(1'b0), .curs_waddr(6'd0),
		.curs_wdata(32'd0),
		.clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.cclk(cclk), .vmode(vmode),
		.video_mode(2'd0), .ddmi_en(ddmi_en), .ddmi_sound(1'b0), .ddmi_fs(4'd2),
		.ddmi_acr_n(20'd6144), .ddmi_acr_cts(20'd75600), .ddmi_we(1'b0),
		.ddmi_wdata(32'd0), .video_bright(4'd7),
		.view_load(1'b0), .game_en(1'b0), .game_wrap(1'b0),
		.view_x(10'd0), .view_y(10'd0),
		.color_en(1'b0), .color_np(3'd0), .color_off(6'd0), .color_chunky(1'b0),
		.win_en(1'b0), .win_x0(10'd0), .win_y0(10'd0), .win_x1(10'd0), .win_y1(10'd0),
		.win_desk(1'b0),
		.pal_we(1'b0), .pal_idx(8'd0), .pal_rgb(12'd0), .cvbs_mono(1'b0),
		.frame_ctr(frame_ctr), .in_vblank(in_vblank),
		.dvi_p(dvi_p),
		.gb_adr_o(gb_adr), .gb_dat_i(gb_dat)
	);

	// -- rebuilding characters from the wire --
	//
	// Each half of bclk carries one half-bit on each lane; they are taken
	// a quarter period in, into a 64-deep history per lane. A rising edge
	// on the clock lane is the first bit of a character; from then on a
	// character is every 10 * rep half-bits, and bit j of it is half-bit
	// j * rep + rep / 2.
	integer rep_k = 3;
	integer t = 0;           // half-bits since the clock lane's last character start
	integer locked = 0;
	reg [9:0] cw0, cw1, cw2;
	reg prev_ck = 0;
	reg capturing = 0;
	integer nchars = 0;
	integer want_frames = 0;
	integer seen_frames = 0;
	reg [15:0] fstart = 0;

	task half;
		input [3:0] q;
		integer j;
		begin
			if (prev_ck == 1'b0 && q[3] == 1'b1) begin
				// a character starts here: write the one just finished
				if (locked && t == 10 * rep_k && capturing) begin
					$fwrite(fd, "%03x %03x %03x\n", cw0, cw1, cw2);
					nchars = nchars + 1;
				end else if (locked && t != 10 * rep_k && capturing && nchars > 0) begin
					// (before the first: the old mode's last characters)
					$display("tb_vmodes: FAIL character of %0d half-bits, want %0d at %0d",
						t, 10 * rep_k, nchars);
				end
				locked = 1;
				t = 0;
			end
			if ((t % rep_k) == (rep_k / 2) && t < 10 * rep_k) begin
				j = t / rep_k;
				cw0[j] = q[0];
				cw1[j] = q[1];
				cw2[j] = q[2];
			end
			t = t + 1;
			prev_ck = q[3];
		end
	endtask

`ifdef FAST
	// the encoders' character changes on each edge go is high for: the
	// one being replaced is the one just sent
	always @(posedge cclk) begin
		if (dut.gpu_vchar_i.go && capturing) begin
			$fwrite(fd, "%03x %03x %03x\n", dut.gpu_vchar_i.e_b,
				dut.gpu_vchar_i.e_g, dut.gpu_vchar_i.e_r);
			nchars = nchars + 1;
		end
	end
`else
	always @(posedge bclk) begin
		#0.66 half(dvi_p);
	end
	always @(negedge bclk) begin
		#0.66 half(dvi_p);
	end
`endif

	initial begin
		if (!$value$plusargs("mode=%d", mode_arg)) mode_arg = 0;
		if (!$value$plusargs("frames=%d", frames)) frames = 1;
		if (!$value$plusargs("en=%d", en_arg)) en_arg = 0;
		if (!$value$plusargs("out=%s", outname)) outname = "output/vmodes.txt";
		fd = $fopen(outname, "w");
		ddmi_en = (en_arg != 0);
		#200 resetn = 1;
		// one frame of 640x480, then the mode asked for
		@(posedge frame_ctr[0]);
		vmode = mode_arg;
		rep_k = (mode_arg == 0) ? 3 : (mode_arg == 1 || mode_arg == 4) ? 2 : 1;
		// the switch lands at the next boundary; the frame after that is
		// the first whole one in the new mode. Capture from the boundary
		// before it, so the capture starts at a frame's first character.
		fstart = frame_ctr;
		@(frame_ctr == fstart + 16'd1);
		// characters still in the pipeline belong to the old frame's end;
		// starting a little into blanking is fine, the checker finds the
		// first vsync
		capturing = 1;
		@(frame_ctr == fstart + 16'd2 + frames);
		capturing = 0;
		$fclose(fd);
		$display("tb_vmodes: mode %0d, %0d characters captured", mode_arg, nchars);
		$finish;
	end

endmodule
