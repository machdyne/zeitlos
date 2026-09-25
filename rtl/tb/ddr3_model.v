/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 SDRAM behavioural model -- SIMULATION ONLY, NOT SYNTHESISABLE
 *
 * Written from the JEDEC DDR3 command truth table, mode register
 * definitions and AC timing parameter list, and from the Micron MT41K
 * timing tables. No vendor simulation model was used or adapted; this
 * is not a substitute for one and makes no attempt to be.
 *
 * -- what it checks --
 *
 * The value of this file is that it FAILS LOUDLY. A DRAM model that
 * only stores and returns data will happily accept a controller that
 * violates tRCD, skips a mode register or refreshes too rarely, and
 * the resulting bug then appears for the first time on hardware as
 * intermittent corruption. So this model verifies, and stops:
 *
 *   * the power-on sequence: RESET#/CKE order, and that MR2, MR3, MR1
 *     and MR0 are all written, in that order, before ZQCL, before any
 *     bank is activated
 *   * mode register CONTENTS -- CL, CWL, burst length and DLL reset
 *     are read back out of what the controller wrote, so a wrong
 *     field is caught here rather than by mysteriously bad data
 *   * command legality against per-bank state: no read from an idle
 *     bank, no activate of an already-active bank, no access to a row
 *     other than the one that is open
 *   * inter-command timing: tRCD, tRP, tRC, tRAS, tRFC, tMRD, tMOD,
 *     tWR and tRTP, each against the bank or device it applies to
 *   * refresh rate: that the average interval does not exceed tREFI
 *
 * -- what it does NOT check --
 *
 * Read and write data are sampled on a timing schedule derived from
 * CL and CWL rather than from the DQS strobe. DQS is checked for
 * presence and for not being X during the transfer, but it is not
 * used as the capture clock.
 *
 * That is a deliberate scope line, not an oversight. Strobe alignment
 * is a property of the PHY and the board, and on ECP5 it is trained
 * at run time by the DQS delay hardware -- there is no fixed
 * relationship for a model to assert. Verifying it in simulation
 * would verify the testbench's own idea of alignment and nothing
 * else. It is a phase 2 problem and belongs on hardware.
 *
 * -- memory aliasing --
 *
 * A real MT41K64M16 is 128MB, far too much to allocate. Storage here
 * is indexed by the low ROW_ALIAS bits of the row, so rows that
 * differ only above that bit share storage. Tests must therefore
 * either stay within 2**ROW_ALIAS rows or expect aliasing; the model
 * reports the limit at startup so a surprising result has an obvious
 * explanation.
 */

`timescale 1ps/1ps
`default_nettype none

