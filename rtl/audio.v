/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Audio -- a sample FIFO, a register interface and an interrupt, in
 * front of rtl/audio_out.v's DAC serialisers.
 *
 * This is PHASE 1 of the audio subsystem: software mixes, the hardware
 * plays. There is no bus master here and no arbiter change. A hardware
 * mixer that reads sample data out of main memory is phase 3 and will
 * attach to this block's own FIFO, which is why the register map below
 * leaves room for it. See docs/audio.md.
 *
 * -- THE FIFO IS 1024 FRAMES OF BLOCK RAM. IT USED TO BE 128 FRAMES
 *    OF DISTRIBUTED RAM, AND THAT WAS WRONG --
 *
 * The original sizing argued hard for zero BRAM, on the grounds that
 * Lakritz has only three blocks left and 128 frames is 2.9ms at
 * 44.1kHz, which is comfortable if refill is paced by this block's own
 * watermark interrupt.
 *
 * That reasoning assumed a system where the player gets the CPU when
 * it asks for it. This one is preemptive round-robin on the 732Hz
 * KTIMER, and both sw/apps/wm and sw/os/sh busy-poll rather than
 * blocking -- so they are always runnable, and a player is the third
 * runnable process at best:
 *
 *     runnable procs   player's CPU share   longest gap off-CPU
 *              2              50%                  1.37 ms
 *              3              33%                  2.73 ms
 *              4              25%                  4.10 ms
 *
 * Two things follow, and only one of them is this block's problem.
 *
 * THE BUFFER must cover the longest gap plus margin. 128 frames is
 * 5.8ms at 22kHz and 2.9ms at 44.1kHz -- the second of those is
 * already shorter than a three-way round trip. 1024 frames is 46ms and
 * 23ms, which is slack rather than a coin toss.
 *
 * THE MIXER must run at N times real time during its own slice, since
 * it only has 1/N of the CPU. No FIFO size fixes that, and it is worth
 * being explicit because the two failures sound similar: a buffer that
 * is too small gives occasional clicks, while a mixer that is too slow
 * plays the whole module slow and smooth. sw/apps/mod reports a `mix`
 * percentage on screen precisely so the two can be told apart.
 *
 * -- WHAT THE BRAM COSTS, AND WHY 1024 AND NOT 512 --
 *
 * Measured, yosys 0.33:
 *
 *     Obst     50 -> 52 of 56 DP16KD    4 free
 *     Lakritz  53 -> 55 of 56 DP16KD    1 free   <-- watch this one
 *     Mozart   50 -> 52 of 108 DP16KD  56 free
 *
 * A DP16KD is 18 bits wide, so a 32-bit FIFO needs TWO of them at any
 * depth. 512 frames and 1024 frames cost exactly the same two blocks;
 * anything below 1024 is paying for block RAM it is not using. That is
 * the whole reason for the depth.
 *
 * LAKRITZ IS LEFT WITH ONE SPARE BLOCK. If that is too tight, the
 * options in order of preference are: drop `ICACHE_KB from 4 to 2
 * (frees one or two blocks, costs some CPU), or set `AUDIO_FIFO_LOG2
 * lower and change the ram_style attribute below back to
 * "distributed" -- 128 frames cost 64 TRELLIS_DPR16X4 and about 420
 * LUT4, and 256 nearly triples that because of the read multiplexer.
 *
 * -- A BLOCK RAM READ IS SYNCHRONOUS, HENCE THE OUTPUT REGISTER --
 *
 * Distributed RAM could be read combinationally, which is what let
 * audio_out latch a frame on the same edge that raised frame_req.
 * Block RAM cannot, so `head` holds one frame ahead of the array and
 * IS what audio_out sees. `ram_ready` tracks whether the registered
 * read output can be believed: it is cleared whenever the read pointer
 * moves or a write lands (which may have been to the address just
 * read, and same-cycle read/write behaviour is not worth relying on).
 * The cost is a cycle or two of refill latency against a frame period
 * of 1088 cycles.
 *
 * -- OLD REASONING, KEPT BECAUSE THE CONSTRAINT IS STILL REAL --
 *
 * Measured at commit d02d520, yosys 0.33, ECP5:
 *
 *     Obst     50 of 56 DP16KD used    6 free
 *     Lakritz  53 of 56 DP16KD used    3 free
 *     Mozart   50 of 108 DP16KD used  58 free
 *
 * Lakritz is the binding board, and the three blocks it has left are
 * the entire headroom for anything else this SOC ever wants -- a
 * bigger instruction cache first among them (`ICACHE_KB 4 costs the 3
 * blocks that are the whole difference between Lakritz and Obst
 * above). Spending one of them on an audio FIFO would be spending the
 * scarcest resource on the least deserving thing.
 *
 * So the FIFO is distributed RAM: `ram_style = "distributed"` plus an
 * ASYNCHRONOUS read below, both of which are needed. The attribute
 * alone does not do it -- a synchronous read is a shape yosys prefers
 * to map to a DP16KD regardless, and it will. If you change the read
 * path here, re-check the DP16KD count for Lakritz before believing it
 * still fits; the failure is silent at synthesis and shows up as
 * nextpnr refusing to place a build that used to place.
 *
 * Cost of that choice, measured over depth:
 *
 *     depth  buffer @44.1kHz  DPR16X4  LUT4+PFUMX
 *        64          1.45 ms       32        ~215
 *       128          2.90 ms       64        ~420
 *       256          5.80 ms      128       ~1120
 *
 * 128 is the default. The jump at 256 is the read multiplexer tree,
 * not the storage, which is the shape of thing worth knowing before
 * reaching for more depth: doubling the buffer costs nearly triple.
 *
 * -- 2.9ms is enough BECAUSE the interrupt exists --
 *
 * The kernel tick (rtl/sysctl.v's rtc_ctr, cpu_irq[3]) is 732Hz, a
 * 1.37ms period. Refilling from there would leave barely one tick of
 * slack against jitter, and an audio underrun is not a subtle failure
 * -- it is a click on every occurrence.
 *
 * So this block raises its own interrupt, cpu_irq[7], whenever the
 * FIFO falls below WMARK. Refill is then paced by the FIFO itself:
 * with WMARK at half depth, software is woken with 1.45ms of audio
 * still queued and 1.45ms of space to fill. That is a completely
 * different margin from the same buffer polled on someone else's
 * clock.
 *
 * The interrupt is LEVEL-SENSITIVE and must be non-latched in the
 * CPU's LATCHED_IRQ mask -- see rtl/sysctl.v, where bit 7 is cleared
 * alongside bit 4 (the UART, for the same reason). A latched
 * level-sensitive source re-fires immediately after every handler
 * return and the machine makes no forward progress. Polling STATUS is
 * always available as well and needs none of this; an app that just
 * wants a beep should use it.
 *
 * -- where it lives in the address map --
 *
 * 0x7000_05xx, the SIXTH tenant of nibble 7, alongside csrs (00xx),
 * cache (01xx), socctl (02xx), rtc (03xx) and trng (04xx). No decode
 * mask change was needed: rtl/sysctl.v already widened the tenant mask
 * to 0x700 when trng was added, and its own comment says the leftover
 * 05xx..07xx range is "what a future sixth tenant will want anyway".
 * This is that tenant.
 *
 * The rule that makes an optional tenant safe applies here unchanged:
 * csrs.v absorbs this window on a build without `AUDIO, acks it, and
 * reads back zero -- so MAGIC fails and software correctly concludes
 * there is no audio. An address NOTHING decodes gets no ack at all and
 * hangs the CPU forever, which is why the absorption matters and why
 * software must check the FEATURE bit at 0x7000_0008 before reading
 * MAGIC here. See sw/common/zaudio.h, which does it in that order.
 *
 * -- the window offset trap --
 *
 * wb_adr_i is masked to this block's own window by rtl/sysctl.v, not
 * passed through raw. At 0x7000_05xx the raw wbm_adr_sel_word is 0x140
 * for register 0, so an unmasked address matches no case: writes
 * vanish and MAGIC reads back zero, with no error anywhere. Six bits
 * (not three, as rtc.v and trng.v use) because this block has eight
 * registers now and phase 3 adds per-channel state; six covers the
 * whole 256-byte window and means the decode never has to be revisited.
 *
 * Register map (word-addressed):
 *
 *   0  MAGIC   R   32'h5A41_5544 ("ZAUD"). Check this before anything
 *                  else here, and check Z_FEATURE_AUDIO before this.
 *   1  CTRL    RW  0  EN      enable output (0 = mute, clocks keep
 *                             running -- see audio_out.v)
 *                  1  IRQEN   raise cpu_irq[7] while level < WMARK
 *                  2  SWAPLR  swap channels in the output stage
 *                  3  FLUSH   write 1 to empty the FIFO. Command bit,
 *                             never stored, always reads 0.
 *                  4  CLRUR   write 1 to clear STATUS.UNDERRUN.
 *                             Command bit, never stored, reads 0.
 *                  5  RESERVED -- see the S/PDIF note below. Do not
 *                             reuse; write 0.
 *                  6  MIXEN   take the DAC's input from the HARDWARE
 *                             MIXER (rtl/audio_mixer.v) instead of
 *                             from the FIFO below. The two are fully
 *                             independent: the FIFO keeps working, it
 *                             is simply not what feeds the output
 *                             stage while this is set.
 *                  Bits 31:7 reserved, write 0.
 *   2  STATUS  R   15:0  LEVEL     frames currently queued
 *                  16    EMPTY
 *                  17    FULL
 *                  18    BELOW     level < WMARK (the IRQ condition)
 *                  19    UNDERRUN  sticky; a frame was needed and the
 *                                  FIFO was empty. Cleared by CTRL.CLRUR.
 *   3  DATA    W   push one frame: { left[31:16], right[15:0] }.
 *                  Signed 16-bit, twos complement. A write while FULL
 *                  is DROPPED, silently -- see below. Reads return 0.
 *   4  RATE    RW  [7:0] half-BCK period in sys_clk cycles.
 *                  fs = CLK_HZ / (64 * RATE). Resets to 17 ->
 *                  44117.6 Hz at 48MHz. 34 -> 22058.8 Hz.
 *   5  WMARK   RW  [15:0] interrupt watermark. Resets to depth/2.
 *   6  CONFIG  R   { 16'h5A41, 4'b0, FORMATS[3:0], DEPTH_LOG2[7:0] }
 *                  FORMATS bit 0 = a 1-bit DAC is wired on this board,
 *                  bit 1 = a PT8211 is, bit 2 = an optical S/PDIF
 *                  transmitter is (RESERVED, nothing sets it yet --
 *                  see the attach-point note below). Reported from
 *                  rtl/sysctl.v's own `ifdefs, so software can tell
 *                  which output it is actually driving.
 *                  The 16'h5A41 ("ZA") signature is the same trick,
 *                  for the same reason, as socctl.v's VIDEO register:
 *                  a bitstream can have this block (so MAGIC is right)
 *                  and predate this register, and that read returns
 *                  zero -- indistinguishable from a working block
 *                  reporting a depth of 1 and no DACs.
 *   7  CLKHZ   R   CLK_HZ, so software can compute the exact sample
 *                  rate for any RATE rather than assuming 48MHz.
 *
 *   8  MIXVOL  RW  [7:0] hardware mixer master scale. Resets to 128,
 *                  which is safe for eight channels at full gain; a
 *                  four-channel module wants 255. Software owns this
 *                  because software knows how many channels the
 *                  module has. See audio_mixer.v on the arithmetic.
 *   9  MIXSTAT R   [7:0] bit per channel, set while that channel is
 *                  still sounding. Read-only status, not a control --
 *                  a one-shot sample clears its own bit when it runs
 *                  off the end, which is how software knows a note
 *                  finished without timing it.
 *
 *  16..63  PER-CHANNEL MIXER STATE, six words per channel, channel c
 *          at 16 + c*6 + n. WRITE ONLY -- these go straight into
 *          audio_mixer.v's own register file and are not read back.
 *
 *    n=0  BASE     byte address of the sample data in main memory
 *    n=1  LEN      sample length in bytes
 *    n=2  LOOPST   loop start, bytes from BASE
 *    n=3  LOOPLEN  loop length in bytes; 0 means one-shot
 *    n=4  STEP     phase increment per output frame, 18.14 fixed
 *                  point. step = (sample_rate_hz << 14) / output_hz.
 *    n=5  CTRL     { offset[31:24], 6'b0, TRIG[17], EN[16],
 *                    gain_r[15:8], gain_l[7:0] }
 *
 *                  gains are 0..255. EN gates the channel; TRIG
 *                  (write-only, self-clearing) restarts it from
 *                  `offset` * 256 bytes -- which is exactly
 *                  ProTracker's 9xx sample-offset effect, and why the
 *                  field is that shape. Writing CTRL with EN set and
 *                  TRIG clear changes gains WITHOUT restarting the
 *                  sample, which is what a volume-only tracker row
 *                  needs and is the single most common write here.
 *
 *          Six words rather than seven so the whole thing is 8 + 48 =
 *          56 of the window's 64, leaving room to grow.
 *
 * -- a write to a full FIFO is dropped, not blocked --
 *
 * The alternative is stalling the ack until there is room, which turns
 * a register write into an unbounded wait on this bus and would hang
 * the CPU whenever EN is clear (the FIFO never drains, so the write
 * never completes). Software that wants to know reads STATUS. The
 * shape to write is: block on the interrupt, then push until FULL.
 *
 * -- ATTACHING A SECOND SERIALISER (optical S/PDIF) --
 *
 * Some boards have an optical S/PDIF transmitter. Nothing here drives
 * one yet. This note exists so that adding it later is an addition
 * rather than a rework, and so the two non-obvious facts below do not
 * have to be rediscovered from a waveform.
 *
 * A biphase-mark encoder attaches as a SIBLING of audio_out, not
 * downstream of it, and it needs exactly three signals that already
 * exist: `frame_req`, `fifo_dout` and `rate`. Specifically:
 *
 *   fifo_dout IS VALID FOR THE WHOLE CYCLE frame_req IS HIGH. audio_out
 *   latches its own copy combinationally, one cycle earlier, at the
 *   phase-62 boundary; rptr does not advance until the end of the
 *   frame_req cycle. So a sibling that latches fifo_dout on
 *   (frame_req == 1) gets the same frame audio_out is playing, not the
 *   next one and not a stale one. Two consumers, one pop.
 *
 *   fs IS OWNED BY audio_out. A sibling must slave its own bit timing
 *   to frame_req rather than dividing sys_clk independently, or the
 *   two outputs drift apart and the digital one underruns on a
 *   schedule of its own.
 *
 * The rate arithmetic matters here and is worth stating plainly.
 * S/PDIF is 64 time slots per stereo frame, biphase-mark coded, so it
 * needs 128 half-cells per frame -- twice this block's 64 PT8211 slots.
 * A half-cell is therefore RATE/2 sys_clk cycles:
 *
 *   RATE=16  fs 46875.0 Hz   half-cell = 8 cycles exactly, no jitter
 *   RATE=17  fs 44117.6 Hz   half-cell = 8.5 cycles, needs a
 *                            fractional divider (+/- half a sys_clk
 *                            of edge jitter, ~10ns)
 *
 * RATE_RESET is a parameter for this reason among others: a board with
 * a transmitter can be brought up at 16 from rtl/sysctl.v without
 * every app having to know. See docs/audio.md for why 46875 is the
 * only exactly-reachable rate on this clocking and what the trade is.
 *
 * Two things are reserved for that work and should not be reused:
 * CONFIG's FORMATS bit 2 (above), and CTRL bit 5, for enabling the
 * digital output independently of the analogue one.
 *
 * One behavioural note that already generalises correctly: EN=0 MUTES,
 * it does not stop the output stage's clocks (see audio_out.v). That
 * is required for a PT8211, which is in an undefined state with a
 * stopped bit clock, and it is required for S/PDIF too but for a
 * different reason -- a receiver that loses the stream unlocks and
 * re-locks with an audible click, so a silent S/PDIF frame must still
 * be a transmitted frame. Anything added here must keep that property.
 *
 * -- what an underrun sounds like, and why it is a hold --
 *
 * When a frame is due and the FIFO is empty, the output stage repeats
 * the frame it already has rather than taking whatever is sitting in
 * the FIFO's memory at the read pointer. That memory holds a frame
 * from a whole buffer ago; playing it would be a burst of unrelated
 * audio, which is a far worse noise than a held sample and a much
 * harder one to recognise for what it is. A repeated sample is a
 * short flat spot, and STATUS.UNDERRUN says it happened.
 */

`include "boards.vh"

// Normally set by rtl/sysctl.v. Defaulted here as well so this file
// compiles standalone in a testbench, where sysctl.v is not read at
// all -- without this, the parameter expands to nothing and the
// instantiation below is a syntax error a long way from its cause.
`ifndef AUDIO_MIXER_CH_BITS
`define AUDIO_MIXER_CH_BITS 3
`endif

module audio_wb #(
	// FIFO depth is 2**DEPTH_LOG2 frames. See the cost table in this
	// file's header before raising it.
	parameter integer DEPTH_LOG2 = 10,

	// Which DACs rtl/sysctl.v actually wired on this board. Reported
	// in CONFIG; does not change any logic here.
	parameter [3:0] FORMATS = 4'b0000,

	parameter integer CLK_HZ = 48000000,

	// 17 -> 44117.6 Hz at 48MHz. See RATE above.
	parameter [7:0] RATE_RESET = 8'd17,

	// Power-on CTRL. Muted, no interrupt, no channel swap. If a board
	// turns out to need SWAPLR set, change it HERE rather than making
	// every app do it -- see audio_out.v's header.
	parameter [7:0] CTRL_RESET = 8'h00
)
(
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output reg wb_ack_o,
	input wb_cyc_i,

	// cpu_irq[7]. Level-sensitive -- see this file's header.
	output wire int_o,

	// 1-bit DACs (Obst, Lakritz)
	output wire AUDIO_L,
	output wire AUDIO_R,

	// PT8211/TM8211 (Mozart)
	output wire AUD_BCK,
	output wire AUD_WS,
	output wire AUD_DIN,

	// Optical S/PDIF (Sergei)
	output wire AUD_OPTICAL,

	// Wishbone MASTER on the main bus -- the hardware mixer's sample
	// fetches. Third master on rtl/arbiter_main.v. Read only; the
	// write signals exist so the port matches the others.
	output wire [31:0] mx_adr_o,
	output wire [31:0] mx_dat_o,
	input wire [31:0] mx_dat_i,
	output wire mx_we_o,
	output wire [3:0] mx_sel_o,
	output wire mx_stb_o,
	output wire mx_cyc_o,
	input wire mx_ack_i
);

	localparam MAGIC = 32'h5A41_5544;	// "ZAUD"
	localparam CONFIG_SIG = 16'h5A41;	// "ZA"
	localparam integer DEPTH = (1 << DEPTH_LOG2);
