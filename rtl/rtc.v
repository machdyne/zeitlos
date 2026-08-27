/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * RTC -- a free-running wall-clock counter: seconds since the Unix
 * epoch, plus a 1/1024s sub-second field, settable by software.
 *
 * -- Why this exists, given rtl/sysctl.v already has an "RTC" --
 *
 * It doesn't, despite the name. sysctl.v's `rtc_ctr` is a 16-bit
 * free-running counter whose wrap raises the KTIMER interrupt; what
 * software gets from it (z_uptime_ticks(), sw/common/zeitlos.h) is
 * ticks since boot at ~732Hz, which wraps every ~68 days and has no
 * relationship to any date. That is the right thing for measuring
 * elapsed time and the wrong thing for answering "what time is it",
 * which until now nothing on this SOC could do at all.
 *
 * This block is the other half: a counter with an epoch, which
 * software sets once (sw/apps/net's SNTP client, see docs/rtc.md) and
 * reads whenever it wants a date. It deliberately does NOT replace
 * rtc_ctr or touch the interrupt -- the two answer different
 * questions and both are cheap.
 *
 * -- Not battery-backed --
 *
 * There is no coin cell on any board in this lineup, so this counts
 * from zero at power-on and loses the time when the board is powered
 * down. It survives everything short of that: wb_rst_i comes from
 * sysctl.v's resetn_counter, which releases once after the PLLs lock
 * and never asserts again, so the count is not disturbed by a
 * kernel restart, an app crash, or reloading the OS.
 *
 * That is why VALID (register 3) exists rather than being implied by
 * a nonzero SEC. "The time is 00:00:00 on 1 Jan 1970" and "nobody has
 * told me what time it is" are genuinely different states, and
 * software that shows a clock needs to tell them apart -- displaying
 * 1970 with total confidence is worse than displaying nothing.
 *
 * -- Optional, but on by default --
 *
 * `RTC in rtl/boards.vh, defined at the UNIVERSAL level rather than
 * per-board: this needs no pins, no external part and no board support
 * of any kind, just a prescaler and a counter on sys_clk, so there is
 * no board-specific reason to want it or not want it. Every board gets
 * one unless somebody deliberately comments it out to reclaim the
 * logic.
 *
 * A build without it is not a build software has to be told about. The
 * FEATURE bit in rtl/csrs.v goes clear, rtl/sysctl.v hands this
 * window to csrs.v (which acks it and reads back zero), and
 * z_rtc_available() answers false -- so sw/apps/net skips its NTP
 * client and sw/apps/clock says on screen that this bitstream has no
 * clock. Nothing hangs and nothing needs rebuilding differently.
 *
 * -- Where it lives in the address map --
 *
 * Nibble 0x7, at 0x7000_03xx, as the fourth tenant alongside
 * rtl/csrs.v (0x7000_00xx), rtl/cache.v (0x7000_01xx) and
 * rtl/socctl.v (0x7000_02xx). Same reasoning socctl.v's own header
 * gives for not taking a top nibble of its own: sysctl.v's map is
 * full, and the one free nibble (0x8) is the virtual window apps
 * execute in, where a stale app pointer dereferenced in kernel
 * context would land on control registers.
 *
 * The window MUST stay decoded whether or not this block is built,
 * which is why sysctl.v gives it to csrs.v when `RTC is off rather
 * than simply dropping the term. An address nothing decodes gets no
 * ack at all on this bus and the CPU waits for it forever -- see
 * rtl/cache.v's own note on the same hazard, and sysctl.v's cs_csrs
 * comment for how the rule is expressed once for both optional
 * tenants.
 *
 * -- Register map --
 *
 * Word-addressed: wb_adr_i here is the low 3 bits of rtl/sysctl.v's
 * wbm_adr_sel_word, so register n is at byte address
 * 0x7000_0300 + 4n. Same convention as every other simple slave in
 * this codebase (rtl/debug.v, rtl/csrs.v, rtl/socctl.v).
 *
 *   0  MAGIC  ro  fixed 32'h5A52_5443 ("ZRTC"). Check this before
 *                 trusting anything else here, for exactly the reason
 *                 csrs.v's own MAGIC exists: an unmapped read does not
 *                 fault on this bus (see sysctl.v's 32'hzzzz_zzzz
 *                 default), so a known constant is the only way
 *                 software can tell "this block is present" from
 *                 "this is whatever the bus resolved to".
 *
 *   1  SEC    rw  read:  seconds since the Unix epoch, UTC.
 *                 write: sets it. Also loads SUB from whatever was
 *                        last written to register 2, restarts the
 *                        prescaler, and sets VALID.
 *
 *                 A write is the ONLY thing that ever moves this
 *                 backwards or by more than one, so software setting
 *                 the clock is the only source of a discontinuity --
 *                 there is no drift correction, slewing or leap
 *                 second handling in here. See docs/rtc.md.
 *
 *   2  SUB    rw  read:  sub-second counter, 0..RATE-1, live.
 *                 write: the value a subsequent SEC write will load.
 *                        Cleared back to 0 once that write consumes
 *                        it, so a later bare SEC write starts the
 *                        second cleanly rather than inheriting a
 *                        fraction from minutes ago.
 *
 *                 The two-step (write SUB, then write SEC) is what
 *                 makes setting the clock atomic at sub-second
 *                 resolution from a bus that can only carry 32 bits
 *                 at a time. An NTP client knows both halves of the
 *                 timestamp and would otherwise have to either throw
 *                 the fraction away or busy-wait for a second
 *                 boundary; see sw/apps/net/ntp.c.
 *
 *   3  CTRL   rw  bit 0: VALID -- set by a SEC write, cleared only by
 *                 reset or by writing bit 0 as 0. Nothing in the
 *                 hardware consults it; it exists purely so software
 *                 can distinguish "set" from "counting since
 *                 power-on" (see above). Bits 31:1 read 0.
 *
 *   4  RATE   ro  sub-second ticks per second (1024). Read rather
 *                 than assumed, so software that divides by it keeps
 *                 working if this block is ever rebuilt at a
 *                 different resolution.
 *
 *   5  TZ     rw  RESERVED -- 16 bits of signed storage, intended for
 *                 a local time offset from UTC in MINUTES east,
 *                 sign-extended on read (so -60 reads back as
 *                 32'hFFFF_FFC4, and a C int32_t sees -60). Minutes
 *                 rather than hours because several real zones are
 *                 not whole hours off UTC.
 *
 *                 NOTHING CURRENTLY USES IT. Zeitlos is UTC
 *                 throughout for now -- see sw/common/zrtc.h's own
 *                 "no timezones" note for why that is a decision
 *                 rather than an omission, and docs/rtc.md.
 *
 *                 It is implemented rather than left out so the slot
 *                 exists in already-flashed gateware when a timezone
 *                 story does arrive; adding it later would mean a
 *                 reflash for what is otherwise a software change.
 *                 This block never consults it either way -- the
 *                 clock is UTC and stays UTC regardless of what is
 *                 written here.
 *
 *   6,7        -  reserved, read 0.
 *
 * -- Reading SEC and SUB together --
 *
 * There is deliberately no side effect on read: nothing here latches
 * a snapshot, and both registers are live. A reader that wants a
 * consistent pair therefore has to notice the case where the second
 * rolls over between the two reads, which it does by reading SEC,
 * SUB, then SEC again and retrying if the two SEC values differ.
 * z_rtc_get() (sw/common/zrtc.h) is that loop and every caller should
 * use it.
 *
 * The alternative -- have a SEC read latch SUB into a shadow -- was
 * considered and rejected. It makes reads order-dependent in a way
 * nothing else on this bus is, and it breaks the moment two processes
 * interleave their reads, which on this system they can: there is no
 * arbitration of peripheral registers between processes (see
 * docs/gpu_blitter.md's own note on the same hazard). A retry loop in
 * software has neither problem and costs one extra load in the
 * overwhelmingly common case where the second did not roll over.
 */

module rtc_wb #(
	// System clock, Hz. rtl/sysctl.v passes its own SYSCLK. The
	// prescaler below is CLK_HZ / SUBSEC_HZ, which is exact at 48MHz
	// (46875 x 1024 = 48,000,000) -- that exactness is why SUBSEC_HZ
	// is 1024 and not, say, 1000. At a clock where it does not divide
	// evenly the truncation shows up as a constant rate error; at
	// 48MHz there is none.
	parameter CLK_HZ = 48_000_000
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
	input wb_cyc_i
);

	localparam MAGIC = 32'h5A52_5443;	// "ZRTC"

	// Sub-second resolution. A power of two on purpose: it divides
	// 48MHz exactly (see CLK_HZ above), and it lets an NTP fraction
	// -- which is a binary fraction of a second -- convert with a
	// shift instead of a divide (sw/apps/net/ntp.c).
	localparam SUBSEC_HZ = 1024;
	localparam SUB_MAX   = SUBSEC_HZ - 1;

	// Clock cycles per sub-second tick. 46875 at 48MHz, which needs
	// 16 bits; 24 here so a slower or faster CLK_HZ doesn't silently
	// truncate the compare value.
	localparam [23:0] DIV_MAX = (CLK_HZ / SUBSEC_HZ) - 1;

	reg [23:0] div_ctr;
	reg [9:0] subsec;
	reg [31:0] sec;
	reg [9:0] sub_preload;
	reg valid;
	reg [15:0] tz_min;

	always @(posedge wb_clk_i) begin

		if (wb_rst_i) begin

			div_ctr <= 24'd0;
			subsec <= 10'd0;
			sec <= 32'd0;
			sub_preload <= 10'd0;
			// Counting, but nobody has said from when -- see this
			// file's header comment on why that is its own state.
			valid <= 1'b0;
			tz_min <= 16'd0;
			wb_ack_o <= 1'b0;
			wb_dat_o <= 32'd0;

		end else begin

			// -- the time base --
			//
			// Runs unconditionally, every cycle, independent of any
			// bus activity. A SEC write in the same cycle overrides
			// what this does to sec/subsec/div_ctr, because the write
			// branch below assigns them later in the same always
			// block and the last nonblocking assignment wins. That
			// ordering is the whole mechanism for "setting the clock
			// restarts the second cleanly" -- without it a set could
			// land a tick short or long depending on where the
			// prescaler happened to be.
			if (div_ctr == DIV_MAX) begin
				div_ctr <= 24'd0;
				if (subsec == SUB_MAX[9:0]) begin
					subsec <= 10'd0;
					sec <= sec + 1;
				end else begin
					subsec <= subsec + 1;
				end
			end else begin
				div_ctr <= div_ctr + 1;
			end

			wb_ack_o <= 1'b0;

			if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin

				wb_ack_o <= 1'b1;

				if (wb_we_i) begin

					case (wb_adr_i[2:0])

						// SEC: byte lanes honoured like every other
						// writable register in this codebase
						// (rtl/mtu.v, rtl/socctl.v), though software
						// has no reason to write this a byte at a
						// time and a partial write is not a
						// meaningful way to set a clock -- the
						// side effects below happen either way.
						3'd1: begin
							if (wb_sel_i[0]) sec[7:0]   <= wb_dat_i[7:0];
							if (wb_sel_i[1]) sec[15:8]  <= wb_dat_i[15:8];
							if (wb_sel_i[2]) sec[23:16] <= wb_dat_i[23:16];
							if (wb_sel_i[3]) sec[31:24] <= wb_dat_i[31:24];
							// Adopt the fraction staged by a previous
							// SUB write and restart the prescaler, so
							// the new second begins exactly here.
							subsec <= sub_preload;
							div_ctr <= 24'd0;
							// Consumed -- a later bare SEC write
							// starts at .000 rather than inheriting
							// this fraction.
							sub_preload <= 10'd0;
							valid <= 1'b1;
						end

						// SUB: stages a fraction for the next SEC
						// write. Only lane 0 and the low two bits of
						// lane 1 exist, so the upper lanes have
						// nothing to write. Values >= SUBSEC_HZ
						// cannot be expressed at all, which is the
						// range check.
						3'd2: begin
							if (wb_sel_i[0]) sub_preload[7:0] <= wb_dat_i[7:0];
							if (wb_sel_i[1]) sub_preload[9:8] <= wb_dat_i[9:8];
						end

						// CTRL: VALID is writable so software can say
						// "the time I set is no longer trustworthy"
						// -- e.g. a sync that came back with a
						// nonsense timestamp. Nothing sets it except
						// this and a SEC write.
						3'd3: begin
							if (wb_sel_i[0]) valid <= wb_dat_i[0];
						end

						// TZ: 16 bits of signed minutes. Every value
						// is legal (there is no range this could
						// usefully reject that software could not
						// reject better, and a rejected write here
						// would be invisible -- this bus carries no
						// error).
						3'd5: begin
							if (wb_sel_i[0]) tz_min[7:0]  <= wb_dat_i[7:0];
							if (wb_sel_i[1]) tz_min[15:8] <= wb_dat_i[15:8];
						end

						default: begin
							// Reserved or read-only -- silently
							// ignored rather than treated as an
							// error, matching this bus's usual
							// behaviour for an unhandled access.
						end

					endcase

				end else begin

					case (wb_adr_i[2:0])
						3'd0: wb_dat_o <= MAGIC;
						3'd1: wb_dat_o <= sec;
						3'd2: wb_dat_o <= { 22'd0, subsec };
						3'd3: wb_dat_o <= { 31'd0, valid };
						3'd4: wb_dat_o <= SUBSEC_HZ;
						// sign-extended, so a C int32_t read of a
						// negative offset is already correct without
						// software doing the extension itself
						3'd5: wb_dat_o <= { {16{tz_min[15]}}, tz_min };
						default: wb_dat_o <= 32'd0;
					endcase

				end

			end

		end

	end

endmodule
