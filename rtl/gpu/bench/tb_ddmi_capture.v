`timescale 1ns / 1ps
// DDMI capture bench -- the character stream on the three TMDS data
// channels, one line per pixel clock, for tools/ddmi/ddmi.py to check.
// See docs/ddmi_audio.md, "Bench".
//
// Two ways to drive it:
//
//   default    the whole of rtl/gpu/gpu_video.v (+ gpu_ddmi.v,
//              tmds_encoder.v), with VRAM returning a pattern made from
//              the read address, so the stream is exactly what a board
//              sends for that framebuffer. This is the capture whose
//              hash is the "DDMI audio off is bit-identical DVI" golden
//              (rtl/gpu/bench/ddmi_dvi_golden.sha256).
//   -DDIRECT   gpu_ddmi.v alone, fed the 640x480 timing and
//              ddmi.py's "xor" pattern -- every 8-bit value on every
//              channel, so the TMDS encoder's DC balance is exercised
//              far beyond the two inks the monochrome desktop uses.
//              Checked character for character against ddmi.py's own
//              encoder.
//
// Either way the bench also rebuilds each channel's 10-bit characters
// from the serialiser's 2-bit-per-bclk output (through
// rtl/tb/ecp5_io_cells.v's ODDRX1F) and checks they match the
// parallel words: the 10:1 shift is checked too, not just the coding.
//
// Each of them also builds with -DGPU_DDMI_AUDIO (the data-island
// scheduler, rtl/gpu/ddmi_island.v) and then takes +en=N:
//
//   +en=0     islands built but off: must still be the DVI golden
//   +en=1     on from reset: every frame carries islands
//   +en=2     switched on in the middle of the first captured frame:
//             that frame must stay DVI and the next carry islands --
//             the frame-boundary adoption
//
// and with islands on, sound (round 3):
//
//   +snd=1    frames written into the island FIFO on the 48 MHz clock,
//             as rtl/audio.v writes them: frame n is { L(n), R(n) },
//             L(n) = n * 1237, R(n) = n * 4813 + 77 (16 bits; ddmi.py's
//             --audio-seq checks the same sequence comes out), one every
//             +period=N cycles (default 1000: 48 kHz), each written
//             +rep=K times (default 1); +n= / +cts= / +fs= the clock
//             regeneration pair and channel-status code (default
//             6144 / 25200 / 2: 48 kHz)
//
//   make test_ddmi       (top-level Makefile) runs all of these and
//                        the checker
//
// pclk is made from bclk (one in five), so the two are exactly 5:1 and
// phase-locked as pll1.v makes them.

module tb_ddmi_capture;

	reg bclk = 0;
	reg pclk = 0;
	reg clk = 0;
	reg resetn = 0;
	reg [2:0] bdiv = 0;
	integer frames = 2;
	integer en_arg = 0;
	reg ddmi_en = 0;

	// -- the sound half --
	integer snd_arg = 0, period = 1000, rep_k = 1, acr_n_arg = 6144,
		acr_cts_arg = 25200, fs_arg = 2;
	reg ddmi_sound = 0;
	reg [19:0] acr_n = 20'd6144, acr_cts = 20'd25200;
	reg [3:0] fs_code = 4'd2;
	reg aud_we = 0;
	reg [31:0] aud_wdata = 0;
	integer acnt = 0, aseq = 0, aleft = 0;
	always @(posedge clk) begin
		aud_we <= 1'b0;
		if (snd_arg) begin
			if (aleft > 0) begin
				aud_we <= 1'b1;
				aleft = aleft - 1;
			end
			acnt = acnt + 1;
			if (acnt >= period) begin
				acnt = 0;
				aud_wdata <= { aseq[15:0] * 16'd1237, aseq[15:0] * 16'd4813 + 16'd77 };
				aud_we <= 1'b1;
				aleft = rep_k - 1;
				aseq = aseq + 1;
			end
		end
	end
	integer fd;
	integer errors = 0;

	always #3.968 bclk = ~bclk;                 // 126 MHz
	always #10.417 clk = ~clk;                  // 48 MHz, unrelated
	always @(posedge bclk) begin
		bdiv <= (bdiv == 3'd4) ? 3'd0 : bdiv + 3'd1;
		if (bdiv == 3'd4 || bdiv == 3'd1) pclk <= (bdiv == 3'd4);
	end

	wire [3:0] dvi_p;

`ifdef DIRECT
	// -- gpu_ddmi alone, with gpu_video.v's timing --
	reg [10:0] hc = 0, vc = 0;
	reg [7:0] fno = 0;
	wire hsync = (hc < 16) || (hc >= 16 + 96);
	wire vsync = (vc < 10) || (vc >= 10 + 2);
	wire vis = (hc >= 160) && (vc >= 45);
	wire [9:0] px = hc - 160;
	wire [9:0] py = vc - 45;
	// ddmi.py PATTERNS["xor"]: r = x^y, g = x+3y, b = 7x+y
	wire [7:0] pr = vis ? (px ^ py) : 8'd0;
	wire [7:0] pg = vis ? (px + 3 * py) : 8'd0;
	wire [7:0] pb = vis ? (px * 7 + py) : 8'd0;
	always @(posedge pclk) begin
		if (hc == 799) begin
			hc <= 0;
			if (vc == 524) begin vc <= 0; fno <= fno + 1; end
			else vc <= vc + 1;
		end else hc <= hc + 1;
	end

	wire [1:0] t_r, t_g, t_b, t_c;
	// the frame-boundary adoption gpu_video.v does, for the direct
	// case: islands_en follows ddmi_en only as a frame begins
	reg islands_en = 0;
	reg sound_en = 0;
	always @(posedge pclk)
		if (hc == 799 && vc == 524) begin
			islands_en <= ddmi_en;
			sound_en <= ddmi_sound;
		end