`ifdef AUDIO_MIXER
	localparam HAS_MIXER = 1'b1;
`else
	localparam HAS_MIXER = 1'b0;
`endif
	localparam [7:0] DEPTH_LOG2_B = DEPTH_LOG2;

	// Width of the zero padding that widens `level` to the 16 bits
	// LEVEL and WMARK are compared and reported in. Named rather than
	// written inline twice, so a change to DEPTH_LOG2 cannot make the
	// two disagree.
	localparam integer LEVEL_PAD = 15 - DEPTH_LOG2;

	// BLOCK RAM. See this file's header on why this changed from
	// distributed RAM, and what it cost.
	(* ram_style = "block" *)
	reg [31:0] fifo [0:DEPTH-1];

	reg [DEPTH_LOG2:0] wptr;
	reg [DEPTH_LOG2:0] rptr;

	// Output stage. A block RAM read is SYNCHRONOUS, so the frame the
	// output stage latches cannot come straight out of the array the
	// way it did from distributed RAM -- see this file's header.
	reg [31:0] head;
	reg head_valid;
	reg [31:0] ram_q;
	reg ram_ready;

	reg [7:0] ctrl;
	reg [7:0] rate;
	reg [7:0] mixvol;
	// Channel status sampling-frequency nibble. Advisory -- receivers
	// PLL to the line -- so software can say "48 kHz" while the line
	// runs at 46875. See rtl/audio_spdif.v.
	reg [3:0] spdif_fs;
	reg [15:0] wmark;
	reg underrun;

	wire sd_l;
	wire sd_r;
	wire frame_req;

	// Frames sitting in the array. The extra pointer bit is what
	// distinguishes full from empty: this reaches exactly DEPTH and
	// nothing else sets that bit.
	wire [DEPTH_LOG2:0] ram_level = wptr - rptr;
	wire ram_empty = (wptr == rptr);
	wire full = ram_level[DEPTH_LOG2];

	// What software sees, and what the watermark compares against, is
	// the array PLUS the one frame held in the output register.
	// Reporting only the array would make LEVEL read one low and, more
	// annoyingly, would let STATUS say EMPTY while a frame was still
	// to be played.
	wire [15:0] level_wide =
		{ {LEVEL_PAD{1'b0}}, ram_level } + { 15'b0, head_valid };
	wire empty = !head_valid && ram_empty;
	wire below = (level_wide < wmark);

	wire [31:0] fifo_dout = head;

	wire ctrl_en = ctrl[0];
	wire ctrl_irqen = ctrl[1];
	wire ctrl_swap = ctrl[2];
`ifdef AUDIO_MIXER
	wire ctrl_mixen = ctrl[6];
