/*
 * Zeitlos SOC GPU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * gpu_ckfetch.v -- chunky game mode's picture from main memory, a line
 * ahead of the beam (`VRAM_SDRAM; docs/profiles.md, "Lakritz").
 *
 * -- why --
 *
 * A 320x240 picture at a byte a pixel is 19,200 words: 40 of the 25F's
 * 56 block RAMs as VRAM, and T2 needs 30 of its own. So on the console
 * profile VRAM holds only two 320x240 bitplanes (4,800 words: the
 * blitter, the line rasterizer and the CPU draw there exactly as
 * before, a word a cycle), and the chunky picture is in main memory:
 * T2 writes it there (gpu_t2.v, LINEAR), software draws in it, and
 * this module reads it out for gpu_video.
 *
 * -- the shape --
 *
 * gpu_video is not changed. It reads a chunky line as it always has --
 * four segments of 20 words, at VRAM word addresses 20(r + 240s) + w
 * (docs/color.md, "Chunky") -- on its graphics port, an address on one
 * edge and the word two edges later, with no wait. That timing is
 * block RAM's, and main memory cannot meet it, so this module sits on
 * the graphics port and answers it from a block RAM of its own holding
 * TWO LINES: the one being shown and the next (LINES = 4: and two more).
 * While line L is shown, the lines after it are read from main memory,
 * each 80 words in 20 four-word bursts
 * (`SDRAM_BURST), in about 300-600 cycles with the bus shared, against
 * about 3,000 a game-mode line lasts (two scanlines at 48 MHz).
 *
 * Which line is next comes from gpu_video's own refill row (gb_row,
 * already in this clock's domain): the line it is about to read. A
 * line that is not here when it is read (the first line after chunky
 * comes on, or a bus so busy the fetch fell a line behind) reads as 0
 * -- palette entry 0, for one line of one frame -- and is fetched at
 * once; `misses` counts them.
 *
 * In main memory the picture is LINEAR: line y at base + 320 y, pixel x
 * byte x of it. That is the layout software wants to draw in, and the
 * order a line is fetched in; this module turns it back into the four
 * segments gpu_video asks for. base (socctl's CKBASE) is taken as the
 * frame's line 0 is fetched -- while line 239 of the frame before is
 * shown -- so a new base is a page flip at the next frame, no tearing.
 *
 * With chunky off the port is VRAM's, unchanged: vram_dat, made 0 past
 * VRAM_WORDS (the planes' VRAM is smaller than the framebuffer software
 * knows about; rtl/mem/vram.v's GUARD drops the writes there).
 *
 * Nothing is fetched unless CKBASE is in main memory (0x4xxx_xxxx, or
 * 0x5xxx_xxxx too with MAIN_512): at
 * its reset value, 0, which a program from before `VRAM_SDRAM leaves it
 * at when it turns chunky on, a fetch from address 0 up reaches past
 * the boot block RAM, where no slave answers -- the main bus would wait
 * for ever, and the machine with it; and an app address not made
 * physical (0x8xxx_xxxx) is no better. Such a program shows what VRAM
 * holds instead.
 *
 * The view's y must be 0 in chunky (as docs/color.md says already);
 * with any other a line's segments come from different lines and miss.
 *
 * Plain Verilog-2005: no declarations inside blocks, one driver a signal.
 */