`ifdef GPU_DDMI_AUDIO
	gpu_ddmi #(.ISLANDS(1)) ddmi (
`else
	gpu_ddmi #(.ISLANDS(0)) ddmi (
`endif
		.pclk(pclk), .tmds_clk(bclk),
		.hc(hc), .vc(vc), .islands_en(islands_en),
		.sound_en(sound_en), .fs_code(fs_code), .acr_n(acr_n), .acr_cts(acr_cts),
		.aud_clk(clk), .aud_we(aud_we), .aud_wdata(aud_wdata),
		.in_vga_red(pr), .in_vga_green(pg), .in_vga_blue(pb),
		.in_vga_blank(!vis), .in_vga_vsync(vsync), .in_vga_hsync(hsync),
		.out_tmds_red(t_r), .out_tmds_green(t_g), .out_tmds_blue(t_b),
		.out_tmds_clk(t_c)
	);
	ODDRX1F o3 (.D0(t_c[0]), .D1(t_c[1]), .Q(dvi_p[3]), .SCLK(bclk), .RST(1'b0));
	ODDRX1F o2 (.D0(t_r[0]), .D1(t_r[1]), .Q(dvi_p[2]), .SCLK(bclk), .RST(1'b0));
	ODDRX1F o1 (.D0(t_g[0]), .D1(t_g[1]), .Q(dvi_p[1]), .SCLK(bclk), .RST(1'b0));
	ODDRX1F o0 (.D0(t_b[0]), .D1(t_b[1]), .Q(dvi_p[0]), .SCLK(bclk), .RST(1'b0));
	wire [9:0] w0 = ddmi.tmds_blue, w1 = ddmi.tmds_green, w2 = ddmi.tmds_red;
	wire frame_start = (vc == 0 && hc == 0);
`else
	// -- the whole video output --
	reg [31:0] gb_dat_i = 0;
	wire red, green, blue, hsync, vsync, is_visible, in_vblank;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dac;
	wire [15:0] frame_ctr;

	gpu_video #(.COLOR_AVAIL(1'b1)) dut (
		.pixel(1'b0), .clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.video_mode(2'd0), .video_bright(4'd7), .ddmi_en(ddmi_en),
		.ddmi_sound(ddmi_sound), .ddmi_fs(fs_code), .ddmi_acr_n(acr_n),
		.ddmi_acr_cts(acr_cts), .ddmi_we(aud_we), .ddmi_wdata(aud_wdata),
		.view_load(1'b0), .game_en(1'b0), .game_wrap(1'b0),
		.view_x(10'd0), .view_y(10'd0),
		.color_en(1'b0), .color_np(2'd0), .color_off(6'd0),
		.pal_we(1'b0), .pal_idx(4'd0), .pal_rgb(12'd0), .cvbs_mono(1'b0),
		.frame_ctr(frame_ctr), .in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue), .hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p), .dac(dac), .x(x), .y(y), .is_visible(is_visible),
		.gb_adr_o(gb_adr_o), .gb_dat_i(gb_dat_i)
	);
	// a framebuffer that is a function of the address: varied, and
	// the same on every run
	always @(posedge clk)
		gb_dat_i <= { gb_adr_o, 2'b01, ~gb_adr_o } ^ { gb_adr_o[7:0], 24'h5A3C96 };
	wire [9:0] w0 = dut.gpu_ddmi_i.tmds_blue;
	wire [9:0] w1 = dut.gpu_ddmi_i.tmds_green;
	wire [9:0] w2 = dut.gpu_ddmi_i.tmds_red;
	wire frame_start = (dut.vc == 0 && dut.hc == 0);