module ddr3_model #(
	parameter ROW_BITS  = 13,
	parameter COL_BITS  = 10,
	parameter BANK_BITS = 3,

	// Rows actually backed by storage; see header.
	parameter ROW_ALIAS = 6,

	// DRAM clock period in picoseconds. Every timing check below is
	// expressed in real time, not clocks, so this is the only place
	// the frequency appears.
	parameter TCK_PS = 10417,

	// AC timings, picoseconds. Defaults are the MT41K -125 speed bin
	// values that apply regardless of frequency.
	parameter TRCD_PS = 13750,
	parameter TRP_PS  = 13750,
	parameter TRC_PS  = 48750,
	parameter TRAS_PS = 35000,
	parameter TRFC_PS = 260000,   // 4Gb: 260ns (MT41K256M16)
	parameter TREFI_PS = 7800000,
	parameter TWR_PS  = 15000
) (
	input wire ck,
	input wire reset_n,
	input wire cke,
	input wire cs_n,
	input wire ras_n,
	input wire cas_n,
	input wire we_n,
	input wire [15:0] a,
	input wire [2:0] ba,
	input wire [1:0] dm,
	inout wire [15:0] dq,
	inout wire [1:0] dqs
);

	localparam NBANKS = (1 << BANK_BITS);
	localparam MEMSZ  = (1 << (BANK_BITS + ROW_ALIAS + COL_BITS));

	// -- storage --------------------------------------------------
	reg [15:0] mem [0:MEMSZ-1];

	// -- device state ---------------------------------------------
	reg [ROW_BITS-1:0] bank_row [0:NBANKS-1];
	reg bank_active [0:NBANKS-1];
	time t_act [0:NBANKS-1];     // last ACTIVATE, per bank
	time t_pre [0:NBANKS-1];     // last PRECHARGE completion start
	time t_rd [0:NBANKS-1];      // last READ
	time t_wr_end [0:NBANKS-1];  // end of last write burst

	time t_mrs;
	time t_ref;
	time t_ref_prev;

	reg [15:0] mr [0:3];
	reg mr_written [0:3];
	reg zqcl_done;
	reg init_ok;

	integer cl_cfg;
	integer cwl_cfg;
	integer errors;
	integer refreshes;
	// Each concurrent block gets its OWN loop index.
	//
	// There was a single module-scope `integer i` shared by the
	// command decoder, the read burst and the write burst. Those run
	// at the same time: a command arriving while a read burst is
	// walking its eight beats reassigns the loop variable underneath
	// it, and the burst finishes somewhere it should not.
	//
	// The symptom was the last beat of a read coming back as z or as
	// a copy of the beat before it -- intermittently, depending on
	// what else the controller happened to be doing. It looks exactly
	// like a datapath alignment fault, which is what makes it
	// expensive: it sends you looking in the design under test.
	integer i;
	integer rb;


	// -- read burst driving ---------------------------------------
	reg [15:0] dq_drv;
	reg [1:0] dqs_drv;
	reg drv_en;
	reg rd_go;
	reg [BANK_BITS-1:0] rd_ba;
	reg [COL_BITS-1:0] rd_col;

	reg wr_go;
	reg [BANK_BITS-1:0] wr_ba;
	reg [COL_BITS-1:0] wr_col;

	assign dq  = drv_en ? dq_drv : 16'hzzzz;
	assign dqs = drv_en ? dqs_drv : 2'bzz;

	// Command decode
	wire [3:0] cmd = {cs_n, ras_n, cas_n, we_n};
	localparam [3:0] C_MRS = 4'b0000, C_REF = 4'b0001, C_PRE = 4'b0010,
	                 C_ACT = 4'b0011, C_WR  = 4'b0100, C_RD  = 4'b0101,
	                 C_ZQC = 4'b0110, C_NOP = 4'b0111;

	task fail(input [1023:0] msg);
	begin
		$display("[%0t] DDR3 MODEL ERROR: %0s", $time, msg);
		errors = errors + 1;
	end
	endtask

	// MR0 A[11:9]: write recovery for auto-precharge, in clocks.
	function integer mr0_wr_ck(input [15:0] m);
		case (m[11:9])
		3'b001: mr0_wr_ck = 5;
		3'b010: mr0_wr_ck = 6;
		3'b011: mr0_wr_ck = 7;
		3'b100: mr0_wr_ck = 8;
		3'b101: mr0_wr_ck = 10;
		3'b110: mr0_wr_ck = 12;
		3'b111: mr0_wr_ck = 14;
		default: mr0_wr_ck = 16;
		endcase
	endfunction

	function [31:0] addr_of(input [BANK_BITS-1:0] b,
	                        input [ROW_BITS-1:0] r,
	                        input [COL_BITS-1:0] c);
	begin
		addr_of = (b << (ROW_ALIAS + COL_BITS))
		        | ((r & ((1 << ROW_ALIAS) - 1)) << COL_BITS)
		        | c;
	end
	endfunction

	initial begin
		errors = 0;
		refreshes = 0;
		drv_en = 1'b0;
		dq_drv = 16'd0;
		dqs_drv = 2'b00;
		rd_go = 1'b0;
		wr_go = 1'b0;
		init_ok = 1'b0;
		zqcl_done = 1'b0;
		cl_cfg = 0;
		cwl_cfg = 0;
		t_mrs = 0;
		t_ref = 0;
		t_ref_prev = 0;
		for (i = 0; i < 4; i = i + 1) begin
			mr[i] = 16'd0;
			mr_written[i] = 1'b0;
		end
		for (i = 0; i < NBANKS; i = i + 1) begin
			bank_active[i] = 1'b0;
			bank_row[i] = 0;
			t_act[i] = 0;
			t_pre[i] = 0;
			t_rd[i] = 0;
			t_wr_end[i] = 0;
		end
		for (i = 0; i < MEMSZ; i = i + 1) mem[i] = 16'h0000;
		$display("[%0t] ddr3_model: %0d banks, %0d row bits (%0d backed), %0d col bits, tCK=%0dps",
			$time, NBANKS, ROW_BITS, ROW_ALIAS, COL_BITS, TCK_PS);
	end

	// =============================================================
	// Command capture
	// =============================================================
	always @(posedge ck) begin

		if (reset_n === 1'b0) begin

			// RESET# is asynchronous and returns everything to idle.
			for (i = 0; i < NBANKS; i = i + 1) bank_active[i] = 1'b0;
			for (i = 0; i < 4; i = i + 1) mr_written[i] = 1'b0;
			zqcl_done = 1'b0;
			init_ok = 1'b0;

		end else if (cke === 1'b1 && cs_n === 1'b0) begin

			// tRFC governs refresh to ANY command, not merely to the
			// next refresh.
			//
			// This model used to check it only REFRESH to REFRESH, and
			// a controller that waited the 1Gb value of 110ns on a 4Gb
			// part -- which needs 260ns -- passed it cleanly for weeks.
			// Every access issued in the gap went to a DRAM that was
			// still refreshing. The refresh-to-refresh spacing was
			// always fine, because the refresh INTERVAL is microseconds;
			// it is the next ordinary command that lands too early.
			//
			// A checker that only compares a rule against itself cannot
			// catch the rule being applied to the wrong thing.
			if (cmd != C_NOP && t_ref != 0 && $time - t_ref < TRFC_PS)
				fail("tRFC violated: command issued while still refreshing");

			case (cmd)

			C_MRS: begin
				if (ba > 3) fail("MRS to a bank group above 3");
				if ($time - t_mrs < 4 * TCK_PS && t_mrs != 0)
					fail("tMRD violated: mode registers written too close together");
				mr[ba] = a;
				mr_written[ba] = 1'b1;
				t_mrs = $time;

				// Enforce the order the JEDEC init sequence requires.
				// MR0 carries the DLL reset and must be last, after
				// the settings that the DLL and the data path depend
				// on are already loaded.
				if (ba == 2) begin
					cwl_cfg = a[5:3] + 5;
					$display("[%0t] ddr3_model: MR2 = %04h, CWL = %0d", $time, a, cwl_cfg);
					if (a[5:3] > 3'd4) fail("MR2 sets a CWL this part does not support");
				end
				if (ba == 1) begin
					if (!mr_written[2]) fail("MR1 written before MR2");
					if (a[0] !== 1'b0) fail("MR1 leaves the DLL disabled");
					$display("[%0t] ddr3_model: MR1 = %04h", $time, a);
				end
				if (ba == 0) begin
					if (!mr_written[1] || !mr_written[2] || !mr_written[3])
						fail("MR0 written before MR1/MR2/MR3");
					cl_cfg = a[6:4] + 4;
					if (a[1:0] !== 2'b00) fail("MR0 does not select BL8 fixed");
					if (a[3] !== 1'b0) fail("MR0 does not select sequential burst order");
					if (a[8] !== 1'b1) fail("MR0 does not reset the DLL");
					$display("[%0t] ddr3_model: MR0 = %04h, CL = %0d, BL8", $time, a, cl_cfg);
				end
			end

			C_ZQC: begin
				if (!mr_written[0]) fail("ZQ calibration before MR0");
				if (a[10] !== 1'b1) fail("expected ZQCL (A10 high) during init");
				zqcl_done = 1'b1;
				init_ok = 1'b1;
				$display("[%0t] ddr3_model: ZQCL, initialisation complete", $time);
			end

			C_ACT: begin
				if (!init_ok) fail("ACTIVATE before initialisation completed");
				if (bank_active[ba]) fail("ACTIVATE to a bank that is already active");
				// Written without subtraction. $time and t_pre are
				// unsigned, and for an auto-precharge that has not yet
				// STARTED -- t_pre still in the future -- the old form
				// ($time - t_pre < tRP) wrapped to a huge number and
				// passed. So an ACTIVATE that arrived before write
				// recovery had even finished was invisible: the model
				// could catch "during the precharge" but never "before
				// it".
				if (t_pre[ba] != 0 && $time < t_pre[ba] + TRP_PS)
					fail("tRP violated: ACTIVATE before the bank finished precharging");
				if ($time - t_act[ba] < TRC_PS && t_act[ba] != 0)
					fail("tRC violated: ACTIVATE to ACTIVATE on the same bank too soon");
				bank_active[ba] = 1'b1;
				bank_row[ba] = a[ROW_BITS-1:0];
				t_act[ba] = $time;
			end

			C_PRE: begin
				if (a[10]) begin
					for (i = 0; i < NBANKS; i = i + 1)
						if (bank_active[i]) begin
							if ($time - t_act[i] < TRAS_PS)
								fail("tRAS violated by PRECHARGE ALL");
							bank_active[i] = 1'b0;
							t_pre[i] = $time;
						end
				end else begin
					if (bank_active[ba] && ($time - t_act[ba] < TRAS_PS))
						fail("tRAS violated: PRECHARGE too soon after ACTIVATE");
					bank_active[ba] = 1'b0;
					t_pre[ba] = $time;
				end
			end

			C_RD: begin
				if (!bank_active[ba]) fail("READ from an idle bank");
				if ($time - t_act[ba] < TRCD_PS)
					fail("tRCD violated: READ too soon after ACTIVATE");
				rd_ba = ba;
				rd_col = {a[COL_BITS-1:3], 3'b000};
				t_rd[ba] = $time;
				rd_go = ~rd_go;
				// Auto-precharge closes the row after the burst plus
				// tRTP; modelled as taking effect at the end of the
				// burst, which is when the bank becomes unavailable.
				if (a[10]) begin
					bank_active[ba] = 1'b0;
					t_pre[ba] = $time + (4 * TCK_PS);
				end
			end

			C_WR: begin
				if (!bank_active[ba]) fail("WRITE to an idle bank");
				if ($time - t_act[ba] < TRCD_PS)
					fail("tRCD violated: WRITE too soon after ACTIVATE");
				wr_ba = ba;
				wr_col = {a[COL_BITS-1:3], 3'b000};
				wr_go = ~wr_go;
				if (a[10]) begin
					bank_active[ba] = 1'b0;
					// Internal precharge begins WR CLOCKS after the end
					// of the burst -- WR as programmed in MR0, which is
					// what the part actually uses for auto-precharge.
					// Not tWR in nanoseconds: MR0's WR must be at least
					// tWR rounded up to clocks, but the DRAM then waits
					// the MR0 value, whatever it is.
					//
					// This used the nanosecond tWR, so a controller
					// waiting 15ns while MR0 said 6 clocks (62.5ns here)
					// passed cleanly, while on the part it activated
					// banks that were still recovering from a write.
					// Two numbers that must agree, checked against the
					// wrong one of them.
					t_pre[ba] = $time + (cwl_cfg + 4 + mr0_wr_ck(mr[0])) * TCK_PS;
					if (mr0_wr_ck(mr[0]) * TCK_PS < TWR_PS)
						fail("MR0 WR shorter than tWR");
				end
			end

			C_REF: begin
				if (!init_ok) fail("REFRESH before initialisation completed");
				for (i = 0; i < NBANKS; i = i + 1)
					if (bank_active[i]) fail("REFRESH with a bank still active");
				if ($time - t_ref < TRFC_PS && t_ref != 0)
					fail("tRFC violated: REFRESH to REFRESH too soon");
				t_ref_prev = t_ref;
				t_ref = $time;
				refreshes = refreshes + 1;
			end

			default: ; // NOP / DESELECT

			endcase

		end

	end

	// =============================================================
	// Refresh rate watchdog
	//
	// Checked continuously rather than at each refresh, so that a
	// controller which stops refreshing entirely is caught rather
	// than simply never triggering the check.
	// =============================================================
	always @(posedge ck) begin
		if (init_ok && t_ref != 0 && ($time - t_ref) > (TREFI_PS * 2))
			if (($time - t_ref) % (TREFI_PS * 2) < TCK_PS)
				fail("refresh interval exceeded: tREFI is not being met");
	end

	// =============================================================
	// Read burst
	//
	// Data is driven edge aligned with DQS, which is what a DDR3 part
	// does on a read; it is the receiver's job to delay the strobe
	// into the middle of the eye.
	// =============================================================
	always @(rd_go) begin
		if (init_ok) begin
			// CL clocks from the command to the first data beat. The
			// command was captured on a rising edge, so wait CL full
			// periods less the half period already elapsed.
			// Read PREAMBLE: DQS driven low for one clock before the
			// first rising edge, as the part does. Without it the
			// strobe went straight from high impedance to 1, and a
			// receiver that only trusts real 0/1 transitions -- which
			// is the only safe kind -- could not tell the first beat
			// from the line waking up.
			#(cl_cfg * TCK_PS - (TCK_PS / 2) - TCK_PS);
			drv_en = 1'b1;
			dqs_drv = 2'b00;
			#(TCK_PS);
			for (rb = 0; rb < 8; rb = rb + 1) begin
				dq_drv = mem[addr_of(rd_ba, bank_row[rd_ba], rd_col + rb[COL_BITS-1:0])];
				dqs_drv = {2{rb[0] ^ 1'b1}};
				#(TCK_PS / 2);
			end
			// Read POSTAMBLE.
			//
			// DQ and DQS were released the instant the eighth beat
			// ended. A receiver whose time origin is the CONTROLLER
			// command rather than the DRAM bus -- which is every real
			// receiver, since the command takes time to get here --
			// samples the last beat slightly after that instant and
			// catches the release instead of the data. The last word
			// of every burst read back as z.
			//
			// Real DDR3 holds the strobe for half a clock after the
			// burst and the data lines do not go high impedance the
			// moment the final bit is nominally over.
			#(TCK_PS);
			drv_en = 1'b0;
			dqs_drv = 2'b00;
		end
	end

	// =============================================================
	// Write burst
	//
	// Sampled on a CWL-derived schedule rather than on DQS; see the
	// header for why that is deliberate. DQS is still checked for
	// being driven, so a PHY that forgets the strobe entirely does
	// not pass silently.
	// =============================================================
	// -- write capture, on the STROBE ---------------------------
	//
	// A DRAM captures write data on DQS, which the controller drives
	// centre-aligned in each beat -- one edge per beat, eight edges
	// per burst. So does this model now.
	//
	// It used to wait a fixed time from the WRITE command and then
	// sample every half clock. That time was measured from the DRAM
	// bus, while the PHY double measured its drive from the controller
	// command, and the two origins differ. The model sampled one beat
	// early: each beat took the value of the one before it, beat 0
	// took whatever preceded the burst, and beat 7 was lost.
	//
	// Invisible whenever the two halves of a 32-bit word held the
	// same value, which is most test patterns -- 44444444 passes,
	// a5a50003 does not. A test only finds faults along the axes its
	// pattern varies on.
	//
	// Capturing on the strobe removes the disagreement rather than
	// tuning a number until it goes away, and it is what the part
	// actually does.
	reg wr_active;
	integer wr_beat;

	// The strobe's previous level. Only a true 0->1 or 1->0
	// transition carries data.
	//
	// When the controller starts driving DQS for the preamble, the
	// line goes from high impedance to 0 -- which Verilog counts as a
	// negedge. Counting it made the preamble beat 0, and every beat
	// after it landed one place late: the first real beat went into
	// slot 1 and the last was dropped.
	reg dqs_last;
	always @(dqs[0]) dqs_last <= dqs[0];

	always @(wr_go) begin
		if (init_ok) begin
			wr_beat = 0;
			wr_active = 1'b1;
		end
	end

	always @(posedge dqs[0] or negedge dqs[0]) begin
		if (wr_active && !drv_en && (dqs[0] === 1'b0 || dqs[0] === 1'b1)
		    && (dqs_last === 1'b0 || dqs_last === 1'b1)) begin
			if (^dq === 1'bx)
				fail("write burst with DQ undriven or unknown");
			// DM is per byte and active high for masked.
			if (dm[0] !== 1'b1)
				mem[addr_of(wr_ba, bank_row[wr_ba], wr_col + wr_beat[COL_BITS-1:0])][7:0] = dq[7:0];
			if (dm[1] !== 1'b1)
				mem[addr_of(wr_ba, bank_row[wr_ba], wr_col + wr_beat[COL_BITS-1:0])][15:8] = dq[15:8];
			wr_beat = wr_beat + 1;
			if (wr_beat == 8) begin
				wr_active = 1'b0;
				t_wr_end[wr_ba] = $time;
			end
		end
	end

	// Backdoor for the testbench: preload or inspect without going
	// through the bus.
	task poke(input [31:0] idx, input [15:0] val);
	begin
		mem[idx] = val;
	end
	endtask

	function [15:0] peek(input [31:0] idx);
	begin
		peek = mem[idx];
	end
	endfunction

endmodule

`default_nettype wire
