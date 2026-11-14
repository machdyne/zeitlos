/*
 * Zeitlos SOC GPU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The character side of the screen modes (docs/screen_modes.md): what
 * goes on the wire, VGA and DDMI, for any of the modes rtl/gpu/
 * gpu_video.v makes. Used only by a build with VMODES; a 640x480-only
 * build keeps rtl/gpu/gpu_ddmi.v and its pixel-clock outputs exactly
 * as they were.
 *
 * -- three clocks, none of which ever changes --
 *
 *   pclk   37.8 MHz  gpu_video's pixel logic, which makes a SAMPLE on
 *                    each clock it is enabled: one pixel (640x480,
 *                    800x600) or two (1024x768, 1280x720) with their
 *                    syncs and blanking and where they are (hc, vc).
 *                    640x480 is enabled two clocks in three: 25.2 MHz.
 *   cclk   75.6 MHz  this module: one CHARACTER (a pixel on the wire)
 *                    every 3 clocks at 640x480 (25.2 MHz), every 2 at
 *                    800x600 (37.8), every clock at 1024x768 and
 *                    1280x720 (75.6).
 *   the TMDS serialiser, one of two (X2, below):
 *   sclk   189 MHz   X2: twenty bits every five clocks, four a clock
 *                    into an ODDRX2F, whose edge clock (bclk, 378 MHz)
 *                    puts them on the pin at 756 Mb/s. sclk is bclk
 *                    divided by CLKDIVF, so the fabric never runs
 *                    faster than 189 MHz. Only where the HDMI pins are
 *                    in a left or right bank: ECP5 has edge clocks, and
 *                    so ODDRX2F, nowhere else.
 *   bclk   378 MHz   otherwise: ten bits every five clocks, two a
 *                    clock into an ODDRX1F, and three registers a lane
 *                    between the shifter and the pin.
 *
 * The bit rate of the link is the character rate times ten, and only
 * 720p's (and 1024x768's) is 756 Mb/s. The slower modes keep the
 * serialiser and its clock as they are and REPEAT each bit: three
 * half-clocks a bit at 640x480 (252 Mb/s), two at 800x600 (378). So
 * the serialiser's clocks never switch, and neither does any other: a mode
 * change is a change of counters, at a frame boundary, and nothing
 * here has to be stopped, switched or re-locked.
 *
 * -- pclk to cclk --
 *
 * Both clocks come from one VCO (rtl/clk/pll1.v), cclk exactly twice
 * pclk, edges aligned, so the crossing is a timed path between related
 * clocks, as pclk to the old 126 MHz bit clock was. pclk toggles ptog
 * every clock; here it is registered twice, and the clock after it
 * changes (hev) the sample registered with it is a complete, settled
 * sample from the pclk clock just ended. s_new says whether that clock
 * was enabled -- a new sample -- or a hold.
 *
 * Samples go into a FIFO of two; the emitter takes one, sends its one
 * or two characters, C clocks each, and takes the next. The rates are
 * equal by construction, so the FIFO only absorbs 640x480's uneven
 * arrivals (two clocks out of three): it never fills, and after the
 * first sample it never runs dry. A mode change is at a frame boundary,
 * in blanking; at worst one character there is a clock long or short.
 *
 * -- characters --
 *
 * The character in hand goes to the three TMDS encoders (rtl/gpu/
 * tmds_enc_p.v) and to the data-island scheduler (rtl/gpu/
 * ddmi_island.v), each enabled once a character (go). The encoders'
 * output is spread over the character's C clocks, ten bits a clock,
 * each bit repeated C times, and handed to the serialiser. VGA takes the
 * character's colour bits and syncs, registered, for C clocks.
 *
 * Plain Verilog, no local declarations.
 */

