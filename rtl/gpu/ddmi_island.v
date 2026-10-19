/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDMI audio: the data-island scheduler and packet engine -- what goes
 * on the three TMDS data channels between the pixels when DDMI audio
 * is on. See docs/ddmi_audio.md for the whole picture.
 *
 * Driven by gpu_video.v's own counters, so every character is placed
 * by position rather than by a state machine that could drift. A line
 * (hc counts from the front porch, as gpu_video.v does):
 *
 *     hc 0..11     control                          (12 characters)
 *        12..19    data-island preamble, CTL = 1010
 *        20..21    leading guard band
 *        22..85    two packets of 32 characters
 *        86..87    trailing guard band
 *        88..149   control                          (62: the extended
 *                                                   control period)
 *       150..157   video preamble, CTL = 1000       (picture lines)
 *       158..159   video guard band                 (picture lines)
 *       160..799   picture
 *
 * One island on every line, vertical blanking included. hsync (16..111)
 * is constant through the island, so a receiver that only understands
 * control tokens loses no sync edge.
 *
 * -- what is sent --
 *
 *   slot 0      audio samples, up to four frames, when SOUND is on and
 *               there are any; otherwise null
 *   slot 1      vc 10: general control (clear AV mute) -- within the
 *                      first lines of vsync, where a sink looks for it
 *               vc 11: AVI InfoFrame
 *               vc 12: SPD InfoFrame
 *               vc 13: Audio InfoFrame             (SOUND on)
 *               every 8th line (vc % 8 == 4) otherwise: audio clock
 *                      regeneration, N and CTS     (SOUND on)
 *               else:  null
 *
 * -- the samples --
 *
 * rtl/audio.v writes each frame it plays (after the system volume,
 * muted by EN, once or 2^REP times) into a 16-frame FIFO here, on its
 * own clock. Pointers cross as Gray code, two flops each way; the
 * array is LUT RAM read asynchronously on this side. Each line takes
 * whatever has arrived, up to four frames, at hc 12..15 -- at 48 kHz
 * and 31.5 kHz lines that is one or two, at 96 kHz three or four, so
 * the FIFO never holds more than a line or two and the latency is
 * about 50 us. With SOUND off the read pointer follows the write
 * pointer, so nothing stale is sent when it comes back on.
 *
 * Each frame carries IEC 60958 channel status, one bit per frame of a
 * 192-frame block, the same bits audio_spdif.v sends: consumer, PCM,
 * copy permitted (bit 2), and the rate code fs_code in bits 24..27.
 * The block start is the packet header's B flag for that frame;
 * V = U = 0; P is even parity over the 24 sample bits (the 16-bit
 * sample in the top two bytes) and V, U, C.
 *
 * The packets are constants (tools/ddmi/gen_packets.py makes them
 * from tools/ddmi/ddmi.py, the model the bench checks against). Their
 * BCH parity is NOT stored: it is computed as the bits go out, by an
 * 8-bit LFSR for the header and one per subpacket -- G(x) = 1 + x^6 +
 * x^7 + x^8, LSB first, mask 0x83. Header bit c leaves on channel 0
 * bit 2 of character c; subpacket k's bits 2c and 2c+1 leave on
 * channels 1 and 2, bit k, of character c. Parity follows the data:
 * characters 24..31 for the header, 28..31 (two bits each) for the
 * subpackets.
 *
 * Channel 0 bit 3 is 0 on the island's first packet character and 1
 * on every other one; bits 1:0 are vsync and hsync as they are on the
 * wire (active low).
 *
 * -- timing --
 *
 * Everything here is combinational from hc/vc, like gpu_video.v's
 * blank and sync, and lands in tmds_encoder.v's output register with
 * them. The parity registers advance on the clock edge that sends
 * character c, so character c+1 sees parity over 0..c: exactly what
 * a parity character needs.
 *
 * Registers start from their declared values (the FPGA's power-on
 * state); there is no reset input, as there is none in gpu_ddmi.v.
 * No local declarations; plain Verilog-2005.
 */

