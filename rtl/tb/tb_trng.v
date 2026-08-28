/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/trng.v.
 *
 * Defines `TRNG_SIM, which swaps the ring oscillator bank for a set of
 * LFSRs -- see trng.v's own header for why a real oscillator bank
 * cannot be simulated by an event-driven simulator (a zero-delay
 * combinational loop never converges, iverilog simply hangs). That
 * means THIS TESTBENCH DOES NOT AND CANNOT TEST THE ENTROPY SOURCE.
 * It tests everything downstream of it: the sampler, the von Neumann
 * debiaser, the packer, the FIFO, the health monitor and the Wishbone
 * interface. Whether the oscillators oscillate is a question for a
 * board, an oscilloscope and the HEALTH bit -- see docs/trng.md.
 *
 *   iverilog -g2005 -DTRNG_SIM -o tb_trng.vvp tb_trng.v ../trng.v
 *   ./tb_trng.vvp
 *
 * SAMPLE_DIV is overridden to 4 here so a run finishes in a sensible
 * number of simulated cycles. The real default is 256.
 */

`timescale 1ns / 1ps

module tb_trng;

	reg clk;
	reg rst;
	reg [31:0] adr;
	reg [31:0] dat_i;
	wire [31:0] dat_o;
	reg we;
	reg [3:0] sel;
	reg stb;
	wire ack;
	reg cyc;

	integer errors;
	integer words_read;
	integer i;
	integer ones_total;
	integer zeros_total;
	reg [31:0] rv;
	reg [31:0] prev_word;
	integer distinct;

	trng_wb #(
		.CLK_HZ(48_000_000),
		.NUM_RO(8),
		.RO_BASE(13),
		.SAMPLE_DIV(4),
		.FIFO_DEPTH(8)
	) dut (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(adr),
		.wb_dat_i(dat_i),
		.wb_dat_o(dat_o),
		.wb_we_i(we),
		.wb_sel_i(sel),
		.wb_stb_i(stb),
		.wb_ack_o(ack),
		.wb_cyc_i(cyc)
	);

	always #10 clk = ~clk;		// 50MHz-ish, close enough to 48

	// -- bus helpers --
	//
	// Tasks rather than inline code because a Wishbone cycle here is
	// four coordinated signals and getting one of them wrong in one
	// of a dozen places is how a testbench ends up testing itself.

	task wb_read;
		input [2:0] regno;
		output [31:0] value;
		begin
			@(posedge clk);
			adr <= { 29'd0, regno };
			dat_i <= 32'd0;
			we <= 1'b0;
			sel <= 4'hf;
			stb <= 1'b1;
			cyc <= 1'b1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			value = dat_o;
			stb <= 1'b0;
			cyc <= 1'b0;
			@(posedge clk);
		end
	endtask

	task wb_write;
		input [2:0] regno;
		input [31:0] value;
		begin
			@(posedge clk);
			adr <= { 29'd0, regno };
			dat_i <= value;
			we <= 1'b1;
			sel <= 4'hf;
			stb <= 1'b1;
			cyc <= 1'b1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			stb <= 1'b0;
			cyc <= 1'b0;
			we <= 1'b0;
			@(posedge clk);
		end
	endtask

	task check;
		input cond;
		input [511:0] name;
		begin
			if (cond) begin
				$display("  ok   %0s", name);
			end else begin
				$display("  FAIL %0s", name);
				errors = errors + 1;
			end
		end
	endtask

	initial begin

		clk = 0; rst = 1; adr = 0; dat_i = 0; we = 0; sel = 0;
		stb = 0; cyc = 0;
		errors = 0; words_read = 0; ones_total = 0; zeros_total = 0;
		distinct = 0; prev_word = 32'hffffffff;

		repeat (8) @(posedge clk);
		rst = 0;
		repeat (8) @(posedge clk);

		$display("tb_trng: start");

		// -- MAGIC --
		//
		// The one register software is allowed to trust before it
		// trusts anything else (see trng.v's register map).
		wb_read(3'd0, rv);
		check(rv === 32'h5A52_4E47, "MAGIC reads ZRNG");

		// -- RATE is a sane, nonzero number --
		wb_read(3'd4, rv);
		check(rv > 0, "RATE is nonzero");
		$display("  ..   RATE = %0d words/sec (SAMPLE_DIV=4 in this tb)", rv);

		// -- CTRL comes up enabled --
		wb_read(3'd3, rv);
		check(rv[0] === 1'b1, "CTRL.ENABLE set out of reset");

		// -- wait for the FIFO to fill --
		//
		// Von Neumann discards most samples, so this needs real time
		// even at SAMPLE_DIV=4. If READY never comes up the packer or
		// the debiaser is stuck, which is the failure this catches.
		i = 0;
		rv = 0;
		while (rv[0] !== 1'b1 && i < 20000) begin
			wb_read(3'd2, rv);
			i = i + 1;
		end
		check(rv[0] === 1'b1, "STATUS.READY eventually asserts");
		check(rv[1] === 1'b1, "STATUS.HEALTH_OK still set");

		// -- reading DATA pops --
		//
		// The property that matters for a random source: you never get
		// the same word twice just because you read twice. A source
		// that returned a held register would pass every other test
		// here and be catastrophically wrong.
		wb_read(3'd2, rv);
		i = rv[11:4];			// level before
		wb_read(3'd1, rv);
		wb_read(3'd2, rv);
		check(rv[11:4] == (i - 1), "reading DATA decrements LEVEL");

		// -- collect words, check they are not all identical --
		for (words_read = 0; words_read < 24; words_read = words_read + 1) begin
			rv = 0;
			i = 0;
			while (rv[0] !== 1'b1 && i < 20000) begin
				wb_read(3'd2, rv);
				i = i + 1;
			end
			wb_read(3'd1, rv);
			if (rv !== prev_word) distinct = distinct + 1;
			prev_word = rv;
			for (i = 0; i < 32; i = i + 1) begin
				if (rv[i]) ones_total = ones_total + 1;
				else zeros_total = zeros_total + 1;
			end
		end
		check(distinct > 20, "consecutive words differ");

		// Von Neumann guarantees an unbiased output for any fixed
		// input bias, so even the LFSR stand-in should land near 50/50
		// -- a wide window here because 768 bits is a small sample and
		// this test is looking for "stuck", not for randomness.
		$display("  ..   ones=%0d zeros=%0d of %0d bits",
			ones_total, zeros_total, ones_total + zeros_total);
		check(ones_total > (ones_total + zeros_total) / 4 &&
			ones_total < 3 * (ones_total + zeros_total) / 4,
			"output is not stuck at all-ones or all-zeros");

		// -- disabling really stops it --
		//
		// Not cosmetic: CTRL.ENABLE gates the oscillators themselves,
		// and software that turns the source off has a right to expect
		// it stays off.
		wb_write(3'd3, 32'h0000_0003);	// clear health + flush, stay enabled
		wb_write(3'd3, 32'h0000_0000);	// disable
		wb_read(3'd2, rv);
		check(rv[2] === 1'b0, "STATUS.ENABLED clears");
		repeat (2000) @(posedge clk);
		wb_read(3'd2, rv);
		check(rv[0] === 1'b0, "no new words while disabled");

		// -- and re-enabling starts it again --
		wb_write(3'd3, 32'h0000_0001);
		rv = 0;
		i = 0;
		while (rv[0] !== 1'b1 && i < 20000) begin
			wb_read(3'd2, rv);
			i = i + 1;
		end
		check(rv[0] === 1'b1, "words resume after re-enable");

		// -- health monitor actually fires --
		//
		// Forced rather than waited for: a stuck source is the exact
		// failure this block exists to make visible, and a test that
		// only ever sees the healthy path proves nothing about it.
		// Holding the sampler's input constant is what a bank of
		// optimised-away oscillators looks like from here.
		// NOTE the 0x3, not 0x2: a CTRL write always loads ENABLE
		// from bit 0, so clearing the health flag with bit 1 alone
		// would also switch the source off and nothing would ever be
		// sampled again. Caught here first; zrng.c does the same.
		wb_write(3'd3, 32'h0000_0003);
		force dut.ro_s2 = {8{1'b0}};
		repeat (20000) @(posedge clk);
		release dut.ro_s2;
		wb_read(3'd2, rv);
		check(rv[1] === 1'b0, "HEALTH_OK clears on a stuck source");

		// -- and can be acknowledged --
		wb_write(3'd3, 32'h0000_0003);
		wb_read(3'd2, rv);
		check(rv[1] === 1'b1, "CTRL bit 1 clears the sticky failure");
		check(rv[11:4] == 0, "acknowledging also flushed the FIFO");

		$display("tb_trng: %0d error(s)", errors);
		if (errors == 0) $display("tb_trng: PASS");
		else $display("tb_trng: FAIL");
		$finish;

	end

endmodule
