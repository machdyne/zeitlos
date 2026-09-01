/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * CSRs -- a small, read-only bank of registers exposing build-time
 * facts about THIS bitstream (how much main RAM it was built for,
 * which optional peripherals actually got synthesized in) to
 * software, so software doesn't have to guess/assume. See
 * docs/csrs.md for the full design and motivating story.
 *
 * Motivating case: sw/apps/net picking between the ENC28J60
 * (SPI_ETH) and RMII (ETH_RMII) backends, or refusing to start at
 * all on a board with neither, without ever touching a register that
 * might not physically exist on this bitstream -- reading an address
 * nothing else decodes doesn't fault on this wishbone bus (see
 * rtl/sysctl.v's own `32'hzzzz_zzzz` default case in its
 * wbm_dat_i mux), so software previously had no reliable way to tell
 * "this register genuinely isn't here" from "it's here, and this is
 * what it happens to read as right now".
 *
 * Deliberately just a handful of read-only registers, no state
 * machine, no side effects on read -- same "keep it simple" spirit
 * as rtl/debug.v, which this file's own structure closely follows.
 *
 * Unlike every other peripheral in rtl/sysctl.v, this one has NO
 * `ifdef guarding whether it exists at all -- it's always
 * instantiated, on every board, regardless of what else is or isn't
 * built in. Its whole job is to be a reliable, always-present way to
 * ask what else exists, so it can't itself be one of the things that
 * might be missing.
 *
 * Register map (word-addressed -- wb_adr_i here is
 * rtl/sysctl.v's wbm_adr_sel_word, matching every other simple slave
 * in this codebase, e.g. rtl/debug.v):
 *
 *   0  MAGIC     fixed 32'h5A45_4954 ("ZEIT" in ASCII). Check this
 *                FIRST, before trusting any other register here --
 *                it's the only way software can confirm this block
 *                itself is really present in the running bitstream,
 *                as opposed to reading back floating-bus garbage from
 *                an older build that predates rtl/csrs.v entirely
 *                (see this file's own header comment above on why an
 *                unmapped read doesn't fault on this bus).
 *   1  MEM_MB    total main RAM in megabytes, from rtl/boards.vh's
 *                `MEM for the board this was built for (defaulted to
 *                1 by rtl/sysctl.v if a board block doesn't set it --
 *                see that file's own `ifndef MEM guard).
 *   2  FEATURES  bitmask of which optional peripherals THIS
 *                bitstream was actually built with -- see
 *                rtl/sysctl.v's CSR_FEATURES localparam for exactly
 *                which `ifdef maps to which bit. Software-side
 *                mirror of the same bit assignments:
 *                sw/common/zsoc.h's Z_FEATURE_* constants -- there's
 *                no single generated source shared between Verilog
 *                and C here, so both sides have to be hand-edited
 *                together and kept in sync deliberately, same as
 *                e.g. rtl/usb_hid.v/sw/common/zkbd.h's own HID-usage
 *                translation is split the same way.
 */

module csrs_wb #(
	parameter MEM_MB = 1,
	parameter FEATURES = 32'h0
)
(
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output wb_ack_o,
	input wb_cyc_i,
);

	localparam MAGIC = 32'h5A45_4954; // "ZEIT"

	// Registered, like every other slave on this bus. It was
	// combinational (`assign wb_ack_o = cyc && stb`) -- the ONE slave
	// that was -- which put the address decode, three 32-bit compares
	// and this block's position at the tail of the ack mux into a
	// single 48MHz cycle. One extra cycle on a block read at boot is
	// free; the timing was not.
	//
	// wb_we_i is deliberately never even looked at: writes are silently
	// no-ops, not errors, matching this bus's usual "unmapped access
	// doesn't fault" behavior rather than introducing a new failure
	// mode for what should be an inert, read-only block.
	reg        ack_r;
	reg [31:0] dat_r;
	assign wb_ack_o = ack_r;
	assign wb_dat_o = dat_r;

	always @(posedge wb_clk_i) begin
		if (wb_rst_i) begin
			ack_r <= 1'b0;
			dat_r <= 32'h0;
		end else begin
			ack_r <= wb_cyc_i && wb_stb_i && !ack_r;
			dat_r <=
				(wb_adr_i == 32'd0) ? MAGIC :
				(wb_adr_i == 32'd1) ? MEM_MB :
				(wb_adr_i == 32'd2) ? FEATURES :
				32'h0;
		end
	end

endmodule
