/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Phantom 16550 -- a UART-shaped hole for builds without `UART0.
 *
 * -- Why this exists --
 *
 * Every other optional peripheral in this SOC degrades quietly when it
 * is left out: rtl/sysctl.v hands the address window to rtl/csrs.v,
 * which acks it and reads back zero, the FEATURE bit goes clear, and
 * the software-side probe (z_rtc_available(), z_audio_present(), ...)
 * answers false. Nothing hangs.
 *
 * UART0 was the exception, and it failed the worst way available. With
 * `UART0 undefined there was no cs_uart0 in the ack mux, so the mux
 * fell through to 1'b0 and a read of 0xf000_0014 NEVER ACKED. The very
 * first thing sw/bios/bios.c's putchar() does is
 *
 *     while ((reg_uart0_lsr & 0x20) == 0);
 *
 * so the CPU stalled on that read before a single character or pixel
 * reached anything. A board built without a UART did not come up
 * degraded; it did not come up.
 *
 * -- Why a phantom rather than checks in software --
 *
 * The honest fix is for every caller to consult Z_FEATURE_UART0 first.
 * There are a lot of callers -- the BIOS, the kernel's console, the
 * shell, uart.c's ISR and its transmit drain -- and each one would grow
 * a branch that is dead on every board anyone actually has. That is a
 * large, invasive, permanently-carried change to avoid a case that is
 * entirely local to one address window.
 *
 * So this block answers instead. It is not a UART; it is the smallest
 * thing that lets code written for a UART run to completion:
 *
 *   LSR (word 5) reads 0x60 -- THRE and TEMT set, DR clear.
 *
 * That is the whole trick. "Transmitter is always ready" means every
 * spin-until-ready loop exits on its first read instead of never, and
 * the character written afterwards goes nowhere. "Receiver never has
 * data" means every drain loop exits immediately and nothing invents
 * input. Output is discarded, input is empty, and no caller can tell
 * the difference except by asking the FEATURE bit -- which is exactly
 * the property that makes the software side need no changes.
 *
 *   IIR (word 2) reads 0x01 -- bit 0 set is "no interrupt pending",
 *                             which is what sw/os/uart.c's ISR tests
 *                             first. Belt and braces: int_o is tied
 *                             low, so that ISR never runs anyway.
 *
 * Everything else reads zero. Writes are accepted and dropped.
 *
 * -- What this is NOT --
 *
 * Not a loopback. A phantom UART that echoed transmitted bytes back
 * would make the kernel shell appear to work while nothing was
 * connected, which is a worse failure than silence: it looks like a
 * working console right up until you wonder why nothing you type
 * arrives.
 *
 * Not a way to have a console without pins. Z_FEATURE_UART0
 * (sw/common/zsoc.h, bit 12) is clear on a build using this, and any
 * software that wants to TELL THE USER there is no serial console --
 * as opposed to merely not crashing -- should check that bit. This
 * block's job is only to keep the machine alive; saying so is
 * somebody else's.
 *
 * -- Cost --
 *
 * One registered ack and a 3-way mux on a 26-bit compare. It is
 * cheaper than the address decoder that reaches it.
 *
 * Word-addressed, like every other slave here: rtl/sysctl.v passes
 * wbm_adr_sel_word (wbm_adr_sel[27:2]), so word 5 is byte offset 0x14,
 * which is where sw/common/zeitlos.h puts reg_uart0_lsr.
 */

`default_nettype none

module uart_null
(
	input wire wb_clk_i,
	input wire wb_rst_i,

	input wire [25:0] wb_adr_i,
	input wire [31:0] wb_dat_i,
	output wire [31:0] wb_dat_o,
	input wire wb_we_i,
	input wire [3:0] wb_sel_i,
	input wire wb_stb_i,
	input wire wb_cyc_i,
	output reg wb_ack_o,

	output wire int_o
);

	// 16550 register indices, as WORDS. Byte offsets are 4x these --
	// see sw/common/zeitlos.h's reg_uart0_* macros.
	localparam REG_IIR = 26'd2;		// 0x08
	localparam REG_LSR = 26'd5;		// 0x14

	// LSR: THRE (bit 5) and TEMT (bit 6) always set, DR (bit 0) always
	// clear. See the header -- this pair of facts is the entire reason
	// this module exists.
	localparam [7:0] LSR_VALUE = 8'h60;

	// IIR bit 0 set means "no interrupt pending", which is the first
	// thing sw/os/uart.c's handler tests.
	localparam [7:0] IIR_VALUE = 8'h01;

	assign wb_dat_o =
		(wb_adr_i == REG_LSR) ? {24'h0, LSR_VALUE} :
		(wb_adr_i == REG_IIR) ? {24'h0, IIR_VALUE} :
		32'h0;

	// Never interrupt. Tied off rather than left to IIR alone, so that
	// a build using this block cannot enter uart.c's ISR at all.
	assign int_o = 1'b0;

	// Single-cycle ack, registered, matching the other slaves on this
	// bus. Writes are accepted and discarded -- acking a write we do
	// not perform is the point, not an oversight.
	always @(posedge wb_clk_i) begin
		if (wb_rst_i)
			wb_ack_o <= 1'b0;
		else
			wb_ack_o <= wb_stb_i && wb_cyc_i && !wb_ack_o;
	end

endmodule

`default_nettype wire
