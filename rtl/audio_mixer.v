/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Audio mixer -- eight channels of sample playback, in hardware.
 *
 * PHASE 3 of the audio subsystem. Phase 1 gave the DAC a FIFO; phase 2
 * proved the whole path with a software MOD player (sw/apps/mod) and
 * measured what software mixing actually costs on this machine. This
 * block exists because that measurement came back badly.
 *
 * -- THE NUMBER THIS BLOCK IS ANSWERING --
 *
 * On Obst, four channels at 22kHz mixed in software cost about 40% of
 * a whole 48MHz picorv32 -- the core is not pipelined and has no
 * instruction cache (nor should it: 10ns SRAM at 48MHz is already
 * zero wait state, so a cache would add latency rather than remove
 * it). The kernel schedules round-robin on the 732Hz KTIMER, and with
 * wm and sh also runnable the player gets a third of the CPU. A third
 * of the CPU against a 40% job means the music plays at 83% speed:
 * measured on hardware, reported by sw/apps/mod's own `mix` readout.
 *
 * No FIFO size fixes that, and no amount of tuning the inner loop
 * closes a 3x gap. The whole point of the design is this:
 *
 *   AUDIO MUST NOT CARE HOW MANY PROCESSES ARE RUNNING.
 *
 * Once this block owns playback, the CPU's entire remaining job is to
 * write channel registers when a tracker row changes -- about 50 times
 * a second, a few dozen stores. Everything that used to scale with the
 * sample rate now happens here, on a clock that does not get
 * preempted.
 *
 * -- ONE TIME-MULTIPLEXED ENGINE, NOT EIGHT CHANNELS OF LOGIC --
 *
 * At 44.1kHz there are 1088 sys_clk cycles per sample period. Eight
 * channels at roughly 10 cycles each is 80 cycles, about 7% utilised.
 * A sequencer walking eight sets of state registers is where the
 * cheapness comes from; eight parallel channel units would cost eight
 * times the logic to idle for 93% of every frame.
 *
 * The sequence per channel is: read the state, fetch the sample byte
 * from main memory, scale it by the two gains, accumulate, advance the
 * phase accumulator, handle the loop, write the phase back.
 *
 * -- SAMPLES LIVE IN MAIN MEMORY, SO THIS IS A BUS MASTER --
 *
 * A MOD's sample bank is 100KB and up. There is no BRAM for that on
 * any board here, so this block reads sample bytes over the main
 * wishbone bus as the third master on rtl/arbiter_main.v.
 *
 * Bandwidth is not the concern: eight channels at 44.1kHz is 353k
 * single-word reads a second against a 48MHz bus, under 1% of it. See
 * arbiter_main.v's own header for why this master sits IN the
 * round-robin rather than below it -- a master this light perturbs
 * nobody, and a lowest-priority scheme would invent a starvation
 * corner that appears exactly when a game is rendering hard, which is
 * when the audio must not break.
 *
 * The read is a full 32-bit word and the byte is selected from it.
 * Sample data is 8-bit signed, which is what every tracker format in
 * this family uses; there is no 16-bit path and adding one would
 * double this fetch for nothing anybody here needs.
 *
 * -- PHASE ACCUMULATOR --
 *
 * 32 bits with 14 fractional bits, the same split sw/apps/mod's
 * software engine uses and for the same reasons: a ProTracker sample
 * can be 131070 bytes so the integer part needs 18 bits, and
 * 131070 * 2^14 still fits a 32-bit register with headroom. 1/16384 of
 * a sample is well under a cent of pitch error.
 *
 * -- STATE STORAGE --
 *
 * The six configuration words per channel are written only by the CPU
 * and read only by the sequencer -- one writer, one reader -- which is
 * exactly the shape distributed RAM wants, so they cost LUT-RAM rather
 * than block RAM (the scarce thing) or flip-flops. `pos` and `active`
 * are written by BOTH the sequencer and a CPU trigger, so those are
 * ordinary registers; two write ports into LUT-RAM is not a thing.
 *
 * -- ONE OPERATION PER STATE, AND ONE SHARED MULTIPLIER --
 *
 * The sequencer is deliberately longer than it needs to be. The first
 * version did the obvious thing and packed each channel into four
 * states, which put chains like this in a single cycle:
 *
 *   LUT-RAM read -> 32-bit add -> shift -> compare against another
 *   32-bit add -> mux -> 32-bit subtract -> register
 *
 * That is four carry chains deep and it made nextpnr's placer work
 * very hard on a design that otherwise fits comfortably. There is no
 * reason to accept it: eight channels at ~11 cycles each is 88 of the
 * 1088 sys_clk in a 44.1kHz frame, so cycles are the one resource this
 * block has in abundance. Every state below now does ONE 32-bit
 * operation from registered inputs.
 *
 * The same argument applies to the multiplier. Computing both gains
 * and both output scales as separate combinational expressions asked
 * for four multipliers, which yosys spread across seven MULT18X18D.
 * There are eighteen multiplies to do per frame and a thousand cycles
 * to do them in, so one shared multiplier fed by the sequencer is
 * strictly better: fewer DSPs, fewer long paths, no downside anyone
 * can hear.
 *
 * -- OUTPUT SCALING --
 *
 * Eight channels of (8-bit sample x 8-bit gain) sum to +/-261120, 19
 * bits signed. `mixvol` scales that down:
 *
 *     out = clamp16((acc * mixvol) >> 10)
 *
 * mixvol 255 puts four channels at full gain near full scale, which is
 * what a 4-channel MOD wants; 128 is the reset value and is safe for
 * all eight. Software owns the choice because software knows how many
 * channels the module has. The clamp is still there -- "cannot clip on
 * ordinary material" is not the same as cannot clip.
 */

