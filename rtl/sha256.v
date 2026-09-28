/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SHA-256 compression -- one 64-byte block into the eight-word state.
 * docs/sha256_hw.md.
 *
 * -- What this is for --
 *
 * Measured on Lakritz (docs/cryptobench.md), software SHA-256 costs
 * ~44,000 cycles a block. The BBS's 2000-round password hash is
 * 4.26 s of it; zfed hashes every object; SSH MACs every packet; TLS
 * hashes its transcript. This block does the compression in ~144
 * cycles, and sw/common/zsha256.c uses it when it is there -- every
 * caller of z_sha256_*() gets it without changing a line.
 *
 * -- Shape, and why --
 *
 * The board this has to fit is at 91% of its LUTs and 40% of its
 * flip-flops, with block RAM to spare. So storage goes where LUTs are
 * not spent:
 *
 *   - the message schedule is a 16-word SHIFT REGISTER: 512 flip-flops
 *     and no multiplexers, W[t] always at the bottom;
 *   - the 64 round constants are a ROM in ONE BLOCK RAM;
 *   - the state H0..H7 is eight words of DISTRIBUTED RAM, loaded into
 *     and added back from the working variables one word per cycle
 *     through the same shift chain the rounds use;
 *   - a round takes TWO cycles, so no path has more than two 32-bit
 *     adders in series. 144 cycles a block instead of ~72 costs
 *     nothing that shows: software spends more than that pushing the
 *     block's sixteen words.
 *
 * The block keeps nothing between calls that software needs: software
 * writes H from its own context, pushes blocks, and reads H back. So a
 * process can lose the block between two calls -- to preemption, to
 * another process -- without harm (docs/sha256_hw.md, "Sharing").
 *
 * -- Register map (word-addressed, base 0x7e00_0000; the SOC hands the
 *    block word addresses, as it does montmul) --
 *
 *    0  MAGIC   read: 32'h5A534841 ("ZSHA").
 *    1  CTRL    write bit 0: START -- compress the sixteen words pushed
 *               into H. read bit 0: BUSY.
 *    2  CONFIG  read: { 16'h5348, 8'd0, 8'd1 } -- version 1.
 *    3  OWNER   the advisory claim, exactly as montmul's: read the
 *               owner (0: none); write a pid to claim if free; write
 *               pid | 2^31 to release if yours.
 *    4  W       write: push one message word AS LOADED FROM MEMORY on
 *               this little-endian CPU -- the block swaps its bytes,
 *               since SHA-256 reads bytes big-endian. Sixteen pushes
 *               make a block.
 *    5  WBE     write: push one word already big-endian (no swap).
 *    8..15 H    read/write: the state, H0..H7, as numbers.
 *
 * Writes to W, WBE and H while BUSY are ignored.
 */

module sha256 (
	input clk,
	input resetn,

	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output wb_ack_o,
	input wb_cyc_i
);

	// -- the round constants: one block RAM, synchronous read --------

	// Forced into block RAM: left to itself yosys judges 64 words
	// too small for one and builds the ROM from LUTs -- ~200 of them,
	// on a board where LUTs are the scarce thing and RAM is not.
	(* rom_style = "block" *) (* ram_style = "block" *)
	reg [31:0] krom [0:63];
	reg [31:0] k_q;
	reg [5:0]  rnd;					// the round; 63 while idle (see k_addr)
	wire [5:0] k_addr = rnd + 6'd1;	// always the NEXT round's constant

	always @(posedge clk) k_q <= krom[k_addr];

	initial begin
		krom[ 0] = 32'h428a2f98; krom[ 1] = 32'h71374491; krom[ 2] = 32'hb5c0fbcf; krom[ 3] = 32'he9b5dba5;
		krom[ 4] = 32'h3956c25b; krom[ 5] = 32'h59f111f1; krom[ 6] = 32'h923f82a4; krom[ 7] = 32'hab1c5ed5;
		krom[ 8] = 32'hd807aa98; krom[ 9] = 32'h12835b01; krom[10] = 32'h243185be; krom[11] = 32'h550c7dc3;
		krom[12] = 32'h72be5d74; krom[13] = 32'h80deb1fe; krom[14] = 32'h9bdc06a7; krom[15] = 32'hc19bf174;
		krom[16] = 32'he49b69c1; krom[17] = 32'hefbe4786; krom[18] = 32'h0fc19dc6; krom[19] = 32'h240ca1cc;
		krom[20] = 32'h2de92c6f; krom[21] = 32'h4a7484aa; krom[22] = 32'h5cb0a9dc; krom[23] = 32'h76f988da;
		krom[24] = 32'h983e5152; krom[25] = 32'ha831c66d; krom[26] = 32'hb00327c8; krom[27] = 32'hbf597fc7;
		krom[28] = 32'hc6e00bf3; krom[29] = 32'hd5a79147; krom[30] = 32'h06ca6351; krom[31] = 32'h14292967;
		krom[32] = 32'h27b70a85; krom[33] = 32'h2e1b2138; krom[34] = 32'h4d2c6dfc; krom[35] = 32'h53380d13;
		krom[36] = 32'h650a7354; krom[37] = 32'h766a0abb; krom[38] = 32'h81c2c92e; krom[39] = 32'h92722c85;
		krom[40] = 32'ha2bfe8a1; krom[41] = 32'ha81a664b; krom[42] = 32'hc24b8b70; krom[43] = 32'hc76c51a3;
		krom[44] = 32'hd192e819; krom[45] = 32'hd6990624; krom[46] = 32'hf40e3585; krom[47] = 32'h106aa070;
		krom[48] = 32'h19a4c116; krom[49] = 32'h1e376c08; krom[50] = 32'h2748774c; krom[51] = 32'h34b0bcb5;
		krom[52] = 32'h391c0cb3; krom[53] = 32'h4ed8aa4a; krom[54] = 32'h5b9cca4f; krom[55] = 32'h682e6ff3;
		krom[56] = 32'h748f82ee; krom[57] = 32'h78a5636f; krom[58] = 32'h84c87814; krom[59] = 32'h8cc70208;
		krom[60] = 32'h90befffa; krom[61] = 32'ha4506ceb; krom[62] = 32'hbef9a3f7; krom[63] = 32'hc67178f2;
	end

	// -- the state H0..H7: distributed RAM, one write port -----------

	reg [31:0] hmem [0:7];
	reg [2:0]  h_wa;
	reg [31:0] h_wd;
	reg        h_we;
	reg [2:0]  h_ra_eng;
	wire [2:0] h_ra;
	wire [31:0] h_rd = hmem[h_ra];

	always @(posedge clk) if (h_we) hmem[h_wa] <= h_wd;

	// -- the working variables and the schedule ----------------------

	reg [31:0] a, b, c, d, e, f, g, h;
	reg [31:0] w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, w14, w15;
	reg [31:0] x1, x2, t2;			// the first cycle's partial sums

	// SHA-256's functions
	wire [31:0] bsig0 = {a[1:0], a[31:2]} ^ {a[12:0], a[31:13]} ^ {a[21:0], a[31:22]};
	wire [31:0] bsig1 = {e[5:0], e[31:6]} ^ {e[10:0], e[31:11]} ^ {e[24:0], e[31:25]};
	wire [31:0] ch    = (e & f) ^ (~e & g);
	wire [31:0] maj   = (a & b) ^ (a & c) ^ (b & c);
	wire [31:0] ssig0 = {w1[6:0], w1[31:7]} ^ {w1[17:0], w1[31:18]} ^ {3'd0, w1[31:3]};
	wire [31:0] ssig1 = {w14[16:0], w14[31:17]} ^ {w14[18:0], w14[31:19]} ^ {10'd0, w14[31:10]};
	// W[t+16], from W[t+14], W[t+9], W[t+1] and W[t]
	wire [31:0] w_next = ssig1 + w9 + ssig0 + w0;
	wire [31:0] t1 = x1 + x2;

	// -- bus ---------------------------------------------------------

	localparam S_IDLE  = 3'd0;
	localparam S_LOAD  = 3'd1;		// H7..H0 into the chain: a = H0 .. h = H7
	localparam S_RA    = 3'd2;		// a round, first cycle
	localparam S_RB    = 3'd3;		// a round, second cycle
	localparam S_ADD   = 3'd4;		// H7..H0 += h, the chain rotating

	reg [2:0] st;
	wire busy = (st != S_IDLE);
	reg [3:0] n;					// LOAD / ADD counter
	reg [30:0] owner;

	reg [31:0] dat_r;
	reg ack_r;
	assign wb_dat_o = dat_r;
	assign wb_ack_o = ack_r;

	wire sel = wb_cyc_i && wb_stb_i && !ack_r;
	wire wr  = sel && wb_we_i;
	wire [4:0] wa = wb_adr_i[4:0];
	wire is_h = (wa[4:3] == 2'b01);	// 8..15

	// H's read port: the bus's word while idle, the engine's otherwise
	assign h_ra = busy ? h_ra_eng : wa[2:0];

	wire [31:0] swapped = { wb_dat_i[7:0], wb_dat_i[15:8], wb_dat_i[23:16], wb_dat_i[31:24] };
	wire push = wr && !busy && (wa == 5'd4 || wa == 5'd5);
	wire [31:0] push_word = (wa == 5'd4) ? swapped : wb_dat_i;

	always @(posedge clk) begin

		ack_r <= 1'b0;
		h_we <= 1'b0;

		if (!resetn) begin
			st <= S_IDLE;
			rnd <= 6'd63;
			owner <= 31'd0;
			dat_r <= 32'd0;
		end else begin

			if (sel) begin
				ack_r <= 1'b1;
				dat_r <= 32'd0;
				if (wr) begin
					if (wa == 5'd1) begin
						if (wb_dat_i[0] && !busy) begin
							n <= 4'd0;
							h_ra_eng <= 3'd7;
							st <= S_LOAD;
						end
					end else if (wa == 5'd3) begin
						if (wb_dat_i[31]) begin
							if (owner == wb_dat_i[30:0]) owner <= 31'd0;
						end else if (owner == 31'd0) begin
							owner <= wb_dat_i[30:0];
						end
					end else if (is_h && !busy) begin
						h_wa <= wa[2:0]; h_wd <= wb_dat_i; h_we <= 1'b1;
					end
				end else begin
					if (wa == 5'd0) dat_r <= 32'h5A534841;
					else if (wa == 5'd1) dat_r <= { 31'd0, busy };
					else if (wa == 5'd2) dat_r <= { 16'h5348, 8'd0, 8'd1 };
					else if (wa == 5'd3) dat_r <= { 1'b0, owner };
					else if (is_h) dat_r <= h_rd;
				end
			end

			case (st)

			// Eight cycles: H7 first, so that after eight shifts
			// a = H0 and h = H7. e takes d, as in a round's shift.
			S_LOAD: begin
				h <= g; g <= f; f <= e; e <= d;
				d <= c; c <= b; b <= a; a <= h_rd;
				h_ra_eng <= h_ra_eng - 3'd1;
				if (n == 4'd7) begin
					rnd <= 6'd0;			// k_q already holds K[0]: see k_addr
					st <= S_RA;
				end else n <= n + 4'd1;
			end

			// First cycle: h + K + W, S1(e) + Ch, S0(a) + Maj. At most
			// two adders in series.
			S_RA: begin
				x1 <= h + k_q + w0;
				x2 <= bsig1 + ch;
				t2 <= bsig0 + maj;
				st <= S_RB;
			end

			// Second cycle: T1 = x1 + x2; a = T1 + T2, e = d + T1;
			// the chain and the schedule shift.
			S_RB: begin
				h <= g; g <= f; f <= e; e <= d + t1;
				d <= c; c <= b; b <= a; a <= t1 + t2;
				w0 <= w1; w1 <= w2; w2 <= w3; w3 <= w4;
				w4 <= w5; w5 <= w6; w6 <= w7; w7 <= w8;
				w8 <= w9; w9 <= w10; w10 <= w11; w11 <= w12;
				w12 <= w13; w13 <= w14; w14 <= w15; w15 <= w_next;
				if (rnd == 6'd63) begin
					n <= 4'd0;
					h_ra_eng <= 3'd7;
					st <= S_ADD;		// rnd stays 63: idle's value
				end else begin
					rnd <= rnd + 6'd1;
					st <= S_RA;
				end
			end

			// Eight cycles: H[i] += the chain's last word, for i = 7
			// down to 0, rotating the chain one step each time.
			S_ADD: begin
				h_wa <= h_ra_eng; h_wd <= h_rd + h; h_we <= 1'b1;
				h <= g; g <= f; f <= e; e <= d;
				d <= c; c <= b; b <= a;
				h_ra_eng <= h_ra_eng - 3'd1;
				if (n == 4'd7) st <= S_IDLE;
				else n <= n + 4'd1;
			end

			default: ;

			endcase

			// A pushed word enters at the top of the schedule. Only
			// while idle, so it never meets the rounds' shift.
			if (push) begin
				w0 <= w1; w1 <= w2; w2 <= w3; w3 <= w4;
				w4 <= w5; w5 <= w6; w6 <= w7; w7 <= w8;
				w8 <= w9; w9 <= w10; w10 <= w11; w11 <= w12;
				w12 <= w13; w13 <= w14; w14 <= w15; w15 <= push_word;
			end

		end

	end

endmodule