module ddmi_island (
	input pclk,
	input en,               // frame-adopted, from gpu_video.v
	input [10:0] hc,
	input [10:0] vc,
	input hsync,
	input vsync,

	// per channel: encoder MODE (tmds_encoder.v), TERC4 nibble, and
	// the control bits {C1, C0} (channels 1 and 2 only; channel 0's
	// are the syncs)
	output [1:0] mode0,
	output [3:0] t0,
	output [1:0] mode1,
	output [3:0] t1,
	output [1:0] ctl1,
	output [1:0] mode2,
	output [3:0] t2,
	output [1:0] ctl2,

	// the sound half: settings (this clock, frame-adopted upstream)
	input sound_en,
	input [3:0] fs_code,
	input [19:0] acr_n,
	input [19:0] acr_cts,
	// frames, { left[31:16], right[15:0] }, written on aud_clk
	input aud_clk,
	input aud_we,
	input [31:0] aud_wdata
);

	localparam [10:0] V_ACTIVE0 = 11'd45;

	// general control: clear AV mute
	localparam [247:0] PK_GCP = 248'h00000000000010000000000000100000000000001000000000000010000003;
	// AVI InfoFrame: VIC 1, RGB full range, 4:3, IT, underscan
	localparam [247:0] PK_AVI = 248'h000000000000000000000000000000000000000000000001881812bc0d0282;
	// SPD InfoFrame: Zeitlos, PC general
	localparam [247:0] PK_SPD = 248'h000009000000000000000000736f6c7469655a00736f6c7469655a86190183;
	// Audio InfoFrame: 2 channels, the rest from the stream
	localparam [247:0] PK_AIF = 248'h000000000000000000000000000000000000000000000000000001700a0184;

	// -- where are we --
	wire in_pre   = en && hc >= 11'd12 && hc < 11'd20;
	wire in_gb    = en && (hc == 11'd20 || hc == 11'd21 || hc == 11'd86 || hc == 11'd87);
	wire in_pkt   = en && hc >= 11'd22 && hc < 11'd86;
	wire vline    = en && vc >= V_ACTIVE0;
	wire in_vpre  = vline && hc >= 11'd150 && hc < 11'd158;
	wire in_vgb   = vline && (hc == 11'd158 || hc == 11'd159);

	wire [10:0] off = hc - 11'd22;
	wire slot = off[5];
	wire [4:0] c = off[4:0];

	wire snd = en && sound_en;

	// -- the FIFO: written on aud_clk, read here --
	reg [31:0] fmem [0:15];
	reg [4:0] wbin = 5'd0;
	reg [4:0] wgray = 5'd0;
	reg [4:0] rgray_w0 = 5'd0, rgray_w1 = 5'd0;   // rgray, into aud_clk
	reg [4:0] rbin = 5'd0;
	reg [4:0] rgray = 5'd0;
	reg [4:0] wgray_r0 = 5'd0, wgray_r1 = 5'd0;   // wgray, into pclk

	function [4:0] g2b;
		input [4:0] g;
		begin
			g2b[4] = g[4];
			g2b[3] = g2b[4] ^ g[3];
			g2b[2] = g2b[3] ^ g[2];
			g2b[1] = g2b[2] ^ g[1];
			g2b[0] = g2b[1] ^ g[0];
		end
	endfunction

	wire [4:0] rbin_w = g2b(rgray_w1);
	wire full = (wbin[4] != rbin_w[4]) && (wbin[3:0] == rbin_w[3:0]);
	wire [4:0] wbin_n = wbin + 5'd1;

	always @(posedge aud_clk) begin
		rgray_w0 <= rgray;
		rgray_w1 <= rgray_w0;
		if (aud_we && !full) begin
			fmem[wbin[3:0]] <= aud_wdata;
			wbin <= wbin_n;
			wgray <= wbin_n ^ (wbin_n >> 1);
		end
	end

	wire [4:0] wbin_r = g2b(wgray_r1);
	wire [4:0] avail = wbin_r - rbin;

	// -- this line's samples, taken at hc 11..15 --
	reg [2:0] nsamp = 3'd0;            // 0..4 frames in this line's packet
	reg [31:0] smp0 = 32'd0, smp1 = 32'd0, smp2 = 32'd0, smp3 = 32'd0;
	reg [3:0] sb_b = 4'd0;             // B (block start) per frame
	reg [3:0] sb_c = 4'd0;             // channel-status bit per frame
	reg [7:0] cs_idx = 8'd0;           // frame within the 192-frame block

	wire [2:0] take = (avail > 5'd4) ? 3'd4 : avail[2:0];
	wire [1:0] pop_i = hc[1:0];        // hc 12..15 -> 0..3
	wire popping = snd && hc >= 11'd12 && hc < 11'd16 && { 1'b0, pop_i } < nsamp;
	wire cs_bit = (cs_idx == 8'd2) ? 1'b1 :
		(cs_idx >= 8'd24 && cs_idx <= 8'd27) ? fs_code[cs_idx[1:0]] : 1'b0;
	wire [31:0] fword = fmem[rbin[3:0]];
	wire [4:0] rbin_n = rbin + 5'd1;

	always @(posedge pclk) begin
		wgray_r0 <= wgray;
		wgray_r1 <= wgray_r0;
		if (!snd) begin
			// nothing is being sent: drop what arrives
			rbin <= wbin_r;
			rgray <= wbin_r ^ (wbin_r >> 1);
			nsamp <= 3'd0;
		end else begin
			if (hc == 11'd11)
				nsamp <= take;
			if (popping) begin
				case (pop_i)
					2'd0: smp0 <= fword;
					2'd1: smp1 <= fword;
					2'd2: smp2 <= fword;
					default: smp3 <= fword;
				endcase
				sb_b[pop_i] <= (cs_idx == 8'd0);
				sb_c[pop_i] <= cs_bit;
				cs_idx <= (cs_idx == 8'd191) ? 8'd0 : cs_idx + 8'd1;
				rbin <= rbin_n;
				rgray <= rbin_n ^ (rbin_n >> 1);
			end
		end
	end

	// one audio subpacket: SB0..SB6 for a frame, or zeros if absent
	function [55:0] asub;
		input [31:0] f;
		input present;
		input c;
		reg [15:0] l;
		reg [15:0] r;
		begin
			l = f[31:16];
			r = f[15:0];
			asub = present ? { (^r) ^ c, c, 1'b0, 1'b0, (^l) ^ c, c, 1'b0, 1'b0,
				r[15:8], r[7:0], 8'd0, l[15:8], l[7:0], 8'd0 } : 56'd0;
		end
	endfunction

	wire [3:0] present = (nsamp == 3'd0) ? 4'b0000 : (nsamp == 3'd1) ? 4'b0001 :
		(nsamp == 3'd2) ? 4'b0011 : (nsamp == 3'd3) ? 4'b0111 : 4'b1111;

	wire [247:0] pk_aud = {
		asub(smp3, present[3], sb_c[3]),
		asub(smp2, present[2], sb_c[2]),
		asub(smp1, present[1], sb_c[1]),
		asub(smp0, present[0], sb_c[0]),
		{ sb_b & present, 4'b0000 },        // HB2: B per subpacket
		{ 4'b0000, present },               // HB1: layout 0, sample present
		8'h02                               // HB0: audio sample packet
	};

	// clock regeneration: CTS then N, big-endian, in all four subpackets
	wire [55:0] acr_sb = { acr_n[7:0], acr_n[15:8], { 4'b0, acr_n[19:16] },
		acr_cts[7:0], acr_cts[15:8], { 4'b0, acr_cts[19:16] }, 8'd0 };

	// -- which packet, and this character's bits of it --
	//
	// The nine data bits a character carries: header bit c, and bits 2c
	// and 2c+1 of each subpacket. Each source picks its own nine first
	// and only then is one source chosen -- choosing a whole 248-bit
	// packet first and indexing it afterwards is the same function,
	// but it builds a 248-bit-wide multiplexer in front of the index
	// and costs several hundred LUTs more. Indices past the data are
	// don't-care (parity is sent there); kept in range.
	wire [4:0] ch = (c < 5'd24) ? c : 5'd0;
	wire [5:0] c2 = (c < 5'd28) ? { c, 1'b0 } : 6'd0;

	// { s3b, s3a, s2b, s2a, s1b, s1a, s0b, s0a, hd }
	function [8:0] pick;
		input [247:0] v;
		input [4:0] hi;
		input [5:0] si;
		begin
			pick = { v[8'd193 + si], v[8'd192 + si], v[8'd137 + si], v[8'd136 + si],
				v[8'd81 + si], v[8'd80 + si], v[8'd25 + si], v[8'd24 + si], v[hi] };
		end
	endfunction

	// clock regeneration has the same seven bytes in every subpacket,
	// so pick them once
	wire [7:0] acr_hb = { 7'd0, 1'b1 };
	wire acr_hd = (ch < 5'd8) ? acr_hb[ch[2:0]] : 1'b0;
	wire acr_a = acr_sb[c2];
	wire acr_b = acr_sb[c2 + 6'd1];

	wire [2:0] kind =
		!slot ? ((nsamp != 3'd0) ? 3'd1 : 3'd0) :
		(vc == 11'd10) ? 3'd2 :
		(vc == 11'd11) ? 3'd3 :
		(vc == 11'd12) ? 3'd4 :
		(snd && vc == 11'd13) ? 3'd5 :
		(snd && vc[2:0] == 3'd4) ? 3'd6 : 3'd0;

	wire [8:0] bits =
		(kind == 3'd1) ? pick(pk_aud, ch, c2) :
		(kind == 3'd2) ? pick(PK_GCP, ch, c2) :
		(kind == 3'd3) ? pick(PK_AVI, ch, c2) :
		(kind == 3'd4) ? pick(PK_SPD, ch, c2) :
		(kind == 3'd5) ? pick(PK_AIF, ch, c2) :
		(kind == 3'd6) ? { acr_b, acr_a, acr_b, acr_a, acr_b, acr_a, acr_b, acr_a, acr_hd } :
		9'd0;

	wire hd  = bits[0];
	wire s0a = bits[1];
	wire s0b = bits[2];
	wire s1a = bits[3];
	wire s1b = bits[4];
	wire s2a = bits[5];
	wire s2b = bits[6];
	wire s3a = bits[7];
	wire s3b = bits[8];

	// -- parity --
	function [7:0] ecc1;
		input [7:0] p;
		input b;
		begin
			ecc1 = (p >> 1) ^ ((p[0] ^ b) ? 8'h83 : 8'h00);
		end
	endfunction

	reg [7:0] ph = 8'd0, p0 = 8'd0, p1 = 8'd0, p2 = 8'd0, p3 = 8'd0;

	wire hdata = (c < 5'd24);
	wire sdata = (c < 5'd28);

	wire hb  = hdata ? hd  : ph[0];
	wire b0a = sdata ? s0a : p0[0];
	wire b0b = sdata ? s0b : p0[1];
	wire b1a = sdata ? s1a : p1[0];
	wire b1b = sdata ? s1b : p1[1];
	wire b2a = sdata ? s2a : p2[0];
	wire b2b = sdata ? s2b : p2[1];
	wire b3a = sdata ? s3a : p3[0];
	wire b3b = sdata ? s3b : p3[1];

	always @(posedge pclk) begin
		if (!in_pkt || c == 5'd31) begin
			ph <= 8'd0; p0 <= 8'd0; p1 <= 8'd0; p2 <= 8'd0; p3 <= 8'd0;
		end else begin
			ph <= hdata ? ecc1(ph, hd) : (ph >> 1);
			p0 <= sdata ? ecc1(ecc1(p0, s0a), s0b) : (p0 >> 2);
			p1 <= sdata ? ecc1(ecc1(p1, s1a), s1b) : (p1 >> 2);
			p2 <= sdata ? ecc1(ecc1(p2, s2a), s2b) : (p2 >> 2);
			p3 <= sdata ? ecc1(ecc1(p3, s3a), s3b) : (p3 >> 2);
		end
	end

	// -- the characters --
	wire first = (hc == 11'd22);

	assign mode0 = (in_gb || in_pkt) ? 2'd1 : in_vgb ? 2'd2 : 2'd0;
	assign t0 = in_gb ? { 2'b11, vsync, hsync } :
		{ !first, hb, vsync, hsync };

	assign mode1 = in_pkt ? 2'd1 : in_gb ? 2'd3 : in_vgb ? 2'd2 : 2'd0;
	assign t1 = { b3a, b2a, b1a, b0a };
	assign ctl1 = (in_pre || in_vpre) ? 2'b01 : 2'b00;     // CTL0 = 1

	assign mode2 = in_pkt ? 2'd1 : in_gb ? 2'd3 : in_vgb ? 2'd2 : 2'd0;
	assign t2 = { b3b, b2b, b1b, b0b };
	assign ctl2 = in_pre ? 2'b01 : 2'b00;                   // CTL2 = 1

endmodule
