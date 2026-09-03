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
 * -- ANY RATE, ODD OR EVEN --
 *
 * A half-cell is rate/2 sys_clk, which is not an integer for an odd
 * rate. The divider below handles that by accumulating rather than
 * dividing, so exactly 128 half-cells land in every frame whatever the
 * rate. An odd rate costs half a cycle of edge jitter and nothing
 * else.
 *
 * That matters because the rate is a RUNTIME register that any app can
 * write, and the value every app actually asks for is 17. An earlier
 * version required an even rate, said so only in a comment, and
 * truncated when it got 17 -- producing a line 6.2% faster than the
 * samples feeding it.
 *
 * What is still worth respecting is the RECEIVER's range: IEC 60958
 * parts are typically specified for 32kHz and up, so the 22kHz and
 * 11kHz settings in sw/common/zaudio.h are below what a DAC will lock
 * to no matter how clean the line is. See z_audio_spdif_ok().
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

	/*
	 * The sample pair, STAGED at frame_req and TRANSFERRED at the
	 * S/PDIF frame boundary.
	 *
	 * -- why there are two registers and not one --
	 *
	 * frame_req comes from audio_out.v, which raises it at phase 62 of
	 * its own 64-slot counter. This module's frame is defined by its
	 * own half-cell counter, hc. Both run off the same clock at the
	 * same ratio so they cannot DRIFT -- but nothing aligns their
	 * PHASE, and whatever offset they come out of reset with persists
	 * forever.
	 *
	 * Latching the sample straight into hold_l/hold_r on frame_req
	 * therefore changes it at an arbitrary but FIXED point in the
	 * frame. If that point falls inside a subframe that is being
	 * transmitted, the slots already on the wire carry the old sample
	 * and the rest carry the new one -- and since the audio field is
	 * sent LSB first, it is the HIGH bits that get replaced. The
	 * parity bit, computed from the current value, then no longer
	 * matches what was sent.
	 *
	 * Every frame. Consistently. On one channel, because the phase
	 * does not move. That is audible as static behind the music, worst
	 * on sparse material where there is little to mask it, and it
	 * affects every app that uses this output -- it is not a property
	 * of what is being played.
	 *
	 * Staging fixes it: hold_l/hold_r change only at hc == 127, one
	 * half-cell before subframe A begins, so both are stable for the
	 * whole frame they are transmitted in. The cost is up to one frame
	 * of extra latency, which at 46875Hz is 21 microseconds.
	 *
	 * The analogue path does not have this problem and that is why it
	 * went unnoticed: audio_out.v feeds hold_l/hold_r to a
	 * sigma-delta, which simply tracks its input, so a change partway
	 * through a conversion is not a corruption. Only a SERIAL format
	 * cares when the value moves.
	 */
	reg signed [15:0] stage_l;
	reg signed [15:0] stage_r;

	reg [15:0] acc;
	reg [6:0] hc;          // half-cell within the frame, 0..127
	reg [7:0] blkframe;    // frame within the 192-frame block
	reg pre_inv;
	reg signed [15:0] hold_l;
	reg signed [15:0] hold_r;

	// FRACTIONAL half-cell divider.
	//
	// A frame is 64*rate sys_clk and carries exactly 128 half-cells,
	// so a half-cell is rate/2 cycles -- which is not an integer for
	// an odd rate. The first version did `rate >> 1` and TRUNCATED:
	// at rate 17 it produced 8-cycle half-cells, so the line ran a
	// 1024-cycle frame against the sample path's 1088. 46875 Hz of
	// S/PDIF fed from 44118 Hz of samples, 6.2% apart, with nothing
	// correcting it -- frames duplicated and dropped continuously, and
	// the output was audibly wrong.
	//
	// The docs said "RATE must be even" and nothing enforced it, while
	// the default rate every app asks for is 17. A constraint that is
	// only written down is not a constraint.
	//
	// So instead of dividing, this accumulates: add 128 every cycle
	// and emit a half-cell each time a frame period's worth has piled
	// up. That puts EXACTLY 128 half-cells in every frame at any rate,
	// odd or even, and makes drift structurally impossible rather than
	// merely unlikely.
	//
	// The cost for an odd rate is that half-cells alternate between 8
	// and 9 cycles: half a cycle, 10ns, on a 177ns cell. Receivers PLL
	// to the stream and tolerate far more than that. An even rate
	// divides exactly and has no jitter at all.
	wire [15:0] frame_cycles = { 2'b0, rate, 6'b0 };   // 64 * rate
	wire tick = (acc >= frame_cycles);

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

			acc <= 16'd0;
			hc <= 7'd0;
			blkframe <= 8'd0;
			pre_inv <= 1'b0;
			pre_cur <= 8'd0;
			spdif <= 1'b0;
			hold_l <= 16'sd0;
			hold_r <= 16'sd0;
			stage_l <= 16'sd0;
			stage_r <= 16'sd0;

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
			// frame_req only STAGES. See stage_l's comment: the two
			// counters share a ratio but not a phase, so this fires at
			// an arbitrary point in the S/PDIF frame and must not
			// touch anything currently on the wire.
			if (frame_req) begin
				stage_l <= enable ? sample_l : 16'sd0;
				stage_r <= enable ? sample_r : 16'sd0;
			end

			acc <= tick ? (acc - frame_cycles + 16'd128) : (acc + 16'd128);

			if (tick) begin

				hc <= hc + 7'd1;

				if (hc == 7'd127) begin
					// frame boundary: take the staged pair, advance
					// the block counter and start subframe A's
					// preamble. This is the ONLY place hold_l/hold_r
					// move, which is what makes them stable for the
					// whole of both subframes.
					hold_l <= stage_l;
					hold_r <= stage_r;
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

			end
		end

	end

endmodule