`else
	wire ctrl_mixen = 1'b0;
`endif

`ifdef AUDIO_MIXER
	wire signed [15:0] mix_l;
	wire signed [15:0] mix_r;
	wire [7:0] mix_active;
	wire [31:0] mix_dbg_adr;
	wire [31:0] mix_dbg_dat;
`else
	// No mixer in this build: MIXEN does nothing, MIXSTAT reads zero,
	// and the channel registers are written into a void. Software sees
	// mixstat == 0 with a valid CONFIG signature, which is what
	// z_audio_mixer_present() checks -- so sw/apps/mod falls back to
	// software mixing and says so, rather than playing silence.
	wire signed [15:0] mix_l = 16'sd0;
	wire signed [15:0] mix_r = 16'sd0;
	wire [7:0] mix_active = 8'h00;
	wire [31:0] mix_dbg_adr = 32'h0;
	wire [31:0] mix_dbg_dat = 32'h0;
`endif

	// Channel-register writes are decoded here and handed straight to
	// the mixer rather than being stored twice. Registers 16..63,
	// six per channel.
	wire cfg_hit = wb_cyc_i && wb_stb_i && !wb_ack_o && wb_we_i
		&& (wb_adr_i >= 32'd16) && (wb_adr_i < 32'd64);
	wire [5:0] cfg_off = wb_adr_i[5:0] - 6'd16;

	// cfg_off / 6 and cfg_off % 6, for eight channels of six words.
	// Written as a comparison chain rather than a divide: yosys would
	// synthesize a real divider otherwise, for a value with eight
	// possible answers.
	wire [2:0] cfg_ch =
		(cfg_off < 6'd6)  ? 3'd0 : (cfg_off < 6'd12) ? 3'd1 :
		(cfg_off < 6'd18) ? 3'd2 : (cfg_off < 6'd24) ? 3'd3 :
		(cfg_off < 6'd30) ? 3'd4 : (cfg_off < 6'd36) ? 3'd5 :
		(cfg_off < 6'd42) ? 3'd6 : 3'd7;
	// ch*6 as two shifts and an add, NOT as a multiply.
	//
	// `cfg_off - (cfg_ch * 3'd6)` reads harmlessly and yosys turns it
	// into a MULT18X18D. On Obst that put a DSP on the wishbone
	// ADDRESS DECODE path -- 3.93ns of DSP latency plus 4.3ns of
	// routing out to a DSP column and back, about 8ns of a 20.8ns
	// budget, to multiply a three-bit number by six. It was the single
	// largest item in the critical path and it made the 48MHz domain
	// fail at 31MHz.
	//
	// The comment two lines above already warned against writing a
	// divide here for the same reason. A multiply is the same mistake
	// wearing a different hat.
	wire [5:0] cfg_base = { 1'b0, cfg_ch, 2'b00 }    // ch * 4
		+ { 2'b00, cfg_ch, 1'b0 };                   // ch * 2
	wire [5:0] cfg_reg6 = cfg_off - cfg_base;
	wire [2:0] cfg_reg = cfg_reg6[2:0];

	// REGISTERED CONFIG PORT.
	//
	// Without these the path runs uninterrupted from the MTU's
	// translation base, through the main-bus address mux, through this
	// block's decode, and all the way into the mixer's per-channel
	// register file -- ending on the clock enable of ch_pos[7]. One
	// combinational path across three modules and most of the die:
	// 9.3ns of logic and 18.9ns of routing.
	//
	// A tracker writes these about fifty times a second, so a cycle of
	// latency is free, and it cuts that path in half at the module
	// boundary.
	reg cfg_we_r;
	reg [2:0] cfg_ch_r;
	reg [2:0] cfg_reg_r;
	reg [31:0] cfg_dat_r;

	// Gated by IRQEN alone, not by EN as well. An app enables the
	// interrupt when it has a handler and expects to be woken
	// immediately on an empty FIFO -- that first interrupt is how it
	// primes the pump.
	assign int_o = ctrl_irqen && below;

	assign AUDIO_L = sd_l;
	assign AUDIO_R = sd_r;

	// WHICH SOURCE FEEDS THE DAC. One mux, and the two paths are
	// otherwise completely independent -- the FIFO keeps accepting
	// pushes while the mixer plays, so sw/apps/audiotest and any
	// software-mixed app keep working unchanged, and there is a
	// fallback if the mixer misbehaves on real hardware.
	//
	// sample_valid is 1'b1 in mixer mode: the mixer always has a frame
	// ready (silence is a frame of zeros), so there is no such thing
	// as a mixer underrun and the hold-last-frame path never engages.
	wire signed [15:0] src_l = ctrl_mixen ? mix_l : fifo_dout[31:16];
	wire signed [15:0] src_r = ctrl_mixen ? mix_r : fifo_dout[15:0];
	wire src_valid = ctrl_mixen ? 1'b1 : head_valid;

`ifdef AUDIO_SPDIF
	// S/PDIF sits alongside the analogue output stage, not downstream
	// of it: same frame_req, same source mux, its own pin. Both run at
	// once on a board that has both.
	audio_spdif spdif_i (
		.clk(wb_clk_i),
		.rst(wb_rst_i),
		.enable(ctrl_en),
		.rate(rate),
		.frame_req(frame_req),
		.sample_l(src_l),
		.sample_r(src_r),
		.fs_code(spdif_fs),
		.spdif(AUD_OPTICAL)
	);
`else
	assign AUD_OPTICAL = 1'b0;
`endif


`ifdef AUDIO_MIXER
	audio_mixer #(.CH_BITS(`AUDIO_MIXER_CH_BITS)) mixer_i (
		.clk(wb_clk_i),
		.rst(wb_rst_i),
		.frame_req(frame_req),
		.cfg_we(cfg_we_r),
		.cfg_ch(cfg_ch_r),
		.cfg_reg(cfg_reg_r),
		.cfg_dat(cfg_dat_r),
		.mixvol(mixvol),
		.m_adr_o(mx_adr_o),
		.m_dat_o(mx_dat_o),
		.m_dat_i(mx_dat_i),
		.m_we_o(mx_we_o),
		.m_sel_o(mx_sel_o),
		.m_stb_o(mx_stb_o),
		.m_cyc_o(mx_cyc_o),
		.m_ack_i(mx_ack_i),
		.out_l(mix_l),
		.out_r(mix_r),
		.active_o(mix_active),
		.dbg_adr_o(mix_dbg_adr),
		.dbg_dat_o(mix_dbg_dat)
	);
