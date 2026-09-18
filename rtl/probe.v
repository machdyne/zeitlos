/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * probe.v -- a two-channel logic analyser built into the SoC.
 *
 * Samples two wires every clock (or every DIV+1 clocks), packs sixteen
 * samples to a 32-bit word, and stores them in its own block RAM.
 * Software arms it, the hardware triggers on a signal chosen at build
 * time, and the capture is read back a word at a time over Wishbone.
 *
 * Enabled by `PROBE in rtl/boards.vh. Absent otherwise -- this is a
 * bring-up instrument, not something to carry on every board.
 *
 * -- why it is shaped like this --
 *
 * An earlier attempt at the same thing, built into the USB host, cost
 * ~600 LUT4 in synthesis and then failed to place: 115% TRELLIS_COMB.
 * The memory had a registered auto-incrementing read pointer, and an
 * auto-increment on the read side means the address depends on the bus
 * cycle, which stops it being a plain synchronous read port -- so it
 * did not map to a DP16KD and unrolled into logic.
 *
 * Everything here is arranged to avoid that:
 *
 *   - ONE synchronous write port and ONE synchronous read port, both
 *     with plain registered addresses, which is the shape yosys maps
 *     to a DP16KD without being asked.
 *   - The read address comes from SOFTWARE, in its own register. No
 *     auto-increment, no pointer arithmetic in the read path.
 *   - Packing is a SHIFT REGISTER, not an indexed write into a wide
 *     word. `sh <= {sig, sh[31:2]}` is two flops of work; an indexed
 *     part-select write is a 16-way mux over 32 bits.
 *   - `no_rw_check` for the same reason the USB packet buffer needs
 *     it: without it the inferred memory grows read/write collision
 *     logic that this design cannot produce and does not want.
 *
 * -- capacity --
 *
 * WORDS words is WORDS*16 samples. At 48 MHz with DIV=0 that is
 * WORDS/3 microseconds. The default 512 words gives 8192 samples,
 * 170 us -- and a whole low-speed USB SETUP transaction, token through
 * handshake, is about 125 us. One DP16KD.
 */

module probe #(
	// Capture depth in 32-bit words. Sixteen samples each.
	parameter WORDS = 512,
	parameter AW = 9,
	// Sample every DIV+1 clocks. 0 is full rate.
	parameter DIV = 0
) (
	input wire clk,
	input wire rst,

	// The two wires under observation, already synchronised by
	// whoever instantiates this if they come from a pad.
	input wire [1:0] sig,

	// Capture begins on the rising edge of this, once armed. Tie to
	// 1'b1 to start the moment software arms it.
	input wire trig,

	input wire [1:0] wb_adr_i,
	input wire [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input wire wb_we_i,
	input wire wb_stb_i,
	input wire wb_cyc_i,
	output reg wb_ack_o
);

	localparam R_CTRL = 2'd0;	// W: bit0 arm.  R: running, full, size
	localparam R_ADDR = 2'd1;	// W: word to read back
	localparam R_DATA = 2'd2;	// R: that word

	(* no_rw_check *)
	reg [31:0] mem [0:WORDS-1];

	reg [31:0] sh;
	reg [3:0] sub;
	reg [AW-1:0] wadr;
	reg [AW-1:0] radr;
	reg [31:0] rdat;
	reg [15:0] divc;
	reg armed;
	reg running;
	reg full;
	reg trig_q;

	wire tick = (DIV == 0) ? 1'b1 : (divc == DIV[15:0]);
	wire store = running && tick && (sub == 4'd15);

	// The memory. Nothing clever, deliberately.
	always @(posedge clk) begin
		if (store) mem[wadr] <= {sig, sh[31:2]};
		rdat <= mem[radr];
	end

	always @(posedge clk) begin

		if (rst) begin

			armed <= 1'b0;
			running <= 1'b0;
			full <= 1'b0;
			wadr <= {AW{1'b0}};
			sub <= 4'd0;
			divc <= 16'd0;
			trig_q <= 1'b0;

		end else begin

			trig_q <= trig;

			if (DIV != 0) begin
				if (tick) divc <= 16'd0;
				else divc <= divc + 16'd1;
			end

			// Arm now, capture when the trigger next asserts. A
			// level would start mid-event; the edge puts the start
			// of the thing we care about at word zero.
			if (armed && trig && !trig_q) begin
				armed <= 1'b0;
				running <= 1'b1;
			end

			if (running && tick) begin
				sh <= {sig, sh[31:2]};
				sub <= sub + 4'd1;
				if (sub == 4'd15) begin
					wadr <= wadr + {{(AW-1){1'b0}}, 1'b1};
					// All-ones, NOT (WORDS-1) sliced to AW bits.
					// WORDS[AW-1:0] for 512 with AW=9 is ZERO,
					// and 0-1 promotes to 32 bits as 0xffffffff,
					// which a 9-bit wadr can never equal -- so
					// the capture ran forever, wrapping the
					// buffer, and `full` never set. WORDS is
					// 2**AW by construction, so all-ones is the
					// last word and needs no arithmetic.
					if (wadr == {AW{1'b1}}) begin
						running <= 1'b0;
						full <= 1'b1;
					end
				end
			end

			if (wb_cyc_i && wb_stb_i && wb_we_i && !wb_ack_o) begin
				case (wb_adr_i)
				R_CTRL: if (wb_dat_i[0]) begin
					armed <= 1'b1;
					running <= 1'b0;
					full <= 1'b0;
					wadr <= {AW{1'b0}};
					sub <= 4'd0;
					divc <= 16'd0;
				end
				R_ADDR: radr <= wb_dat_i[AW-1:0];
				default: ;
				endcase
			end

		end

	end

	always @(posedge clk) begin
		if (rst) begin
			wb_ack_o <= 1'b0;
			wb_dat_o <= 32'd0;
		end else begin
			wb_ack_o <= 1'b0;
			if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
				wb_ack_o <= 1'b1;
				case (wb_adr_i)
				R_CTRL: wb_dat_o <=
					{WORDS[15:0], 13'd0, full, running, armed};
				R_DATA: wb_dat_o <= rdat;
				// Everything else in the window reads zero and
				// acks. An unacked address hangs picorv32_wb, so
				// a stale pointer must land on a dead read rather
				// than a dead machine.
				default: wb_dat_o <= 32'd0;
				endcase
			end
		end
	end

endmodule