module gpu_ckfetch #(
	// four-word read bursts (rtl/mem/sdram_kianv.v BURST=1, CTI 010);
	// 0: single reads
	parameter BURST = 1,
	// words of VRAM behind vram_dat; reads past it are 0
	parameter integer VRAM_WORDS = 4800,
	// main memory's address nibble: a CKBASE elsewhere fetches nothing
	parameter [3:0] MAIN_NIBBLE = 4'h4,
	// 1: main memory is 512 MB, 0x4000_0000-0x5fff_ffff (`MAIN_512MB)
	parameter MAIN_512 = 0,
	// lines held ahead of the beam: 2 (the one shown and the next), or
	// 4 (and two more: a fetch late by up to two lines catches up; for a
	// busy bus, `CKFETCH_LINES in rtl/sysctl.v)
	parameter LINES = 2
) (
	input             clk,
	input             rst,

	input             en,           // chunky on (socctl COLOR CHUNKY, EN, game mode)
	input      [31:0] base,         // socctl CKBASE: the picture, physical, 16-byte aligned
	input      [9:0]  gb_row,       // gpu_video's refill row: the line it reads next

	// the graphics port: gpu_video on one side, VRAM on the other
	input      [14:0] gb_adr,       // from gpu_video
	output     [31:0] gb_dat,       // to gpu_video, two edges after gb_adr
	input      [31:0] vram_dat,     // VRAM's registered graphics-port read

	// Wishbone master: main memory, reads only
	output reg [31:0] m_adr_o,
	output reg        m_cyc_o,
	output reg        m_stb_o,
	output reg [2:0]  m_cti_o,
	output     [3:0]  m_sel_o,
	output            m_we_o,
	input      [31:0] m_dat_i,
	input             m_ack_i,

	output reg [15:0] misses,       // lines not here in time
	output reg [15:0] lines         // lines fetched
);

	assign m_sel_o = 4'b1111;
	assign m_we_o = 1'b0;

	// on: chunky, with a picture in main memory to fetch (the header)
	wire on = en && ((base[31:28] == MAIN_NIBBLE) ||
	                 (MAIN_512 != 0 && base[31:28] == MAIN_NIBBLE + 4'h1));

	// ------------------------------------------------------------------
	// up to four lines: { slot, segment, word }, 4 x 4 x 20 of 512 (one
	// DP16KD, as two lines' 256 were). With LINES = 4 the line shown and
	// the three after it: a fetch can fall more than a line behind (the
	// bus busy with T2 and the CPU) and catch up before its line is
	// read. With two, a line late by a little reads as 0 at its start:
	// short black lines on the left of sergei_ml2_console's picture with
	// T2's overlap options (2026-11-06).
	// ------------------------------------------------------------------

	reg [31:0] lb [0:511];

	reg        v0, v1, v2, v3;      // slot holds its line
	reg [7:0]  ln0, ln1, ln2, ln3;  // the line it holds (0..239)
	// a slot's line's segment 0 VRAM word address: 20 line
	reg [14:0] c0, c1, c2, c3;

	// ------------------------------------------------------------------
	// the read side: which slot and segment holds gb_adr
	// ------------------------------------------------------------------

	// The segment from the address alone (segment s is VRAM rows
	// 240 s .. 240 s + 239, words 4800 s up), then the address as
	// segment 0's, against each slot's line: two subtractions, not one
	// for every slot and segment.
	wire [1:0]  sg = (gb_adr >= 15'd14400) ? 2'd3 : (gb_adr >= 15'd9600) ? 2'd2 :
	                 (gb_adr >= 15'd4800) ? 2'd1 : 2'd0;
	wire [14:0] a0 = gb_adr - ((sg == 2'd3) ? 15'd14400 : (sg == 2'd2) ? 15'd9600 :
	                           (sg == 2'd1) ? 15'd4800 : 15'd0);
	wire [15:0] d0 = {1'b0, a0} - {1'b0, c0};
	wire [15:0] d1 = {1'b0, a0} - {1'b0, c1};
	wire [15:0] d2 = {1'b0, a0} - {1'b0, c2};
	wire [15:0] d3 = {1'b0, a0} - {1'b0, c3};
	// in [0, 20): a negative difference has bit 15 set and is not
	wire h0 = v0 && (d0 < 16'd20);
	wire h1 = v1 && (d1 < 16'd20);
	wire h2 = v2 && (d2 < 16'd20);
	wire h3 = v3 && (d3 < 16'd20);
	wire hit = h0 | h1 | h2 | h3;
	// the slots hold different lines: at most one hits
	wire [8:0] ra = h3 ? {2'd3, sg, d3[4:0]} : h2 ? {2'd2, sg, d2[4:0]} :
	                h1 ? {2'd1, sg, d1[4:0]} : {2'd0, sg, d0[4:0]};

	reg [31:0] ck_q;
	reg        sel_q, hit_q, vin_q;
	always @(posedge clk) begin
		ck_q <= lb[ra];
		sel_q <= on;
		hit_q <= hit;
		vin_q <= (gb_adr < VRAM_WORDS);
	end

	assign gb_dat = sel_q ? (hit_q ? ck_q : 32'd0) : (vin_q ? vram_dat : 32'd0);

	// ------------------------------------------------------------------
	// the fetch
	// ------------------------------------------------------------------

	// the line about to be read, and the three after it
	// Registered: gb_row moves once a line, and a value a clock old
	// names lines already fetched, so the fetch only starts a clock later.
	// (It was the slowest path of lakritz_console.)
	wire [9:0] l_full = (gb_row >= 10'd240) ? (gb_row - 10'd240) : gb_row;
	wire [7:0] lc_d = (l_full >= 10'd240) ? 8'd0 : l_full[7:0];
	reg  [7:0] lc, lw1, lw2, lw3;
	// the next frame's lines only once its last line is about to be
	// read: line 0 takes CKBASE as it is fetched (a page flip), when
	// line 239 is next, as with two lines -- zgl counts on that
	// (zgl_t2.md, "Never over a picture waiting for its boundary")
	reg        al2, al3;
	always @(posedge clk) begin
		lc <= lc_d;
		lw1 <= (lc_d == 8'd239) ? 8'd0 : (lc_d + 8'd1);
		lw2 <= (lc_d == 8'd239) ? 8'd1 : (lc_d + 8'd2);
		lw3 <= (lc_d == 8'd239) ? 8'd2 : (lc_d + 8'd3);
		al2 <= (LINES > 2) && ((lc_d == 8'd239) || (lc_d <= 8'd237));
		al3 <= (LINES > 2) && ((lc_d == 8'd239) || (lc_d <= 8'd236));
	end

	// which slots hold the line shown and those after it
	wire s0c = v0 && (ln0 == lc), s1c = v1 && (ln1 == lc), s2c = v2 && (ln2 == lc), s3c = v3 && (ln3 == lc);
	wire s01 = v0 && (ln0 == lw1), s11 = v1 && (ln1 == lw1), s21 = v2 && (ln2 == lw1), s31 = v3 && (ln3 == lw1);
	wire s02 = v0 && (ln0 == lw2) && al2, s12 = v1 && (ln1 == lw2) && al2,
	     s22 = v2 && (ln2 == lw2) && al2, s32 = v3 && (ln3 == lw2) && al2;
	wire s03 = v0 && (ln0 == lw3) && al3, s13 = v1 && (ln1 == lw3) && al3,
	     s23 = v2 && (ln2 == lw3) && al3, s33 = v3 && (ln3 == lw3) && al3;
	wire hasc = s0c | s1c | s2c | s3c;
	wire has1 = s01 | s11 | s21 | s31;
	wire has2 = s02 | s12 | s22 | s32;
	wire has3 = s03 | s13 | s23 | s33;
	// a slot the window does not need: the next fetch's
	wire k0 = s0c | s01 | s02 | s03, k1 = s1c | s11 | s12 | s13,
	     k2 = s2c | s21 | s22 | s23, k3 = s3c | s31 | s32 | s33;
	wire [1:0] free = !k0 ? 2'd0 : !k1 ? 2'd1 : !k2 ? 2'd2 : 2'd3;

	localparam [2:0] F_IDLE = 3'd0, F_START = 3'd1, F_REQ = 3'd2, F_GAP = 3'd3,
	                 F_STB = 3'd4;
	reg [2:0]  fs;
	reg [1:0]  f_slot;
	reg [7:0]  f_line;
	reg [1:0]  f_k;                 // segment
	reg [4:0]  f_w;                 // its word
	reg [1:0]  f_beat;
	reg [31:0] fbase;               // the frame's base
	reg        fb_ok;               // fbase taken since chunky came on

	// 320 N and 20 N, for the line starting
	wire [16:0] f_off = {1'b0, f_line, 8'd0} + {3'b0, f_line, 6'd0};
	wire [14:0] f_c   = {3'd0, f_line, 4'd0} + {5'd0, f_line, 2'd0};
	wire [31:0] f_base = (f_line == 8'd0 || !fb_ok) ? base : fbase;
	wire        f_last = (f_k == 2'd3) && (f_w == 5'd19);

	always @(posedge clk) begin
		if (rst) begin
			fs <= F_IDLE;
			v0 <= 1'b0; v1 <= 1'b0; v2 <= 1'b0; v3 <= 1'b0;
			ln0 <= 8'd0; ln1 <= 8'd0; ln2 <= 8'd0; ln3 <= 8'd0;
			fb_ok <= 1'b0;
			fbase <= 32'd0;
			m_cyc_o <= 1'b0;
			m_stb_o <= 1'b0;
			m_cti_o <= 3'b000;
			m_adr_o <= 32'd0;
			f_slot <= 2'd0;
			f_line <= 8'd0;
			f_k <= 2'd0;
			f_w <= 5'd0;
			f_beat <= 2'd0;
			misses <= 16'd0;
			lines <= 16'd0;
		end else begin
			case (fs)
			F_IDLE: begin
				if (!on) begin
					v0 <= 1'b0; v1 <= 1'b0; v2 <= 1'b0; v3 <= 1'b0;
					fb_ok <= 1'b0;
				end else if (!hasc || !has1 || (al2 && !has2) || (al3 && !has3)) begin
					// the first line the window lacks (the one shown:
					// late, or the first), into a slot it does not need
					f_line <= !hasc ? lc : !has1 ? lw1 : (al2 && !has2) ? lw2 : lw3;
					f_slot <= free;
					case (free)
					2'd0: v0 <= 1'b0;
					2'd1: v1 <= 1'b0;
					2'd2: v2 <= 1'b0;
					default: v3 <= 1'b0;
					endcase
					if (!hasc) misses <= misses + 16'd1;
					fs <= F_START;
				end
			end

			F_START: begin
				// the slot's segment addresses, and the first burst
				case (f_slot)
				2'd0: begin ln0 <= f_line; c0 <= f_c; end
				2'd1: begin ln1 <= f_line; c1 <= f_c; end
				2'd2: begin ln2 <= f_line; c2 <= f_c; end
				default: begin ln3 <= f_line; c3 <= f_c; end
				endcase
				if (f_line == 8'd0 || !fb_ok) begin
					fbase <= base;
					fb_ok <= 1'b1;
				end
				m_adr_o <= {f_base[31:4], 4'd0} + {15'd0, f_off};
				f_k <= 2'd0;
				f_w <= 5'd0;
				f_beat <= 2'd0;
				m_cyc_o <= 1'b1;
				m_stb_o <= 1'b1;
				m_cti_o <= BURST ? 3'b010 : 3'b000;
				fs <= F_REQ;
			end

			F_REQ: begin
				if (m_ack_i) begin
					m_adr_o <= m_adr_o + 32'd4;
					f_beat <= f_beat + 2'd1;
					if (f_w == 5'd19) begin
						f_w <= 5'd0;
						f_k <= f_k + 2'd1;
					end else f_w <= f_w + 5'd1;
					if (f_last) begin
						m_cyc_o <= 1'b0;
						m_stb_o <= 1'b0;
						m_cti_o <= 3'b000;
						case (f_slot)
						2'd0: v0 <= 1'b1;
						2'd1: v1 <= 1'b1;
						2'd2: v2 <= 1'b1;
						default: v3 <= 1'b1;
						endcase
						lines <= lines + 16'd1;
						fs <= F_IDLE;
					end else if (BURST ? (f_beat == 2'd3) : (f_w == 5'd19)) begin
						// a burst done, or with single reads a
						// segment (20 words): let the bus go
						m_cyc_o <= 1'b0;
						m_stb_o <= 1'b0;
						m_cti_o <= 3'b000;
						fs <= F_GAP;
					end else if (!BURST) begin
						// single reads: STB drops between the words,
						// CYC does not -- the grant is kept for the
						// segment, as the caches' classic line fills
						// keep theirs (the DDR3 controller answers the
						// rest of a 16-byte block from the one it
						// holds, a cycle each). A word a grant was too
						// slow on DDR3 for a line in time: 30 of 94
						// lines wrong in tb_console_ddr3.v, none now.
						m_stb_o <= 1'b0;
						fs <= F_STB;
					end else begin
						m_cti_o <= (f_beat == 2'd2) ? 3'b111 : 3'b010;
					end
				end
			end

			F_STB: begin
				m_stb_o <= 1'b1;
				fs <= F_REQ;
			end

			F_GAP: begin
				m_cyc_o <= 1'b1;
				m_stb_o <= 1'b1;
				m_cti_o <= BURST ? 3'b010 : 3'b000;
				f_beat <= 2'd0;
				fs <= F_REQ;
			end
			endcase
		end
	end

	// the line buffer's write port
	always @(posedge clk) begin
		if (fs == F_REQ && m_ack_i)
			lb[{f_slot, f_k, f_w}] <= m_dat_i;
	end

endmodule
