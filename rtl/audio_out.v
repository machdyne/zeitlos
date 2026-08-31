/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Audio output stage -- turns a stream of 16-bit stereo frames into
 * whatever the board's DAC actually wants.
 *
 * Two formats, ONE timing source. See rtl/audio.v for the FIFO and
 * register interface that feeds this, and docs/audio.md for the whole
 * picture.
 *
 *   PT8211/TM8211  (Mozart)         -- BCK/WS/DIN, 16-bit LSB-justified
 *   1-bit sigma-delta (Obst, Lakritz) -- two independent modulators
 *
 * -- why both are always instantiated --
 *
 * There is no `ifdef or generate block choosing between them here.
 * rtl/sysctl.v connects only the ports its board has pins for, and
 * yosys prunes everything that reaches no output. A board with only a
 * 1-bit DAC leaves pt_bck/pt_ws/pt_din unconnected and the shift
 * register disappears; a board with only a PT8211 leaves sd_l/sd_r
 * unconnected and both modulators disappear. Measured, not assumed --
 * see docs/audio.md's resource table, which reports each board
 * separately for exactly this reason.
 *
 * What does NOT disappear either way is the divider below, because
 * both formats need the same frame tick. That is the point of sharing
 * it: one sample rate, one place to get it wrong.
 *
 * -- clocking: counters, not a PLL --
 *
 * Both EHXPLLL blocks are used on every board in this lineup
 * (rtl/sysctl.v's pll0/pll1), so there is no third PLL to have and
 * this divides sys_clk instead.
 *
 * `rate` is the half-period of BCK in sys_clk cycles, so
 *
 *     f_bck = CLK_HZ / (2 * rate)          f_s = f_bck / 32
 *     f_s   = CLK_HZ / (64 * rate)
 *
 * At 48MHz, rate=17 gives 44117.6 Hz -- 0.04% high, a 0.7-cent pitch
 * error, inaudible. rate=34 gives 22058.8 Hz. It is a register rather
 * than a parameter (rtl/audio.v's RATE) because a MOD player and a
 * sample player want different answers and neither wants a rebuild.
 *
 * `rate` is clamped to 2 below rather than trusted. A zero written
 * there would make bck_tick true every cycle and free-run the
 * serialiser at 24MHz -- not a hang, but not something worth being
 * able to do to yourself from userspace either.
 *
 * -- PT8211 frame timing --
 *
 * phase[5:0] counts 64 half-BCK slots per frame: 32 BCK periods, 16
 * per channel. BCK is phase[0] and WS is phase[5], so both come free
 * out of the same counter.
 *
 * The DAC samples DIN on the RISING edge of BCK, so DIN is updated on
 * the falling edge -- half a BCK period (rate sys_clk cycles, 354ns at
 * the default rate) of setup. LSB-justified with exactly 16 bits per
 * half-frame means the word starts immediately after the WS edge, so
 * MSB-first from the WS transition is the whole rule.
 *
 * WHICH HALF IS LEFT is a `swap_lr` input, not a constant. The part is
 * documented both ways in different places and the boards this runs on
 * were not built to test it; a channel swap is a one-word register
 * write to fix (rtl/audio.v's CTRL bit 2) rather than a rebuild. If
 * you scope it and find the default wrong, change RATE_RESET's
 * neighbour CTRL_RESET in rtl/audio.v so it comes up right instead of
 * making every app fix it.
 *
 * -- the frame handoff, and why frame_req is a cycle early --
 *
 * frame_req pulses at phase 62, not at the phase 63->0 wrap, and this
 * ordering is load-bearing. hold_l/hold_r latch the FIFO's current
 * output on the same edge that raises frame_req; the FIFO sees
 * frame_req a cycle later and only then advances its read pointer; the
 * shift register loads from hold at the wrap. Generating the pulse at
 * the wrap instead would have the shift register load the OLD hold
 * value on the very edge the new one arrives -- a whole frame of
 * delay, which is harmless to listen to and miserable to explain in a
 * waveform six months later.
 */

module audio_out #(
	// Sigma-delta feedback level, i.e. what the 1-bit output is worth
	// when it is high. Deliberately LARGER than a full-scale sample
	// (32768), which is what gives the loop headroom: a 2nd-order
	// modulator driven to its own full scale is only conditionally
	// stable, and the classic symptom is an input near 0dBFS sending
	// the integrators into a limit cycle that sounds like tearing.
	//
	// 49152 is 1.5x full scale. The cost is output amplitude -- a
	// full-scale sample produces 83% duty rather than 100% -- and
	// about 3.5dB of SNR. Worth it. Raise it if you can drive the
	// input harder than this codebase does; do not lower it without
	// running tb_audio's stability check, which exists for this.
	parameter integer SD_FS = 49152
)
(
	input wire clk,
	input wire rst,

	// 0 mutes: hold_l/hold_r are forced to zero at the next frame,
	// which the modulators turn into a steady 50% bitstream and the
	// PT8211 into a zero word. NOT a clock gate -- BCK and WS keep
	// running, because a PT8211 with a stopped bit clock is a part in
	// an undefined state rather than a quiet one.
	input wire enable,

	// half-BCK period in clk cycles; see this file's header
	input wire [7:0] rate,

	// swap the two channels in the output stage -- see header
	input wire swap_lr,

	input wire signed [15:0] sample_l,
	input wire signed [15:0] sample_r,

	// 0 means the FIFO had nothing to give, so hold the frame already
	// playing rather than latching whatever sample_l/sample_r happen
	// to be showing -- see rtl/audio.v's header on why a repeated
	// sample is the right noise to make here.
	input wire sample_valid,

	// one-cycle pulse, one frame period apart. rtl/audio.v pops its
	// FIFO on this.
	output reg frame_req,

	// 1-bit DACs (Obst, Lakritz)
	output wire sd_l,
	output wire sd_r,

	// PT8211/TM8211 (Mozart)
	output wire pt_bck,
	output wire pt_ws,
	output wire pt_din
);

	reg [7:0] div_ctr;
	reg [5:0] phase;
	reg signed [15:0] hold_l;
	reg signed [15:0] hold_r;
	reg [15:0] sr;

	// Clamped, not trusted -- see this file's header.
	wire [7:0] rate_eff = (rate < 8'd2) ? 8'd2 : rate;
	wire bck_tick = (div_ctr == 8'd0);

	// word_a is sent while WS is low, word_b while WS is high
	wire [15:0] word_a = swap_lr ? hold_r : hold_l;
	wire [15:0] word_b = swap_lr ? hold_l : hold_r;

	assign pt_bck = phase[0];
	assign pt_ws = phase[5];
	assign pt_din = sr[15];

	always @(posedge clk) begin

		if (rst) begin

			div_ctr <= 8'd0;
			phase <= 6'd0;
			hold_l <= 16'sd0;
			hold_r <= 16'sd0;
			sr <= 16'd0;
			frame_req <= 1'b0;

		end else begin

			frame_req <= 1'b0;

			if (bck_tick) begin

				div_ctr <= rate_eff - 8'd1;
				phase <= phase + 6'd1;

				// One slot BEFORE the frame wrap -- see header.
				if (phase == 6'd62) begin
					frame_req <= 1'b1;
					if (!enable) begin
						hold_l <= 16'sd0;
						hold_r <= 16'sd0;
					end else if (sample_valid) begin
						hold_l <= sample_l;
						hold_r <= sample_r;
					end
				end

				// phase[0] set means the NEXT slot is even, i.e. this
				// is BCK's falling edge -- the only place DIN moves.
				if (phase[0]) begin
					if (phase[4:0] == 5'd31)
						// entering a new half-frame. phase[5] is still
						// the OLD half, so its complement picks the new
						// one: 31 -> 32 starts WS-high, 63 -> 0 starts
						// WS-low.
						sr <= phase[5] ? word_a : word_b;
					else
						sr <= { sr[14:0], 1'b0 };
				end

			end else begin

				div_ctr <= div_ctr - 8'd1;

			end

		end

	end

	// The two 1-bit DACs. hold_* is already muted by `enable` above,
	// so these need no enable of their own -- a zero input is exactly
	// the 50% bitstream that means silence.
	audio_sd #(.SD_FS(SD_FS)) sd_l_i (
		.clk(clk),
		.rst(rst),
		.sample(hold_l),
		.out(sd_l)
	);

	audio_sd #(.SD_FS(SD_FS)) sd_r_i (
		.clk(clk),
		.rst(rst),
		.sample(hold_r),
		.out(sd_r)
	);

endmodule

/*
 * Second-order error-feedback sigma-delta modulator.
 *
 * Runs at sys_clk, so the oversampling ratio is the whole frame
 * period: 48MHz / 44.1kHz = 1088. The noise transfer function is
 * (1 - z^-1)^2, which at that OSR pushes quantisation noise far enough
 * above the audio band that the RC filter on the board can deal with
 * what is left.
 *
 *     v  = x + 2*e1 - e2
 *     q  = (v >= 0) ? +SD_FS : -SD_FS
 *     e1 = clamp(v - q)
 *     e2 = e1(previous)
 *
 * -- the clamp is not paranoia --
 *
 * A 2nd-order loop is conditionally stable. Without the limit, an
 * input sustained near the feedback level walks the error terms out to
 * the width of the accumulator and the output degenerates into a low
 * frequency limit cycle -- which is audible as a rasp, not as silence,
 * so it does not announce itself as a failure. SD_FS's own headroom
 * (see above) is the first defence and this is the second. Clamping
 * distorts the NTF while it is engaged; that is strictly better than
 * the alternative and it should never engage on real material.
 *
 * ELIM is 4*SD_FS. Accumulator width follows from that: 2*e1 reaches
 * 8*SD_FS, so v reaches 32768 + 8*SD_FS + 4*SD_FS = ~622k, which needs
 * 21 bits signed. 24 is what is used, because the spare three bits
 * cost nothing in LUT4s at this width and mean a future change to
 * SD_FS or ELIM cannot silently wrap.
 *
 * -- reset --
 *
 * q resets to 0, which is momentarily a full-negative output. It is
 * one sys_clk cycle before the loop starts dithering around mid-scale,
 * i.e. 21ns, and the reset released long before any board's output
 * coupling has settled. Not worth logic to avoid.
 */
module audio_sd #(
	parameter integer SD_FS = 49152
)
(
	input wire clk,
	input wire rst,
	input wire signed [15:0] sample,
	output wire out
);

	localparam signed [23:0] FS_POS = SD_FS;
	localparam signed [23:0] FS_NEG = -SD_FS;
	localparam signed [23:0] ELIM_POS = 4 * SD_FS;
	localparam signed [23:0] ELIM_NEG = -4 * SD_FS;

	reg signed [23:0] e1;
	reg signed [23:0] e2;
	reg q;

	wire signed [23:0] x = { {8{sample[15]}}, sample };
	wire signed [23:0] v = x + (e1 <<< 1) - e2;
	wire v_pos = ~v[23];
	wire signed [23:0] e_raw = v - (v_pos ? FS_POS : FS_NEG);
	wire signed [23:0] e_clamped =
		(e_raw > ELIM_POS) ? ELIM_POS :
		(e_raw < ELIM_NEG) ? ELIM_NEG :
		e_raw;

	assign out = q;

	always @(posedge clk) begin
		if (rst) begin
			e1 <= 24'sd0;
			e2 <= 24'sd0;
			q <= 1'b0;
		end else begin
			q <= v_pos;
			e1 <= e_clamped;
			e2 <= e1;
		end
	end

endmodule
