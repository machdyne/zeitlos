/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * S/PDIF transmitter -- IEC 60958 consumer format, biphase-mark coded.
 *
 * Drives an optical TOSLINK module (Sergei's AUD_OPTICAL) or a coax
 * driver. One pin, no clock, no PLL.
 *
 * -- THE RATE, WHICH IS THE WHOLE DESIGN CONSTRAINT --
 *
 * A stereo frame is 64 time slots, each biphase-mark coded into two
 * half-cells, so the line runs at 128 * fs. From a 48MHz sys_clk with
 * both EHXPLLLs already spoken for, the reachable rates are the ones
 * where 48e6 / (128 * fs) is an integer -- fs = 375000/N:
 *
 *     N=7   53571.4 Hz    N=9   41666.7 Hz
 *     N=8   46875.0 Hz    N=10  37500.0 Hz
 *
 * No standard rate is among them. 44.1kHz wants N = 8.5034 and 48kHz
 * wants 7.8125.
 *
 * SO THIS RUNS AT 46875 Hz, N=8, half-cell = 8 sys_clk exactly.
 *
 * That is 2.34% below 48kHz and 6.29% above 44.1kHz -- much closer to
 * 48k than the arithmetic first suggests, and well inside the capture
 * range of any receiver that recovers its clock from the biphase
 * stream, which is all of them. The declared rate in channel status is
 * a separate field (fs_code) and advisory; receivers PLL to what
 * actually arrives.
 *
 * -- WHY THERE IS NO UPSAMPLER --
 *
 * The obvious question is whether to run the mixer at 44.1kHz and
 * resample into a 46875Hz S/PDIF stream. The answer is no, for two
 * reasons.
 *
 * A tracker does not have a native rate. MOD playback resamples every
 * channel through a phase accumulator anyway, so the output rate is a
 * free parameter and 46875 costs exactly nothing -- pitch and tempo
 * are unaffected. Set `AUDIO_RATE_RESET to 16 on a board with a
 * transmitter and the whole audio block, analogue and digital, runs at
 * one rate.
 *
 * And a 1.0629 ratio is a real sample-rate conversion, not an
 * upsample. Zero-order hold at that ratio produces aliasing across the
 * whole band -- it would be plainly audible, and worse than the 6%
 * pitch error it was meant to avoid. Doing it properly means an
 * interpolating resampler, which is more logic than the mixer itself.
 *
 * The one case that would want it is playing 44.1kHz material
 * unresampled while ALSO feeding a digital output. That is not what
 * this machine does.
 *
 * -- RATE MUST BE EVEN --
 *
 * A half-cell is rate/2 sys_clk, so an odd `rate` cannot be divided
 * evenly and the frame would need a one-cycle correction each time it
 * resyncs. 16 is the intended value; 17 (44117.6Hz, the analogue
 * default) is odd and will produce a slightly ragged line. The
 * transmitter still runs -- receivers tolerate far worse -- but a
 * board with S/PDIF should come up at 16.
 *
 * -- FRAME FORMAT --
 *
 * 192 frames per block, 2 subframes per frame, 32 time slots each:
 *
 *   slots 0-3    preamble, 8 half-cells, NOT biphase coded (the
 *                violation is what makes it findable in the stream)
 *   slots 4-7    aux, zero here
 *   slots 8-27   audio, 20 bits LSB first. A 16-bit sample sits in the
 *                TOP 16 (slots 12-27) with slots 8-11 zero, which is
 *                what makes a 16-bit source play at the right level on
 *                a 20/24-bit receiver rather than 16x too quiet.
 *   slot 28      V, validity -- 0 means valid
 *   slot 29      U, user data
 *   slot 30      C, one bit of the 192-bit channel status word
 *   slot 31      P, even parity over slots 4-31
 *
 * Preambles are emitted as absolute line states, inverted when the
 * previous half-cell left the line high, so that the pattern's
 * transitions land where the receiver expects regardless of what came
 * before.
 */

module audio_spdif (
	input wire clk,
	input wire rst,

	// 0 transmits silence -- zero samples in otherwise valid frames.
	// NOT a transmitter disable: a receiver that loses the stream
	// unlocks and re-locks with an audible click, so silence has to
	// stay a transmitted signal.
	input wire enable,

	// same divider the rest of the audio block uses; a half-cell is
	// rate/2 sys_clk. See the note above on why this wants to be even.
	input wire [7:0] rate,

	// one pulse per sample period, from rtl/audio_out.v. The S/PDIF
	// frame is resynced to it, so the two outputs cannot drift.
	input wire frame_req,

	input wire signed [15:0] sample_l,
	input wire signed [15:0] sample_r,

	// channel status sampling-frequency nibble, byte 3 bits 0-3.
	// Advisory: receivers recover the real rate from the line. 0000 is
	// 44.1kHz, 0100 is 48kHz.
	input wire [3:0] fs_code,

	output reg spdif
);

	reg [7:0] div;
	reg [6:0] hc;          // half-cell within the frame, 0..127
	reg [7:0] blkframe;    // frame within the 192-frame block
	reg pre_inv;
	reg signed [15:0] hold_l;
	reg signed [15:0] hold_r;

	wire [7:0] hc_div = (rate < 8'd4) ? 8'd2 : (rate >> 1);
	wire tick = (div == 8'd0);

	wire sub = hc[6];          // 0 = subframe A (left), 1 = B (right)
	wire [5:0] pos = hc[5:0];  // half-cell within the subframe

	// Preamble patterns, MSB first in time, as line states following a
	// LOW half-cell. Z marks the start of a block, X a left subframe,
	// Y a right one.
	//
	// Selected for the subframe ABOUT TO START, not the one ending --
	// and at a frame boundary blkframe is being incremented on the
	// same edge, so the block-start test has to use the incremented
	// value. Reading either of them one subframe late puts an X where
	// a Z belongs and the receiver never finds the start of a block.
	wire [7:0] blk_next = (blkframe == 8'd191) ? 8'd0 : blkframe + 8'd1;

	wire [7:0] pre_frame = (blk_next == 8'd0) ? 8'b11101000    // Z
	                                          : 8'b11100010;   // X
	localparam [7:0] PRE_B = 8'b11100100;                      // Y

	wire signed [15:0] samp = sub ? hold_r : hold_l;

	// 20-bit audio field, LSB first, sample left-justified into it.
	wire [19:0] audio = { samp, 4'b0000 };

	// Channel status, consumer format. Everything not named here is
	// zero: bit 0 clear says consumer, bit 1 clear says linear PCM,
	// category 0 is "general". Only two fields are worth setting.
	wire cs_bit =
		(blkframe == 8'd2) ? 1'b1 :                        // copy permitted
		(blkframe >= 8'd24 && blkframe <= 8'd27)
			? fs_code[blkframe[1:0]] : 1'b0;               // sampling freq

	// Slots 4..30, then parity over them in slot 31.
	wire [26:0] slots_4_30 = { cs_bit, 1'b0, 1'b0, audio, 4'b0000 };
	wire [27:0] sub_bits = { ^slots_4_30, slots_4_30 };

	// The half-cell being emitted is hc+1, so at pos p the data index
	// is p+1 and the bit within the subframe is (p+1-8)>>1.
	wire [4:0] bit_idx = (pos - 6'd7) >> 1;   // pos 7..62 -> bit 0..27
	wire data_bit = sub_bits[bit_idx];

	// The preamble in flight, LATCHED at the transition that starts it.
	//
	// It used to be recomputed from blk_next for half-cells 1..7, and
	// blkframe advances on the very edge that emits half-cell 0 -- so
	// the first half-cell came from one frame's pattern and the other
	// seven from the next frame's. The Z that marks a block start was
	// emitted with an X's tail, and a receiver saw an X. Latching is
	// the whole fix.
	reg [7:0] pre_cur;

	always @(posedge clk) begin

		if (rst) begin

			div <= 8'd0;
			hc <= 7'd0;
			blkframe <= 8'd0;
			pre_inv <= 1'b0;
			pre_cur <= 8'd0;
			spdif <= 1'b0;
			hold_l <= 16'sd0;
			hold_r <= 16'sd0;

		end else begin

			// ONE OWNER PER COUNTER.
			//
			// frame_req latches the sample pair and nothing else. The
			// half-cell counter owns the frame: it wraps every 128
			// half-cells, which is 128 * rate/2 = 64 * rate cycles,
			// exactly the audio frame period -- both dividers come off
			// the same clock with the same ratio, so once started they
			// cannot drift.
			//
			// Both used to advance blkframe, because frame_req and the
			// hc wrap land within a cycle or two of each other and
			// BOTH fired every frame. The block counter went up by two
			// per frame, so it never hit 0, the Z preamble was never
			// emitted, and every channel-status bit read back zero. A
			// receiver would have decoded the audio perfectly and
			// never found the start of a block.
			if (frame_req) begin
				hold_l <= enable ? sample_l : 16'sd0;
				hold_r <= enable ? sample_r : 16'sd0;
			end

			if (tick) begin

				div <= hc_div - 8'd1;
				hc <= hc + 7'd1;

				if (hc == 7'd127) begin
					// frame boundary: advance the block counter and
					// start subframe A's preamble
					blkframe <= blk_next;
					pre_inv <= spdif;
					pre_cur <= pre_frame;
					spdif <= pre_frame[7] ^ spdif;
				end else if (pos == 6'd63) begin
					// entering subframe B's preamble
					pre_inv <= spdif;
					pre_cur <= PRE_B;
					spdif <= PRE_B[7] ^ spdif;
				end else if (pos < 6'd7) begin
					// preamble half-cells 1..7, absolute states
					spdif <= pre_cur[6 - pos[2:0]] ^ pre_inv;
				end else if (pos[0]) begin
					// hc+1 is even: first half of a bit cell, which
					// always transitions
					spdif <= ~spdif;
				end else begin
					// hc+1 is odd: second half, transitions only for a 1
					spdif <= data_bit ? ~spdif : spdif;
				end

			end else begin

				div <= div - 8'd1;

			end
		end

	end

endmodule