module gpu_vchar #(
	// 1: samples carry two different pixels (the 45F boards' native
	// 1024x768 and 1280x720). 0: a two-character mode shows its one
	// pixel twice (1280x720 from a 640x360 desktop).
	parameter WIDE = 0,
	// 1: build the data islands (`GPU_DDMI_AUDIO).
	parameter ISLANDS = 0,
	// 1: the ODDRX2F serialiser on sclk; 0: ODDRX1F on bclk. tmds_x is
	// four bits a clock (D0 first) or two (bits 1:0, D0 first).
	parameter X2 = 0
) (
	input cclk,
	input sclk,
	input bclk,

	// -- from gpu_video, pclk domain --
	input ptog,
	input s_new,
	input [2:0] s_mode,
	input [23:0] s_rgb0,
	input [23:0] s_rgb1,
	input [8:0] s_vga0,
	input [8:0] s_vga1,
	input s_hs,
	input s_vs,
	input s_de,
	input [10:0] s_hc,
	input [10:0] s_vc,

	// data islands, pclk domain, frame-adopted there
	input islands_en,
	input sound_en,
	input [3:0] fs_code,
	input [19:0] acr_n,
	input [19:0] acr_cts,
	input aud_clk,
	input aud_we,
	input [31:0] aud_wdata,

	// -- out --
	output reg [8:0] vga,          // { r, g, b, r_lo[1:0], g_lo[1:0], b_lo[1:0] }
	output reg vga_hs,
	output reg vga_vs,
	output [3:0] tmds_r,
	output [3:0] tmds_g,
	output [3:0] tmds_b,
	output [3:0] tmds_c
);

	// -- the crossing --
	reg t1 = 1'b0, t2 = 1'b0;
	reg q_new = 1'b0;
	reg [2:0] q_mode = 3'd0;
	reg [23:0] q_rgb0 = 24'd0, q_rgb1 = 24'd0;
	reg [8:0] q_vga0 = 9'd0, q_vga1 = 9'd0;
	reg q_hs = 1'b1, q_vs = 1'b1, q_de = 1'b0;
	reg [10:0] q_hc = 11'd0, q_vc = 11'd0;

	always @(posedge cclk) begin
		t1 <= ptog;
		t2 <= t1;
		q_new <= s_new;
		q_mode <= s_mode;
		q_rgb0 <= s_rgb0;
		q_rgb1 <= s_rgb1;
		q_vga0 <= s_vga0;
		q_vga1 <= s_vga1;
		q_hs <= s_hs;
		q_vs <= s_vs;
		q_de <= s_de;
		q_hc <= s_hc;
		q_vc <= s_vc;
	end

	wire push = (t1 ^ t2) && q_new;

	// -- the FIFO: two samples --
	reg [2:0] f_mode0 = 3'd0, f_mode1 = 3'd0;
	reg [23:0] f_rgb00 = 24'd0, f_rgb10 = 24'd0, f_rgb01 = 24'd0, f_rgb11 = 24'd0;
	reg [8:0] f_vga00 = 9'd0, f_vga10 = 9'd0, f_vga01 = 9'd0, f_vga11 = 9'd0;
	reg f_hs0 = 1'b1, f_hs1 = 1'b1, f_vs0 = 1'b1, f_vs1 = 1'b1, f_de0 = 1'b0, f_de1 = 1'b0;
	reg [10:0] f_hc0 = 11'd0, f_hc1 = 11'd0, f_vc0 = 11'd0, f_vc1 = 11'd0;
	reg wp = 1'b0, rp = 1'b0;
	reg [1:0] fcount = 2'd0;

	// -- the emitter --
	reg act = 1'b0;          // a sample is being sent
	reg sub = 1'b0;          // its second character
	reg [1:0] cc = 2'd0;     // clocks left in this character, less one
	reg [2:0] e_mode = 3'd0; // the sample being sent
	reg [23:0] e_rgb1 = 24'd0;
	reg [8:0] e_vga1 = 9'd0;
	reg e_hs = 1'b1, e_vs = 1'b1, e_de = 1'b0;
	reg [10:0] e_hc = 11'd0, e_vc = 11'd0;

	// the character in hand, for C clocks
	reg [2:0] k_mode = 3'd0;
	reg [23:0] k_rgb = 24'd0;
	reg [8:0] k_vga = 9'd0;
	reg k_hs = 1'b1, k_vs = 1'b1, k_de = 1'b0;
	// k_hc in characters: 12 bits, for 1920x1080's 2240 a line
	reg [11:0] k_hc = 12'd0;
	reg [10:0] k_vc = 11'd0;
	reg go = 1'b0;           // the clock after a character is taken

	// A mode of two characters a sample (1024x768, 1280x720, 1280x800,
	// 1920x1080), and a character's clocks less one: three at 640x480,
	// two at 800x600 and 800x480, one in the two-character modes.
	function is_pair;
		input [2:0] m;
		begin
			is_pair = (m == 3'd2) || (m == 3'd3) || (m == 3'd5) || (m == 3'd6);
		end
	endfunction
	function [1:0] c_of;
		input [2:0] m;
		begin
			c_of = (m == 3'd0) ? 2'd2 : (m == 3'd1 || m == 3'd4) ? 2'd1 : 2'd0;
		end
	endfunction
	wire pair_e = is_pair(e_mode);
	wire [1:0] c_of_e = c_of(e_mode);
	wire [2:0] f_mode_r = rp ? f_mode1 : f_mode0;
	wire [1:0] c_of_f = c_of(f_mode_r);

	wire ends = !act || (cc == 2'd0);
	wire start_b = ends && act && pair_e && !sub;
	wire pop = ends && !start_b && (fcount != 2'd0);

	always @(posedge cclk) begin
		go <= start_b || pop;

		if (push) begin
			if (wp) begin
				f_mode1 <= q_mode; f_rgb01 <= q_rgb0; f_rgb11 <= q_rgb1;
				f_vga01 <= q_vga0; f_vga11 <= q_vga1;
				f_hs1 <= q_hs; f_vs1 <= q_vs; f_de1 <= q_de; f_hc1 <= q_hc; f_vc1 <= q_vc;
			end else begin
				f_mode0 <= q_mode; f_rgb00 <= q_rgb0; f_rgb10 <= q_rgb1;
				f_vga00 <= q_vga0; f_vga10 <= q_vga1;
				f_hs0 <= q_hs; f_vs0 <= q_vs; f_de0 <= q_de; f_hc0 <= q_hc; f_vc0 <= q_vc;
			end
			wp <= !wp;
		end

		// a push into a full FIFO is dropped (it cannot happen once
		// running; the count saturates rather than wrap)
		if (push && !pop && fcount != 2'd2)
			fcount <= fcount + 2'd1;
		else if (pop && !push)
			fcount <= fcount - 2'd1;

		if (start_b) begin
			sub <= 1'b1;
			cc <= c_of_e;
			k_mode <= e_mode;
			k_rgb <= (WIDE != 0) ? e_rgb1 : k_rgb;
			k_vga <= (WIDE != 0) ? e_vga1 : k_vga;
			k_hc <= { e_hc[10:0], 1'b1 };
		end else if (pop) begin
			act <= 1'b1;
			sub <= 1'b0;
			cc <= c_of_f;
			rp <= !rp;
			if (rp) begin
				e_mode <= f_mode1; e_rgb1 <= f_rgb11; e_vga1 <= f_vga11;
				e_hs <= f_hs1; e_vs <= f_vs1; e_de <= f_de1; e_hc <= f_hc1; e_vc <= f_vc1;
				k_mode <= f_mode1; k_rgb <= f_rgb01; k_vga <= f_vga01;
				k_hs <= f_hs1; k_vs <= f_vs1; k_de <= f_de1; k_vc <= f_vc1;
				k_hc <= is_pair(f_mode1) ? { f_hc1[10:0], 1'b0 } : { 1'b0, f_hc1 };
			end else begin
				e_mode <= f_mode0; e_rgb1 <= f_rgb10; e_vga1 <= f_vga10;
				e_hs <= f_hs0; e_vs <= f_vs0; e_de <= f_de0; e_hc <= f_hc0; e_vc <= f_vc0;
				k_mode <= f_mode0; k_rgb <= f_rgb00; k_vga <= f_vga00;
				k_hs <= f_hs0; k_vs <= f_vs0; k_de <= f_de0; k_vc <= f_vc0;
				k_hc <= is_pair(f_mode0) ? { f_hc0[10:0], 1'b0 } : { 1'b0, f_hc0 };
			end
		end else if (ends) begin
			// nothing to send: hold the last character (only before the
			// first sample, or across a mode change)
			act <= 1'b0;
		end else begin
			cc <= cc - 2'd1;
		end
	end

	// -- VGA: the character's bits, registered --
	always @(posedge cclk) begin
		vga <= k_vga;
		vga_hs <= k_hs;
		vga_vs <= k_vs;
	end

	// -- where the island goes, for the mode of the character in hand --
	//
	// In characters (a two-character mode's hc is twice the sample's):
	//
	//   mode  picture from   island at   picture lines from  vsync from  AVI
	//   0     160            12          45                  10          VIC 1
	//   1     198            40          28                  1           none
	//   2     304            12          38                  3           none
	//   3     400            12          30                  5           VIC 4
	//   4     400            12          45                  13          none, 16:9
	//   5     160            12          75                  3           none, 16:9
	//   6     320            12          45                  4           VIC 34
	//
	// 800x600's island sits inside its sync pulse (hc 40..135) so that,
	// as at 640x480, hsync does not change inside it.
	reg [11:0] i_off = 12'd0, i_vpre = 12'd150;
	reg [10:0] i_vact = 11'd45, i_vgcp = 11'd10;
	reg [2:0] i_avi = 3'd0;

	always @(posedge cclk) begin
		case (k_mode)
			3'd1: begin i_off <= 12'd28; i_vpre <= 12'd188; i_vact <= 11'd28; i_vgcp <= 11'd1; i_avi <= 3'd2; end
			3'd2: begin i_off <= 12'd0;  i_vpre <= 12'd294; i_vact <= 11'd38; i_vgcp <= 11'd3; i_avi <= 3'd2; end
			3'd3: begin i_off <= 12'd0;  i_vpre <= 12'd390; i_vact <= 11'd30; i_vgcp <= 11'd5; i_avi <= 3'd1; end
			3'd4: begin i_off <= 12'd0;  i_vpre <= 12'd390; i_vact <= 11'd45; i_vgcp <= 11'd13; i_avi <= 3'd3; end
			3'd5: begin i_off <= 12'd0;  i_vpre <= 12'd150; i_vact <= 11'd75; i_vgcp <= 11'd3; i_avi <= 3'd3; end
			3'd6: begin i_off <= 12'd0;  i_vpre <= 12'd310; i_vact <= 11'd45; i_vgcp <= 11'd4; i_avi <= 3'd4; end
			default: begin i_off <= 12'd0; i_vpre <= 12'd150; i_vact <= 11'd45; i_vgcp <= 11'd10; i_avi <= 3'd0; end
		endcase
	end

	// settings from the pixel side: quasi-static, frame-adopted there
	reg isl_en = 1'b0, snd_en = 1'b0;
	reg [3:0] fs_c = 4'd0;
	reg [19:0] n_c = 20'd0, cts_c = 20'd0;
	always @(posedge cclk) begin
		isl_en <= islands_en && (ISLANDS != 0);
		snd_en <= sound_en && (ISLANDS != 0);
		fs_c <= fs_code;
		n_c <= acr_n;
		cts_c <= acr_cts;
	end

	wire [1:0] m0, m1, m2;
	wire [3:0] i0, i1, i2;
	wire [1:0] ctl1, ctl2;

	ddmi_island island_i (
		.pclk(cclk),
		.ce(go),
		.ioff(i_off), .vpre0(i_vpre), .vact0(i_vact), .vgcp(i_vgcp), .avi_sel(i_avi),
		.en(isl_en),
		.hc(k_hc), .vc(k_vc),
		.hsync(k_hs), .vsync(k_vs),
		.sound_en(snd_en),
		.fs_code(fs_c), .acr_n(n_c), .acr_cts(cts_c),
		.aud_clk(aud_clk), .aud_we(aud_we && (ISLANDS != 0)), .aud_wdata(aud_wdata),
		.mode0(m0), .t0(i0),
		.mode1(m1), .t1(i1), .ctl1(ctl1),
		.mode2(m2), .t2(i2), .ctl2(ctl2)
	);

	// channel 0 blue with the syncs, 1 green, 2 red
	wire [9:0] e_r, e_g, e_b;
	tmds_enc_p #(.CH(2)) enc_r (.clk(cclk), .ce(go), .de(k_de), .d(k_rgb[23:16]),
		.c1(ctl2[1]), .c0(ctl2[0]), .mode(m2), .t(i2), .q(e_r));
	tmds_enc_p #(.CH(1)) enc_g (.clk(cclk), .ce(go), .de(k_de), .d(k_rgb[15:8]),
		.c1(ctl1[1]), .c0(ctl1[0]), .mode(m1), .t(i1), .q(e_g));
	tmds_enc_p #(.CH(0)) enc_b (.clk(cclk), .ce(go), .de(k_de), .d(k_rgb[7:0]),
		.c1(k_vs), .c0(k_hs), .mode(m0), .t(i0), .q(e_b));

	// -- spreading a character over its clocks --
	//
	// The encoders take a character on the clock edge go is high for, and
	// show it from then on; ph counts its clocks from that edge, and rep
	// is its bits' repeat count less one (2 at 640x480, 1 at 800x600, 0
	// otherwise). The encoders are three characters behind: every
	// character lasts as long as the one three before it, except across a
	// mode change, in blanking.
	reg [1:0] ph = 2'd0;
	reg [1:0] rep = 2'd0;
	always @(posedge cclk) begin
		if (go) begin
			ph <= 2'd0;
			rep <= c_of(k_mode);
		end else if (ph != 2'd3) begin
			ph <= ph + 2'd1;
		end
	end

	function [9:0] spread;
		input [9:0] c;
		input [1:0] r;
		input [1:0] p;
		begin
			if (r == 2'd2) begin
				case (p)
					2'd0: spread = { c[3], c[2], c[2], c[2], c[1], c[1], c[1], c[0], c[0], c[0] };
					2'd1: spread = { c[6], c[6], c[5], c[5], c[5], c[4], c[4], c[4], c[3], c[3] };
					default: spread = { c[9], c[9], c[9], c[8], c[8], c[8], c[7], c[7], c[7], c[6] };
				endcase
			end else if (r == 2'd1) begin
				if (p == 2'd0)
					spread = { c[4], c[4], c[3], c[3], c[2], c[2], c[1], c[1], c[0], c[0] };
				else
					spread = { c[9], c[9], c[8], c[8], c[7], c[7], c[6], c[6], c[5], c[5] };
			end else begin
				spread = c;
			end
		end
	endfunction

	reg [9:0] w_r = 10'd0, w_g = 10'd0, w_b = 10'd0, w_c = 10'd0;
	always @(posedge cclk) begin
		w_r <= spread(e_r, rep, ph);
		w_g <= spread(e_g, rep, ph);
		w_b <= spread(e_b, rep, ph);
		w_c <= spread(10'b0000011111, rep, ph);
	end

	// -- pairs: two cclk words, twenty bits, every other cclk --
	//
	// The serialiser takes twenty bits every five sclk, which is two
	// cclk. v_x is the word before w_x; on every other clock the two go
	// into pr_x, the earlier one in the low bits (it leaves first), and
	// ctog toggles to say a new pair is there. pr_x then holds for two
	// cclk, 26.5 ns.
	reg cpar = 1'b0;
	reg ctog = 1'b0;
	reg [9:0] v_r = 10'd0, v_g = 10'd0, v_b = 10'd0, v_c = 10'd0;
	reg [19:0] pr_r = 20'd0, pr_g = 20'd0, pr_b = 20'd0, pr_c = 20'd0;
	always @(posedge cclk) begin
		cpar <= !cpar;
		v_r <= w_r;
		v_g <= w_g;
		v_b <= w_b;
		v_c <= w_c;
		if (cpar) begin
			pr_r <= { w_r, v_r };
			pr_g <= { w_g, v_g };
			pr_b <= { w_b, v_b };
			pr_c <= { w_c, v_c };
			ctog <= !ctog;
		end
	end

	// -- the serialiser --
	//
	// Either way the phase is found once, at the start: a cclk-domain
	// toggle (ctog for X2, a new pair every other cclk; cpar for X1, a
	// new word every cclk) is registered in the serialiser's clock, its
	// first change seen (lk), and the first load made from there, well
	// inside the time the word holds still. From then on a one-hot ring of five
	// loads every fifth clock and the toggle is not looked at again: the
	// clocks come from one VCO, so the phase never moves, and a
	// synchroniser that might resolve a clock early or late is used
	// exactly once and cannot make the load wander.
	//
	// Between loads the registers rotate, low bits first (rotate rather
	// than shift zeros in: a constant on one side of the load would let
	// synthesis build that bit's load from the flop's set/reset, which is
	// slower than its data input).
	reg a1 = 1'b0, a2 = 1'b0, a3 = 1'b0;
	reg lk = 1'b0;
	reg det = 1'b0;
	reg [19:0] sh_r = 20'd0, sh_g = 20'd0, sh_b = 20'd0, sh_c = 20'd0;
	reg [3:0] o_r = 4'd0, o_g = 4'd0, o_b = 4'd0, o_c = 4'd0;
	// X1: the lock's one pulse on its way to the lanes: h_x half way to
	// lane x's pin, d_x beside it (below)
	reg h_r = 1'b0, h_g = 1'b0, h_b = 1'b0, h_c = 1'b0;
	reg d_r = 1'b0, d_g = 1'b0, d_b = 1'b0, d_c = 1'b0;

	// A ring per lane, whose bit 4 is that lane's load: each rotates
	// itself, so synthesis cannot make one of the four. X1 only: two
	// more registers a lane (p_x, q_x) before the output register.

	(* keep *) reg [4:0] rg_r = 5'b00001;
	(* keep *) reg [4:0] rg_g = 5'b00001;
	(* keep *) reg [4:0] rg_b = 5'b00001;
	(* keep *) reg [4:0] rg_c = 5'b00001;
	(* keep *) reg [1:0] p_r = 2'd0, p_g = 2'd0, p_b = 2'd0, p_c = 2'd0;
	(* keep *) reg [1:0] q_r = 2'd0, q_g = 2'd0, q_b = 2'd0, q_c = 2'd0;

	generate
		if (X2) begin : g_x2
			// 189 MHz: twenty bits every five clocks. The same shape as
			// X1 below, and for the same reason: with the lock decided in
			// front of the load, and one ring driving all eighty shift
			// flops, a path was three LUTs and a long wire, and sclk made
			// 214 MHz of its 189 on an empty chip -- too little on a full
			// one. So: det is one pulse, ever, the first time the pair's
			// toggle is seen to change; it sets each lane's own ring; a
			// ring's bit 4 is its lane's load. Every flop is one LUT from
			// flops. The ring is set to 00010, so the first load is five
			// sclk after the old one would have been: 13 to 16 ns into the
			// pair's 26.5, the middle, as before.
			always @(posedge sclk) begin
				a1 <= ctog;
				a2 <= a1;
				a3 <= a2;
				det <= !lk && (a2 ^ a3);
				lk <= lk || det;
				rg_r <= det ? 5'b00010 : { rg_r[3:0], rg_r[4] };
				rg_g <= det ? 5'b00010 : { rg_g[3:0], rg_g[4] };
				rg_b <= det ? 5'b00010 : { rg_b[3:0], rg_b[4] };
				rg_c <= det ? 5'b00010 : { rg_c[3:0], rg_c[4] };
				sh_r <= rg_r[4] ? pr_r : { sh_r[3:0], sh_r[19:4] };
				sh_g <= rg_g[4] ? pr_g : { sh_g[3:0], sh_g[19:4] };
				sh_b <= rg_b[4] ? pr_b : { sh_b[3:0], sh_b[19:4] };
				sh_c <= rg_c[4] ? pr_c : { sh_c[3:0], sh_c[19:4] };
				o_r <= sh_r[3:0];
				o_g <= sh_g[3:0];
				o_b <= sh_b[3:0];
				o_c <= sh_c[3:0];
			end
		end else begin : g_x1
			// 378 MHz: ten bits every five clocks, and a 2.6 ns clock, so
			// where things are matters as much as what they are. Each lane's
			// flip-flops sit beside its pin (tools/ddmi/pin_serialiser.py),
			// and the pins are far apart: 27 columns on Sergei ML2, 40 on
			// ULX3S. So nothing at 378 MHz may go from lane to lane, or
			// from the lock to a lane through a LUT.
			//
			// The lock is made once, in one place: cclk's edges are bclk's
			// (one VCO), so a1 takes cpar's change at the next bclk edge,
			// and det is one pulse, ever, the first time a1 changes. That
			// is the only decision; a synchroniser that might take cpar a
			// clock early or late is asked exactly once.
			//
			// det then goes out to every lane the same way, flop to flop
			// with no logic: h_x half way to the lane, d_x beside its pin.
			// Two hops of at most ten columns each, and the same two
			// clocks to every lane, so the lanes stay in step.
			//
			// d_x sets its lane's ring (rg_x), which otherwise rotates;
			// the ring's bit 4 is the lane's load. The ring is set to
			// 00100, so the first load is three clocks after d_x: eight
			// bclk after the word changed, which is three into the next
			// word -- 7.9 ns into its 13.2, the middle, with 5 ns to spare
			// either side for the word's own path from cclk.
			always @(posedge bclk) begin
				a1 <= cpar;
				a2 <= a1;
				det <= !lk && (a1 ^ a2);
				lk <= lk || det;
				rg_r <= d_r ? 5'b00100 : { rg_r[3:0], rg_r[4] };
				rg_g <= d_g ? 5'b00100 : { rg_g[3:0], rg_g[4] };
				rg_b <= d_b ? 5'b00100 : { rg_b[3:0], rg_b[4] };
				rg_c <= d_c ? 5'b00100 : { rg_c[3:0], rg_c[4] };
				sh_r[9:0] <= rg_r[4] ? w_r : { sh_r[1:0], sh_r[9:2] };
				sh_g[9:0] <= rg_g[4] ? w_g : { sh_g[1:0], sh_g[9:2] };
				sh_b[9:0] <= rg_b[4] ? w_b : { sh_b[1:0], sh_b[9:2] };
				sh_c[9:0] <= rg_c[4] ? w_c : { sh_c[1:0], sh_c[9:2] };
				p_r <= sh_r[1:0];
				p_g <= sh_g[1:0];
				p_b <= sh_b[1:0];
				p_c <= sh_c[1:0];
				q_r <= p_r;
				q_g <= p_g;
				q_b <= p_b;
				q_c <= p_c;
				o_r[1:0] <= q_r;
				o_g[1:0] <= q_g;
				o_b[1:0] <= q_b;
				o_c[1:0] <= q_c;
			end

			// The hops, in a block of their own with keep on it: four
			// flops with the same input are one flop to synthesis, which
			// would put the long wires back. keep on the block keeps the
			// cells apart (keep on the regs does not), and
			// pin_serialiser.py stops the build if they are not all there.
			(* keep *) always @(posedge bclk) begin
				h_r <= det;
				h_g <= det;
				h_b <= det;
				h_c <= det;
				d_r <= h_r;
				d_g <= h_g;
				d_b <= h_b;
				d_c <= h_c;
			end
		end
	endgenerate

	assign tmds_r = o_r;
	assign tmds_g = o_g;
	assign tmds_b = o_b;
	assign tmds_c = o_c;

endmodule