module audio_mixer #(
	parameter integer FRAC_BITS = 14,

	// log2 of the channel count. 3 gives eight channels; 2 gives four,
	// which is all a ProTracker MOD needs and is the dial to reach for
	// when placement is tight -- it halves the per-channel register
	// file and turns every 8:1 read mux into a 4:1. The sequencer
	// itself is unchanged, since it was already time-multiplexed.
	//
	// Set from rtl/boards.vh via `AUDIO_MIXER_CH_BITS.
	parameter integer CH_BITS = 3
)
(
	input wire clk,
	input wire rst,

	// one pulse per sample period, from rtl/audio_out.v. Everything
	// here is driven from this, so the mixer runs at exactly the rate
	// the DAC consumes and cannot drift from it.
	input wire frame_req,

	// -- channel configuration, written by rtl/audio.v --
	//
	// cfg_we pulses for one cycle with cfg_ch/cfg_reg/cfg_dat valid.
	// cfg_reg selects which of the six words (see rtl/audio.v's
	// register map).
	input wire cfg_we,
	input wire [2:0] cfg_ch,
	input wire [2:0] cfg_reg,
	input wire [31:0] cfg_dat,

	input wire [7:0] mixvol,

	// -- wishbone MASTER on the main bus --
	output reg [31:0] m_adr_o,
	output wire [31:0] m_dat_o,
	input wire [31:0] m_dat_i,
	output wire m_we_o,
	output wire [3:0] m_sel_o,
	output reg m_stb_o,
	output reg m_cyc_o,
	input wire m_ack_i,

	// -- mixed output, valid from the cycle after a frame completes --
	output reg signed [15:0] out_l,
	output reg signed [15:0] out_r,

	// bit per channel, for software to see what is still sounding
	output wire [7:0] active_o,

	// -- bus read probe --
	//
	// The address and the full word of the LAST fetch this block made.
	// Software can read the same address itself and compare: if they
	// differ, the mixer is not seeing what the CPU sees and the fault
	// is in the bus path, not in the mixing. Nothing else can
	// distinguish "the mixer reads the wrong bytes" from "the mixer
	// mixes them wrongly" without an oscilloscope.
	output reg [31:0] dbg_adr_o,
	output reg [31:0] dbg_dat_o,

	// -- playback position readback --
	//
	// `pos_sel` picks a channel; `pos_o` carries that channel's phase
	// accumulator, in the same 18.14 fixed point ch_pos itself uses.
	// The integer part is a BYTE OFFSET from that channel's BASE.
	//
	// Added for streaming playback (sw/apps/play): with LOOPST=0 and
	// LOOPLEN set to the buffer size, a channel walks a ring buffer
	// forever, and the ONLY thing software cannot otherwise learn is
	// how much of that ring has been consumed -- which is exactly what
	// it needs in order to know how much is safe to refill. dbg_adr_o
	// cannot answer it: that is the last fetch made by WHICHEVER
	// channel fetched most recently, so with two channels running it
	// reports one of them at random.
	//
	// -- WHY THIS IS A REGISTER AND NOT A COMBINATIONAL MUX --
	//
	// Read this before "simplifying" it into an assign, because the
	// simplification is the whole cost.
	//
	// The obvious implementation is `assign pos_o = ch_pos[pos_sel];`,
	// or mapping the positions into audio.v's register read case
	// directly. Either puts a 32-bit 8:1 mux fed by eight flip-flop
	// arrays INTO THE WISHBONE READ PATH, in series with the register
	// mux audio.v already has and whatever routing separates the two
	// blocks. That is a new combinational path spanning two modules,
	// added to a design that is at 79% TRELLIS_COMB on Obst and whose
	// placement is already seed-sensitive (see docs/audio.md,
	// "Measured cost").
	//
	// Registering it instead splits that into two paths that are each
	// trivially short:
	//
	//   ch_pos[*] -> 8:1 mux -> pos_o          flop to flop, one mux,
	//                                          nothing else in it
	//   pos_o -> audio.v's wb_dat_o case       one more arm on a case
	//                                          that already has twelve
	//
	// Neither touches the sequencer, the multiplier, the accumulators,
	// or the ch_pos WRITE path. The only effect on anything that
	// already existed is one extra load on each ch_pos bit -- and its
	// existing reader, ST_FETCH, is deliberately the shallowest state
	// in the machine (register reads and nothing else), so it has the
	// most slack of anywhere this could have landed.
	//
	// The cost is that pos_o is one cycle behind ch_pos and two cycles
	// behind a write to pos_sel. Both are invisible: a CPU read is a
	// whole bus transaction later, and the value describes a position
	// that advances once per audio frame -- 1088 cycles apart at
	// 44.1kHz. Being one cycle stale is not a rounding error, it is
	// not even a different answer.
	input wire [2:0] pos_sel,
	output reg [31:0] pos_o
);

	localparam integer CHANNELS = (1 << CH_BITS);

	localparam ST_IDLE   = 4'd0;
	localparam ST_FETCH  = 4'd1;
	localparam ST_PREP   = 4'd2;
	localparam ST_PREP2  = 4'd3;
	localparam ST_READ   = 4'd4;
	localparam ST_MULL   = 4'd5;
	localparam ST_MULL2  = 4'd13;
	localparam ST_ACCL   = 4'd6;
	localparam ST_ACCR   = 4'd7;
	localparam ST_ADV    = 4'd8;
	localparam ST_SCL    = 4'd9;
	localparam ST_SCL2   = 4'd10;
	localparam ST_SCR    = 4'd11;
	localparam ST_DONE   = 4'd12;

	// -- per-channel configuration, CPU-written / sequencer-read --
	//
	// Distributed RAM: one writer, one reader, eight entries. See this
	// file's header.
	(* ram_style = "distributed" *) reg [31:0] ch_base   [0:CHANNELS-1];
	// 20 bits, not 32. A ProTracker sample is at most 65535 WORDS =
	// 131070 bytes, which is 17 bits; 20 leaves headroom and still
	// halves every comparator these feed. The 32-bit versions put a
	// full-width carry chain on the critical path for a value that
	// physically cannot use the top fourteen bits.
	(* ram_style = "distributed" *) reg [19:0] ch_len    [0:CHANNELS-1];
	(* ram_style = "distributed" *) reg [19:0] ch_lstart [0:CHANNELS-1];
	(* ram_style = "distributed" *) reg [19:0] ch_llen   [0:CHANNELS-1];
	(* ram_style = "distributed" *) reg [31:0] ch_step   [0:CHANNELS-1];
	(* ram_style = "distributed" *) reg [31:0] ch_ctrl   [0:CHANNELS-1];

	/*
	 * -- 8-bit and 16-bit samples --
	 *
	 * CH_CTRL[18] (FMT16) picks a halfword fetch instead of a byte.
	 * Off by default, so every channel comes up 8-bit and every app
	 * written before this bit existed is unaffected.
	 *
	 * An 8-bit sample is promoted into the HIGH byte of `sample` and
	 * the output stage shifts by 18 instead of 10. Those cancel
	 * exactly, so 8-bit playback is BIT-IDENTICAL to what this block
	 * produced before -- verified by rtl/tb/tb_audio_mixer.v passing
	 * all of its checks unmodified, including the exact expected
	 * out_l values.
	 *
	 * That promotion is why there is one shift and not two. Keeping
	 * `>>> 10` for 8-bit and `>>> 18` for 16-bit would mean the shift
	 * depends on which channel the sequencer last visited, while the
	 * accumulator holds a SUM over all of them -- so a frame mixing
	 * both depths would have no correct answer. Normalising at the
	 * fetch is what lets the two coexist in one accumulator.
	 *
	 * Cost, measured (synth_ecp5 -abc9, packed by nextpnr-ecp5):
	 * +62 TRELLIS_COMB, +11 TRELLIS_FF, and NO extra DSP -- the
	 * widened 27x8 final multiply still packs into the same two
	 * MULT18X18D the 24x8 one used.
	 */

	// -- per-channel playback state, written from two places --
	reg [31:0] ch_pos [0:CHANNELS-1];
	reg [CHANNELS-1:0] ch_active;

	reg [3:0] state;
	reg [CH_BITS-1:0] seq;

	reg signed [26:0] acc_l;
	reg signed [26:0] acc_r;

	reg [31:0] cur_base;
	reg [31:0] cur_pos;
	reg [31:0] cur_step;
	reg [19:0] cur_len;
	reg [19:0] cur_lstart;
	reg [19:0] cur_llen;
	reg [31:0] cur_ctrl;

	// Everything below is computed one operation per cycle in
	// ST_PREP / ST_PREP2 so that no single state carries a chain of
	// 32-bit arithmetic -- see this file's header.
	reg [31:0] cur_addr;      // word address of the sample byte
	reg [1:0] cur_byte;       // which byte within that word
	reg [31:0] cur_next;      // pos + step
	reg [31:0] cur_wrapped;   // pos + step - looplen, for a loop wrap
	// Loop end kept as a BYTE offset, not a phase value. The wrap test
	// then compares 20 bits instead of 32, and the only thing that
	// still needs full phase width is the subtract itself.
	reg [19:0] cur_loopend;   // loopstart + looplen, in bytes
	reg [31:0] cur_llen_fx;   // looplen << FRAC
	reg cur_past_end;
	reg cur_do_wrap;
	reg cur_skip;

	reg signed [15:0] sample;
	reg signed [23:0] sc_l;
	reg signed [23:0] sc_r;

	wire [7:0] cur_gain_l = cur_ctrl[7:0];
	wire [7:0] cur_gain_r = cur_ctrl[15:8];
	wire cur_en = cur_ctrl[16];

	// THE one multiplier. Signed operand times unsigned gain, fed by
	// the sequencer -- sample x gain twice per channel, then acc x
	// mixvol twice per frame. Eighteen multiplies, one at a time,
	// against a thousand spare cycles.
	// REGISTERED OUTPUT. mul_p is combinational out of the DSP and
	// straight into a carry chain -- yosys splits a 24x9 multiply into
	// a MULT18X18D plus CCU2C adders, so "one multiply" is really a
	// DSP delay followed by a 24-bit add, and then the accumulate adds
	// another. Measured standalone that whole chain is 11.4ns of a
	// 20.8ns budget, which leaves nothing for routing in a design at
	// 79% utilisation.
	//
	// So the product lands in a register and is consumed the cycle
	// after. Cycles are the resource this block has spare: it uses
	// about 100 of the 1088 in a frame, and this adds three.
	reg signed [26:0] mul_a;
	reg [7:0] mul_b;
	reg signed [35:0] mul_r;
	wire signed [35:0] mul_p = mul_a * $signed({1'b0, mul_b});

	// Clamp from a REGISTER, so this is one compare-and-mux rather
	// than the tail of a multiply.
	wire signed [15:0] clamp_l =
		(sc_l > 24'sd32767) ? 16'sd32767 :
		(sc_l < -24'sd32768) ? -16'sd32768 : sc_l[15:0];
	wire signed [15:0] clamp_r =
		(sc_r > 24'sd32767) ? 16'sd32767 :
		(sc_r < -24'sd32768) ? -16'sd32768 : sc_r[15:0];

	assign m_dat_o = 32'h0000_0000;
	assign m_we_o = 1'b0;
	// READ. On this bus that means sel = 0, NOT 1111.
	//
	// rtl/mem/sdram_kianv.v ignores wb_we_i completely and decides
	// read-versus-write on `wb_sel_i == 4'b0000`. Driving all-ones
	// with we low therefore does not request a read -- it performs a
	// WRITE of wb_dat_i, which this block ties to zero.
	//
	// So the mixer was erasing the very sample data it was trying to
	// play, one word per fetch, and getting stale bus content back
	// because no read ever happened. It looked exactly like a
	// corrupted bus: the mixer returned instruction words and
	// pointers, and the CPU's own buffer came back zeroed after the
	// mixer had run over it.
	//
	// Obst never showed it. rtl/mem/sram.v honours we, so the same
	// cycle was a harmless read there, and the mixer worked.
	//
	// rtl/gpu/gpu_blit.v follows the same convention -- 4'b0000 to
	// read, 4'b1111 to write. This block is the odd one out and was
	// wrong from the day it was written.
	assign m_sel_o = 4'b0000;
	assign active_o = { {(8-CHANNELS){1'b0}}, ch_active };

	// -- position snapshot --
	//
	// Its OWN always block, deliberately, rather than another line in
	// the sequencer's. It shares no logic with the state machine and
	// must not be able to grow an accidental dependency on it: put
	// this inside the big block and the next person to add an `if`
	// around a group of assignments can silently make the position
	// readback conditional on the mixer's state.
	//
	// pos_sel is indexed narrowly so this still elaborates when
	// `AUDIO_MIXER_CH_BITS is lowered to 2 -- the same treatment
	// cfg_ch already gets.
	//
	// See the port declaration for why this is registered.
	always @(posedge clk) begin
		if (rst)
			pos_o <= 32'h0000_0000;
		else
			pos_o <= ch_pos[pos_sel[CH_BITS-1:0]];
	end

	// Trigger decode for a CPU write to a channel's CTRL word. Bit 17
	// restarts the channel; bits 31:24 are the start offset in units
	// of 256 bytes, which is exactly ProTracker's 9xx sample-offset
	// effect and is why the field is that shape.
	wire cfg_ctrl_write = cfg_we && (cfg_reg == 3'd5);
	wire cfg_trigger = cfg_ctrl_write && cfg_dat[17];
	// offset is in units of 256 bytes, and pos is FRAC_BITS-fractional,
	// so the shift is (8 + FRAC_BITS) -- 22 by default.
	//
	// Written as a zero-extend then shift, NOT as {offset, 24'b0}
	// shifted again: that form puts the field at bit 24 first and the
	// second shift walks it straight off the top of the word. It cost
	// the whole 9xx sample-offset effect and the testbench caught it.
	wire [31:0] cfg_trigger_pos =
		{ 24'b0, cfg_dat[31:24] } << (8 + FRAC_BITS);

	integer i;

	always @(posedge clk) begin

		if (rst) begin

			state <= ST_IDLE;
			seq <= 3'd0;
			acc_l <= 27'sd0;
			acc_r <= 27'sd0;
			out_l <= 16'sd0;
			out_r <= 16'sd0;
			ch_active <= 0;
			m_stb_o <= 1'b0;
			m_cyc_o <= 1'b0;
			m_adr_o <= 32'h0000_0000;
			dbg_adr_o <= 32'h0000_0000;
			dbg_dat_o <= 32'h0000_0000;
			mul_a <= 27'sd0;
			mul_b <= 8'd0;
			mul_r <= 36'sd0;
			for (i = 0; i < CHANNELS; i = i + 1)
				ch_pos[i] <= 32'h0000_0000;

		end else begin

			// The multiplier pipeline register, clocked
			// unconditionally: mul_a/mul_b only change when the
			// sequencer sets them, so this simply trails them by one
			// cycle wherever they are used.
			mul_r <= mul_p;

			// -- CPU writes to channel configuration --
			//
			// Deliberately not gated on the sequencer being idle. A
			// write lands between frames in practice (software writes
			// registers on a tracker row, ~50 times a second, against
			// a frame every 1088 cycles), and even if one did land
			// mid-frame the worst case is one sample of the old value
			// on one channel. Interlocking against that would cost a
			// handshake in the software path for something inaudible.
			// A write aimed at a channel this build does not have is
			// DROPPED, not wrapped around onto a real one. Software
			// that assumes eight channels then plays four and hears
			// silence from the rest, which is obvious; aliasing would
			// have channel 4 fight channel 0 for the same registers,
			// which is not.
			if (cfg_we && cfg_ch < CHANNELS) begin
				case (cfg_reg)
					3'd0: ch_base[cfg_ch[CH_BITS-1:0]]   <= cfg_dat;
					3'd1: ch_len[cfg_ch[CH_BITS-1:0]]    <= cfg_dat[19:0];
					3'd2: ch_lstart[cfg_ch[CH_BITS-1:0]] <= cfg_dat[19:0];
					3'd3: ch_llen[cfg_ch[CH_BITS-1:0]]   <= cfg_dat[19:0];
					3'd4: ch_step[cfg_ch[CH_BITS-1:0]]   <= cfg_dat;
					3'd5: ch_ctrl[cfg_ch[CH_BITS-1:0]]   <= cfg_dat;
					default: ;
				endcase
			end

			if (cfg_ctrl_write && cfg_ch < CHANNELS) begin
				if (cfg_trigger) begin
					ch_pos[cfg_ch[CH_BITS-1:0]] <= cfg_trigger_pos;
					ch_active[cfg_ch[CH_BITS-1:0]] <= cfg_dat[16];
				end else if (!cfg_dat[16]) begin
					// clearing EN stops the channel; setting it
					// without TRIGGER resumes wherever it was, which
					// is what a volume-only row change wants
					ch_active[cfg_ch[CH_BITS-1:0]] <= 1'b0;
				end
			end

			case (state)

				ST_IDLE: begin
					if (frame_req) begin
						seq <= 0;
						acc_l <= 27'sd0;
						acc_r <= 27'sd0;
						state <= ST_FETCH;
					end
				end

				// One LUT-RAM read per field, straight into registers.
				// Nothing is computed from them here.
				ST_FETCH: begin
					cur_base <= ch_base[seq];
					cur_pos <= ch_pos[seq];
					cur_step <= ch_step[seq];
					cur_len <= ch_len[seq];
					cur_lstart <= ch_lstart[seq];
					cur_llen <= ch_llen[seq];
					cur_ctrl <= ch_ctrl[seq];
					cur_skip <= !ch_active[seq];
					state <= ST_PREP;
				end

				// Five independent single-operation results, all from
				// registers written last cycle. None of them feeds
				// another in this state.
				ST_PREP: begin
					cur_addr <= cur_base + (cur_pos >> FRAC_BITS);
					// low two bits of the same sum used for cur_addr
					cur_byte <= (cur_base + (cur_pos >> FRAC_BITS)) & 2'b11;
					cur_next <= cur_pos + cur_step;
					cur_loopend <= cur_lstart + cur_llen;
					cur_llen_fx <= { 12'b0, cur_llen } << FRAC_BITS;
					// The integer part of a 32-bit phase with 14
					// fractional bits is 18 bits, not 20 -- slicing
					// [FRAC+19:FRAC] runs off the top of the register
					// and the compare silently never fires.
					cur_past_end <=
						({ 2'b00, cur_pos[31:FRAC_BITS] } >= cur_len);
					state <= ST_PREP2;
				end

				// The second half of the loop arithmetic, kept apart
				// from the first because pos+step feeds both of these.
				ST_PREP2: begin
					cur_wrapped <= cur_next - cur_llen_fx;
					cur_do_wrap <= (cur_llen != 20'd0)
						&& ({ 2'b00, cur_next[31:FRAC_BITS] } >= cur_loopend);

					if (cur_skip || !cur_en || cur_past_end) begin
						if (cur_past_end && !cur_skip)
							ch_active[seq] <= 1'b0;
						cur_skip <= 1'b1;
						state <= ST_ADV;
					end else begin
						state <= ST_READ;
					end
				end

				// The address was computed two states ago, so this is
				// pure bus handshaking.
				ST_READ: begin
					if (!m_cyc_o) begin
						m_adr_o <= { cur_addr[31:2], 2'b00 };
						m_cyc_o <= 1'b1;
						m_stb_o <= 1'b1;
					end else if (m_ack_i) begin
						m_cyc_o <= 1'b0;
						m_stb_o <= 1'b0;
						dbg_adr_o <= { cur_addr[31:2], 2'b00 };
						dbg_dat_o <= m_dat_i;
						if (cur_ctrl[18]) begin
							// 16-bit: halfword select, used as-is
							if (cur_addr[1])
								sample <= m_dat_i[31:16];
							else
								sample <= m_dat_i[15:0];
						end else begin
							// 8-bit: promoted into the HIGH byte, so
							// both depths share one output shift and
							// existing MOD playback is bit-identical
							case (cur_byte)
								2'd0: sample <= { m_dat_i[7:0],   8'b0 };
								2'd1: sample <= { m_dat_i[15:8],  8'b0 };
								2'd2: sample <= { m_dat_i[23:16], 8'b0 };
								default: sample <= { m_dat_i[31:24], 8'b0 };
							endcase
						end
						state <= ST_MULL;
					end
				end

				// mul_r trails mul_a/mul_b by one cycle, so each
				// product is set up in one state and consumed in the
				// state after next. ST_MULL2 exists purely to be that
				// gap, and it doubles as the setup for the second
				// product -- so two multiplies cost three states, not
				// four.
				ST_MULL: begin
					mul_a <= { {11{sample[15]}}, sample };
					mul_b <= cur_gain_l;
					state <= ST_MULL2;
				end

				ST_MULL2: begin
					mul_b <= cur_gain_r;
					state <= ST_ACCL;
				end

				ST_ACCL: begin
					acc_l <= acc_l + mul_r[26:0];   // sample * gain_l
					state <= ST_ACCR;
				end

				ST_ACCR: begin
					acc_r <= acc_r + mul_r[26:0];   // sample * gain_r
					state <= ST_ADV;
				end

				// Just a mux between two values computed earlier.
				ST_ADV: begin
					if (!cur_skip)
						ch_pos[seq] <= cur_do_wrap ? cur_wrapped : cur_next;

					if (seq == (CHANNELS - 1)) begin
						mul_a <= acc_l;
						mul_b <= mixvol;
						state <= ST_SCL;
					end else begin
						seq <= seq + 1'b1;
						state <= ST_FETCH;
					end
				end

				// The master scale, through the same one multiplier.
				ST_SCL: begin
					mul_a <= acc_r;
					state <= ST_SCL2;
				end

				ST_SCL2: begin
					// >>> 10 on the SIGNED product.
					//
					// Written as a plain shift of mul_p, not as a part
					// select. `mul_p[32:9] >>> 1` looks equivalent and
					// is not: a part select of a signed value yields an
					// UNSIGNED result, so the second shift brought in
					// a zero instead of the sign and every negative
					// sample came out as a large positive one. Audible
					// as loud distortion that still sounds like music,
					// which is the worst kind of wrong.
					sc_l <= mul_r >>> 18;
					state <= ST_SCR;
				end

				ST_SCR: begin
					sc_r <= mul_r >>> 18;   // see sc_l above
					state <= ST_DONE;
				end

				ST_DONE: begin
					out_l <= clamp_l;
					out_r <= clamp_r;
					state <= ST_IDLE;
				end

				default: state <= ST_IDLE;

			endcase

		end

	end

endmodule
