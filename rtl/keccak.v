/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Keccak-f[1600] -- the permutation under SHA-3, SHAKE, ML-KEM, ML-DSA
 * and SLH-DSA. docs/keccak_hw.md.
 *
 * -- What this is for --
 *
 * Measured (docs/cryptobench.md): one permutation in software is
 * ~169,000 cycles -- 64-bit lanes on a 32-bit CPU with no data cache --
 * and 51-61% of each ML-KEM-768 operation. ML-DSA and SLH-DSA
 * verification, the post-quantum signatures, are mostly or almost all
 * Keccak, once per object checked. Here a permutation is 24 cycles;
 * moving the state in and out over the bus costs far more than that,
 * and is still ~50x less than software. sw/common/zkeccak.c uses it
 * when it is there.
 *
 * -- Shape, and why --
 *
 * One round per clock, the state in 1,600 flip-flops: the simplest
 * thing that is right, and a faster core would not show -- the bus
 * transfers dominate.
 *
 * The bus does NOT address the 50 state words individually: that would
 * put a five-way multiplexer (hold, round, write, xor-write, clear) in
 * front of every one of the 1,600 bits -- thousands of LUTs of
 * multiplexing alone. The state is a 50-word rotating shift register
 * instead, so each bit only chooses between holding, the round, and its
 * neighbour; clearing is the flip-flops' own synchronous reset.
 *
 *   IN    write: the state shifts down one word, this word entering at
 *         the top. Fifty writes load it, word 0 first.
 *   XIN   write: the same, XORing this word into the word leaving the
 *         bottom -- fifty writes absorb fifty words into the state,
 *         without reading it (zeros where nothing is absorbed).
 *   OUT   read: the bottom word, and the state rotates one word. Fifty
 *         reads return it, word 0 first, and leave it as it was.
 *
 * Word k is bits 32k+31..32k: lane i (i = x + 5y, as in FIPS 202 and
 * the reference code) is words 2i (low half) and 2i+1 (high half) --
 * a uint64_t state[25] as it lies in this little-endian CPU's memory.
 *
 * -- Register map (word-addressed, base 0x7d00_0000) --
 *
 *    0  MAGIC   read: 32'h5A4B4543 ("ZKEC").
 *    1  CTRL    write bit 0: START (24 rounds); bit 1: CLEAR (the state
 *               to zeros). read bit 0: BUSY.
 *    2  CONFIG  read: { 16'h4B45, 8'd0, 8'd1 } -- version 1.
 *    3  OWNER   the advisory claim, exactly as montmul's and sha256's:
 *               read the owner (0: none); write a pid to claim if free;
 *               write pid | 2^31 to release if yours.
 *    4  IN      write, as above.
 *    5  XIN     write, as above.
 *    6  OUT     read, as above.
 *
 * While BUSY, IN, XIN, OUT and CLEAR are ignored (OUT reads 0).
 */

module keccak (
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

	reg [1599:0] st;				// the state
	reg [1599:0] rnd_out;			// one round of it (combinational)
	reg [4:0] rnd;					// the round, 0..23
	reg busy;
	reg [30:0] owner;

	reg [31:0] dat_r;
	reg ack_r;
	assign wb_dat_o = dat_r;
	assign wb_ack_o = ack_r;

	wire sel = wb_cyc_i && wb_stb_i && !ack_r;
	wire wr  = sel && wb_we_i;
	wire rd  = sel && !wb_we_i;
	wire [2:0] wa = wb_adr_i[2:0];

	// -- one round --

	function [63:0] rotl;
		input [63:0] v;
		input integer n;
		begin
			rotl = (n == 0) ? v : ((v << n) | (v >> (64 - n)));
		end
	endfunction

	// the rotation offsets, for lane i = x + 5y
	function integer rho;
		input integer i;
		begin
			case (i)
			 0: rho =  0;  1: rho =  1;  2: rho = 62;  3: rho = 28;  4: rho = 27;
			 5: rho = 36;  6: rho = 44;  7: rho =  6;  8: rho = 55;  9: rho = 20;
			10: rho =  3; 11: rho = 10; 12: rho = 43; 13: rho = 25; 14: rho = 39;
			15: rho = 41; 16: rho = 45; 17: rho = 15; 18: rho = 21; 19: rho =  8;
			20: rho = 18; 21: rho =  2; 22: rho = 61; 23: rho = 56; 24: rho = 14;
			default: rho = 0;
			endcase
		end
	endfunction

	reg [63:0] rc;
	always @(*) begin
		case (rnd)
		5'd0:  rc = 64'h0000000000000001; 5'd1:  rc = 64'h0000000000008082;
		5'd2:  rc = 64'h800000000000808A; 5'd3:  rc = 64'h8000000080008000;
		5'd4:  rc = 64'h000000000000808B; 5'd5:  rc = 64'h0000000080000001;
		5'd6:  rc = 64'h8000000080008081; 5'd7:  rc = 64'h8000000000008009;
		5'd8:  rc = 64'h000000000000008A; 5'd9:  rc = 64'h0000000000000088;
		5'd10: rc = 64'h0000000080008009; 5'd11: rc = 64'h000000008000000A;
		5'd12: rc = 64'h000000008000808B; 5'd13: rc = 64'h800000000000008B;
		5'd14: rc = 64'h8000000000008089; 5'd15: rc = 64'h8000000000008003;
		5'd16: rc = 64'h8000000000008002; 5'd17: rc = 64'h8000000000000080;
		5'd18: rc = 64'h000000000000800A; 5'd19: rc = 64'h800000008000000A;
		5'd20: rc = 64'h8000000080008081; 5'd21: rc = 64'h8000000000008080;
		5'd22: rc = 64'h0000000080000001; 5'd23: rc = 64'h8000000080008008;
		default: rc = 64'h0;
		endcase
	end

	reg [319:0] cpar;				// theta: the column parities C[x]
	reg [319:0] dcol;				// theta: D[x]
	reg [1599:0] bst;				// after theta, rho and pi
	integer x, y;

	always @(*) begin
		for (x = 0; x < 5; x = x + 1)
			cpar[64*x +: 64] = st[64*(x)      +: 64] ^ st[64*(x + 5)  +: 64] ^ st[64*(x + 10) +: 64] ^
			                   st[64*(x + 15) +: 64] ^ st[64*(x + 20) +: 64];
		for (x = 0; x < 5; x = x + 1)
			dcol[64*x +: 64] = cpar[64*((x + 4) % 5) +: 64] ^ rotl(cpar[64*((x + 1) % 5) +: 64], 1);
		// rho and pi: B[y, 2x + 3y] = rotl(A[x, y] ^ D[x], rho[x, y])
		for (x = 0; x < 5; x = x + 1)
			for (y = 0; y < 5; y = y + 1)
				bst[64*(y + 5*((2*x + 3*y) % 5)) +: 64] = rotl(st[64*(x + 5*y) +: 64] ^ dcol[64*x +: 64], rho(x + 5*y));
		// chi, then iota
		for (x = 0; x < 5; x = x + 1)
			for (y = 0; y < 5; y = y + 1)
				rnd_out[64*(x + 5*y) +: 64] = bst[64*(x + 5*y) +: 64] ^
					(~bst[64*((x + 1) % 5 + 5*y) +: 64] & bst[64*((x + 2) % 5 + 5*y) +: 64]);
		rnd_out[63:0] = rnd_out[63:0] ^ rc;
	end

	// -- the bus, and the rounds --

	wire do_in   = wr && !busy && (wa == 3'd4);
	wire do_xin  = wr && !busy && (wa == 3'd5);
	wire do_out  = rd && !busy && (wa == 3'd6);
	wire do_clr  = wr && !busy && (wa == 3'd1) && wb_dat_i[1];
	wire do_go   = wr && !busy && (wa == 3'd1) && wb_dat_i[0] && !wb_dat_i[1];

	always @(posedge clk) begin
		ack_r <= 1'b0;
		if (!resetn) begin
			busy <= 1'b0;
			rnd <= 5'd0;
			owner <= 31'd0;
			dat_r <= 32'd0;
		end else begin
			if (sel) begin
				ack_r <= 1'b1;
				dat_r <= 32'd0;
				if (wr && wa == 3'd3) begin
					if (wb_dat_i[31]) begin
						if (owner == wb_dat_i[30:0]) owner <= 31'd0;
					end else if (owner == 31'd0) begin
						owner <= wb_dat_i[30:0];
					end
				end
				if (rd) begin
					if (wa == 3'd0) dat_r <= 32'h5A4B4543;
					else if (wa == 3'd1) dat_r <= { 31'd0, busy };
					else if (wa == 3'd2) dat_r <= { 16'h4B45, 8'd0, 8'd1 };
					else if (wa == 3'd3) dat_r <= { 1'b0, owner };
					else if (do_out) dat_r <= st[31:0];
				end
			end
			if (do_go) begin
				busy <= 1'b1;
				rnd <= 5'd0;
			end else if (busy) begin
				if (rnd == 5'd23) busy <= 1'b0;
				rnd <= rnd + 5'd1;
			end
		end
	end

	// The state: its own always block, so that clearing it is a plain
	// synchronous reset of these flip-flops.
	always @(posedge clk) begin
		if (!resetn || do_clr)
			st <= 1600'd0;
		else if (busy)
			st <= rnd_out;
		else if (do_in)
			st <= { wb_dat_i, st[1599:32] };
		else if (do_xin)
			st <= { st[31:0] ^ wb_dat_i, st[1599:32] };
		else if (do_out)
			st <= { st[31:0], st[1599:32] };
	end

endmodule