`endif

	// -- the serialiser check --
	//
	// Bits leave LSB first, two per bclk. ODDRX1F puts D0 (bit 2k) on
	// the pin while bclk is high and D1 (bit 2k+1) while it is low, so
	// the pin is read 1ns after each edge, in time order. The clock
	// lane carries 0000011111 (bit 0 first: five ones, five zeros)
	// every pixel; when the last ten bits read that, a word boundary
	// has just passed, and each data lane's last ten bits are one
	// character, bit 0 the oldest. They are compared with the encoder's
	// parallel words, which they trail by a fixed number of pixels.
	reg [9:0] s0 = 0, s1 = 0, s2 = 0, sc = 0;
	reg [9:0] pq0 [0:15];
	reg [9:0] pq1 [0:15];
	reg [9:0] pq2 [0:15];
	integer pn = 0;
	integer ser_lag = -1, nser = 0, nsermatch = 0, lag;
	always @(posedge pclk) if (resetn) begin
		pq0[pn % 16] <= w0; pq1[pn % 16] <= w1; pq2[pn % 16] <= w2;
		pn <= pn + 1;
	end
	always @(bclk) begin
		#1;
		s0 = { dvi_p[0], s0[9:1] }; s1 = { dvi_p[1], s1[9:1] };
		s2 = { dvi_p[2], s2[9:1] }; sc = { dvi_p[3], sc[9:1] };
		if (resetn && pn > 20 && sc == 10'b0000011111) begin
			if (ser_lag < 0) begin
				for (lag = 1; lag < 12; lag = lag + 1)
					if (ser_lag < 0 && pq0[(pn - lag) % 16] == s0
						&& pq1[(pn - lag) % 16] == s1
						&& pq2[(pn - lag) % 16] == s2)
						ser_lag = lag;
			end else begin
				nser = nser + 1;
				if (pq0[(pn - ser_lag) % 16] == s0 && pq1[(pn - ser_lag) % 16] == s1
					&& pq2[(pn - ser_lag) % 16] == s2)
					nsermatch = nsermatch + 1;
			end
		end
	end

	// -- the capture --
	integer nframes = 0;
	reg capturing = 0;
	reg [8*256-1:0] path;
	always @(posedge pclk) begin
		if (capturing)
			$fwrite(fd, "%03x %03x %03x\n", w0, w1, w2);
	end

	initial begin
		if (!$value$plusargs("out=%s", path)) path = "/tmp/ddmi_capture.txt";
		if (!$value$plusargs("frames=%d", frames)) frames = 2;
		if (!$value$plusargs("en=%d", en_arg)) en_arg = 0;
		ddmi_en = (en_arg == 1);
		if (!$value$plusargs("snd=%d", snd_arg)) snd_arg = 0;
		if (!$value$plusargs("period=%d", period)) period = 1000;
		if (!$value$plusargs("rep=%d", rep_k)) rep_k = 1;
		if ($value$plusargs("n=%d", acr_n_arg)) ;
		if ($value$plusargs("cts=%d", acr_cts_arg)) ;
		if ($value$plusargs("fs=%d", fs_arg)) ;
		acr_n = acr_n_arg;
		acr_cts = acr_cts_arg;
		fs_code = fs_arg;
		ddmi_sound = (snd_arg != 0);
		fd = $fopen(path, "w");
		$fwrite(fd, "# tb_ddmi_capture%s: ch0 ch1 ch2 per pixel clock\n",
`ifdef DIRECT
			" -DDIRECT"
`else
			""
`endif
		);
		repeat (20) @(posedge pclk);
		resetn = 1;
		// start at a frame boundary so the file is whole frames -- and
		// with +en=1, one frame later, since the enable is only
		// adopted at a frame boundary
		@(posedge pclk);
		while (!frame_start) @(posedge pclk);
		if (en_arg == 1) begin
			@(posedge pclk);
			while (!frame_start) @(posedge pclk);
		end
		capturing = 1;
		if (en_arg == 2) begin
			// half way down the first captured frame
			repeat (420000 / 2) @(posedge pclk);
			ddmi_en = 1;
			@(posedge pclk);
			while (!frame_start) @(posedge pclk);
			frames = frames - 1;
		end
		repeat (frames) begin
			@(posedge pclk);
			while (!frame_start) @(posedge pclk);
		end
		// one more pixel: the encoder's output is a register, so the
		// last character of the frame leaves one clock after it
		@(posedge pclk);
		capturing = 0;
		$fclose(fd);
		$display("tb_ddmi_capture: %0d frames to %0s", frames, path);
		if (ser_lag < 0 || nser < 1000 || nsermatch != nser) begin
			$display("FAIL serialiser: lag %0d, %0d of %0d words match", ser_lag, nsermatch, nser);
			errors = errors + 1;
		end else
			$display("  ok  serialiser: %0d words, all match (lag %0d pixels)", nser, ser_lag);
		if (errors == 0) $display("RESULT: PASS");
		else $display("RESULT: FAIL (%0d)", errors);
		$finish;
	end

endmodule

// gpu_ddmi.v also carries an iCE40 pin helper (SB_LVCMOS) that nothing
// here instantiates; iverilog still elaborates its SB_IO references.
module SB_IO (
	inout PACKAGE_PIN, input CLOCK_ENABLE, input OUTPUT_CLK,
	input OUTPUT_ENABLE, input D_OUT_0, input D_OUT_1
);
	parameter [5:0] PIN_TYPE = 6'b0;
	parameter IO_STANDARD = "";
endmodule