`else
	assign mx_adr_o = 32'h0000_0000;
	assign mx_dat_o = 32'h0000_0000;
	assign mx_we_o = 1'b0;
	assign mx_sel_o = 4'b0000;
	assign mx_stb_o = 1'b0;
	assign mx_cyc_o = 1'b0;
`endif

	audio_out #() out_i (
		.clk(wb_clk_i),
		.rst(wb_rst_i),
		.enable(ctrl_en),
		.rate(rate),
		.swap_lr(ctrl_swap),
		.sample_l(src_l),
		.sample_r(src_r),
		.sample_valid(src_valid),
		.frame_req(frame_req),
		.sd_l(sd_l),
		.sd_r(sd_r),
		.pt_bck(AUD_BCK),
		.pt_ws(AUD_WS),
		.pt_din(AUD_DIN)
	);

	// ONE always block owns wptr, rptr and the FIFO memory, because a
	// wishbone write and a frame_req pop can land on the same edge and
	// both touch them. Splitting the bus side from the playback side
	// would be two drivers on the same registers.
	always @(posedge wb_clk_i) begin

		if (wb_rst_i) begin

			wptr <= 0;
			rptr <= 0;
			head <= 32'h0000_0000;
			head_valid <= 1'b0;
			ram_q <= 32'h0000_0000;
			ram_ready <= 1'b0;
			ctrl <= CTRL_RESET;
			rate <= RATE_RESET;
			mixvol <= 8'd128;
			spdif_fs <= 4'b0100;   // "48 kHz"
			wmark <= DEPTH / 2;
			underrun <= 1'b0;
			wb_ack_o <= 1'b0;
			wb_dat_o <= 32'h0000_0000;
			cfg_we_r <= 1'b0;
			cfg_ch_r <= 3'd0;
			cfg_reg_r <= 3'd0;
			cfg_dat_r <= 32'h0000_0000;

		end else begin

			// see the declaration: this is the pipeline stage that
			// keeps the bus address net out of the mixer's register
			// file
			cfg_we_r <= cfg_hit;
			cfg_ch_r <= cfg_ch;
			cfg_reg_r <= cfg_reg;
			cfg_dat_r <= wb_dat_i;

			wb_ack_o <= 1'b0;

			// -- block RAM read pipeline --
			//
			// ram_q trails fifo[rptr] by one cycle, unconditionally.
			// ram_ready says whether it can be believed: it is cleared
			// whenever rptr moves (the address just changed) or a
			// write happens (which might have been to rptr itself, and
			// the array's same-cycle read/write behaviour is not
			// something to rely on). One cycle of extra latency after
			// either event, against a frame period of 1088 cycles at
			// the default rate, so this costs nothing that matters.
			ram_q <= fifo[rptr[DEPTH_LOG2-1:0]];
			ram_ready <= 1'b1;

			// -- playback side --
			//
			// GATED ON EN, all of it. The output stage's clocks keep
			// running when EN is clear (a PT8211 with a stopped bit
			// clock is in an undefined state, not a quiet one -- see
			// audio_out.v), but the FIFO must NOT drain while muted.
			//
			// It used to, and that was wrong in two ways. Pausing
			// threw away everything buffered, so resuming restarted
			// from a hole; and a muted FIFO drained to empty and set
			// UNDERRUN, so the sticky bit said a fault had occurred
			// when nothing had. Both showed up as soon as the FIFO
			// got big enough for the drain to outpace a fill loop.
			if (ctrl_en && !ctrl_mixen) begin

				if (frame_req) begin
					if (!head_valid)
						underrun <= 1'b1;
					else
						head_valid <= 1'b0;
				end

				// Refill the output register from the array. Written
				// after the pop above so both can happen on one edge:
				// head_valid is set again in the cycle after
				// frame_req, which is the whole point of the register.
				if (!head_valid || frame_req) begin
					if (!ram_empty && ram_ready) begin
						head <= ram_q;
						head_valid <= 1'b1;
						rptr <= rptr + 1;
						ram_ready <= 1'b0;
					end
				end

			end

			// -- bus side --

			if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin

				wb_ack_o <= 1'b1;

				if (wb_we_i) begin

					// Byte lane 0 only, for CTRL and RATE: both are
					// eight bits wide and everything above them is
					// reserved, so there is nothing for the upper
					// lanes to write. WMARK below is sixteen bits and
					// honours two lanes, and DATA is a whole word by
					// definition. Same principle as rtl/socctl.v --
					// a byte store that silently wrote all four lanes
					// would be a genuinely puzzling bug.
					if (wb_adr_i == 32'd1) begin
						if (wb_sel_i[0]) begin

							// Bits 3 and 4 are commands, not state.
							// Masked out of what gets STORED rather
							// than stored and self-cleared next cycle:
							// a stored command bit is visible to a
							// read that happens to land in that cycle,
							// and FLUSH reading back as 1 would be an
							// alarming thing to see in a register
							// dump for something that already happened.
							ctrl <= wb_dat_i[7:0] & 8'hE7;

							// FLUSH. Assigned after the frame_req
							// branch above, so it wins if both land on
							// the same edge -- emptying a FIFO that
							// was mid-pop is exactly what was asked
							// for.
							if (wb_dat_i[3]) begin
								wptr <= 0;
								rptr <= 0;
								head_valid <= 1'b0;
								ram_ready <= 1'b0;
							end

							if (wb_dat_i[4])
								underrun <= 1'b0;

						end
					end

					// DATA. Dropped when full -- see this file's
					// header for why this does not stall instead.
					if (wb_adr_i == 32'd3) begin
						if (!full) begin
							fifo[wptr[DEPTH_LOG2-1:0]] <= wb_dat_i;
							wptr <= wptr + 1;
							// see the read pipeline above: the write
							// may have been to the address ram_q was
							// just latched from
							ram_ready <= 1'b0;
						end
					end

					if (wb_adr_i == 32'd4) begin
						if (wb_sel_i[0]) rate <= wb_dat_i[7:0];
					end

					if (wb_adr_i == 32'd5) begin
						if (wb_sel_i[0]) wmark[7:0] <= wb_dat_i[7:0];
						if (wb_sel_i[1]) wmark[15:8] <= wb_dat_i[15:8];
					end

					if (wb_adr_i == 32'd8) begin
						if (wb_sel_i[0]) mixvol <= wb_dat_i[7:0];
						if (wb_sel_i[1]) spdif_fs <= wb_dat_i[11:8];
					end

				end else begin

					case (wb_adr_i)
						32'd0: wb_dat_o <= MAGIC;
						32'd1: wb_dat_o <= { 24'b0, ctrl };
						32'd2: wb_dat_o <= {
							12'b0,
							underrun,
							below,
							full,
							empty,
							level_wide
						};
						32'd4: wb_dat_o <= { 24'b0, rate };
						32'd5: wb_dat_o <= { 16'b0, wmark };
						// bit 12 says the hardware mixer is BUILT.
						// Software cannot infer this from anything
						// else: MIXSTAT reads 0 both when the mixer is
						// absent and when it is present with every
						// channel idle, so a build without it used to
						// report "mixing in HARDWARE" and then play
						// silence.
						32'd6: wb_dat_o <= {
							CONFIG_SIG, 3'b0, HAS_MIXER, FORMATS,
							DEPTH_LOG2_B
						};
						32'd7: wb_dat_o <= CLK_HZ;
						32'd8: wb_dat_o <= { 20'b0, spdif_fs, mixvol };
						32'd9: wb_dat_o <= { 24'b0, mix_active };
						// bus read probe -- see audio_mixer.v
						32'd10: wb_dat_o <= mix_dbg_adr;
						32'd11: wb_dat_o <= mix_dbg_dat;
						default: wb_dat_o <= 32'h0000_0000;
					endcase

				end

			end

		end

	end

endmodule
