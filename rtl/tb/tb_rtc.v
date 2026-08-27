/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/rtc.v.
 *
 *   $ iverilog -o /tmp/tb_rtc rtl/tb/tb_rtc.v rtl/rtc.v && /tmp/tb_rtc
 *
 * CLK_HZ is overridden to 10240 rather than the real 48MHz, so the
 * prescaler is 10 cycles per sub-second tick and a simulated second
 * costs 10240 cycles instead of 48 million. Every ratio the block
 * actually cares about (CLK_HZ / SUBSEC_HZ, SUBSEC_HZ per second) is
 * preserved, so this exercises the same arithmetic in a runnable
 * amount of time -- the one thing it does NOT check is that 48MHz
 * divides by 1024 exactly, which is arithmetic done at elaboration
 * and is checked by inspection in rtc.v's own comment.
 *
 * What this covers, in order:
 *   1. MAGIC / RATE read back as the documented constants.
 *   2. VALID is clear at reset (see rtc.v on why that is its own
 *      state rather than implied by SEC == 0).
 *   3. Free-running: SUB advances, SEC rolls over after exactly
 *      SUBSEC_HZ sub-ticks.
 *   4. Setting the clock: SEC write takes, VALID sets, the staged
 *      SUB preload is adopted, and the prescaler restarts.
 *   5. The preload is consumed -- a second bare SEC write starts at
 *      .000 rather than inheriting the earlier fraction. This is the
 *      one behaviour most likely to be got wrong by a later edit.
 *   6. TZ round-trips sign-extended, so a negative offset reads back
 *      as a negative int32_t.
 *   7. VALID can be cleared through CTRL.
 */

`timescale 1ns / 1ps

module tb_rtc;

	localparam SUBSEC_HZ = 1024;
	localparam TB_CLK_HZ = 10240;		// -> DIV = 10 cycles/sub-tick
	localparam DIV = TB_CLK_HZ / SUBSEC_HZ;

	reg clk = 0;
	reg rst = 1;

	reg [31:0] adr = 0;
	reg [31:0] dat_i = 0;
	wire [31:0] dat_o;
	reg we = 0;
	reg [3:0] sel = 4'b1111;
	reg stb = 0;
	wire ack;
	reg cyc = 0;

	integer errors = 0;

	always #5 clk = ~clk;

	rtc_wb #(.CLK_HZ(TB_CLK_HZ)) dut (
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

	// One wishbone read. Leaves the result in `rdata`.
	reg [31:0] rdata;

	task wb_read(input [2:0] a);
	begin
		@(posedge clk);
		adr <= { 29'd0, a };
		we <= 1'b0;
		stb <= 1'b1;
		cyc <= 1'b1;
		@(posedge clk);
		while (!ack) @(posedge clk);
		rdata = dat_o;
		stb <= 1'b0;
		cyc <= 1'b0;
		@(posedge clk);
	end
	endtask

	task wb_write(input [2:0] a, input [31:0] d);
	begin
		@(posedge clk);
		adr <= { 29'd0, a };
		dat_i <= d;
		we <= 1'b1;
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

	task check(input [255:0] what, input [31:0] got, input [31:0] want);
	begin
		if (got !== want) begin
			$display("FAIL: %0s: got 0x%08x, want 0x%08x", what, got, want);
			errors = errors + 1;
		end else begin
			$display("ok:   %0s = 0x%08x", what, got);
		end
	end
	endtask

	integer i;
	reg [31:0] sec_before;

	initial begin

		repeat (4) @(posedge clk);
		rst = 0;
		repeat (2) @(posedge clk);

		// 1. constants
		wb_read(3'd0);
		check("MAGIC", rdata, 32'h5A52_5443);
		wb_read(3'd4);
		check("RATE", rdata, SUBSEC_HZ);

		// 2. not valid until set
		wb_read(3'd3);
		check("CTRL valid at reset", rdata, 32'd0);

		// 3. free-running. Read SUB, wait a known number of sub-ticks,
		//    read again. Bus cycles themselves cost time, so compare
		//    against a window rather than an exact value.
		wb_read(3'd2);
		sec_before = rdata;
		repeat (DIV * 20) @(posedge clk);
		wb_read(3'd2);
		if (rdata <= sec_before || (rdata - sec_before) > 24) begin
			$display("FAIL: SUB advanced by %0d, expected ~20", rdata - sec_before);
			errors = errors + 1;
		end else begin
			$display("ok:   SUB advanced by %0d over 20 sub-ticks", rdata - sec_before);
		end

		// SEC rolls over after a full second of sub-ticks. Set a known
		// time first so this is measured from a clean start.
		wb_write(3'd1, 32'd1000);
		wb_read(3'd1);
		check("SEC after set", rdata, 32'd1000);
		wb_read(3'd3);
		check("CTRL valid after set", rdata, 32'd1);
		wb_read(3'd2);
		if (rdata > 8) begin
			$display("FAIL: SUB after set is %0d, expected near 0", rdata);
			errors = errors + 1;
		end else begin
			$display("ok:   SUB after set = %0d", rdata);
		end

		// Just under a second: still 1000.
		repeat (DIV * (SUBSEC_HZ - 40)) @(posedge clk);
		wb_read(3'd1);
		check("SEC just before rollover", rdata, 32'd1000);

		// Past it: 1001.
		repeat (DIV * 60) @(posedge clk);
		wb_read(3'd1);
		check("SEC after rollover", rdata, 32'd1001);

		// 4. staged sub-second preload is adopted by the SEC write
		wb_write(3'd2, 32'd512);
		wb_write(3'd1, 32'd2000);
		wb_read(3'd1);
		check("SEC after set with preload", rdata, 32'd2000);
		wb_read(3'd2);
		if (rdata < 512 || rdata > 520) begin
			$display("FAIL: SUB after preloaded set is %0d, expected ~512", rdata);
			errors = errors + 1;
		end else begin
			$display("ok:   SUB after preloaded set = %0d", rdata);
		end

		// 5. ...and consumed: a bare SEC write must NOT inherit it
		wb_write(3'd1, 32'd3000);
		wb_read(3'd2);
		if (rdata > 8) begin
			$display("FAIL: SUB after bare set is %0d -- preload was not consumed", rdata);
			errors = errors + 1;
		end else begin
			$display("ok:   SUB after bare set = %0d (preload consumed)", rdata);
		end

		// 6. TZ sign extension
		wb_write(3'd5, 32'd60);
		wb_read(3'd5);
		check("TZ +60", rdata, 32'd60);
		wb_write(3'd5, 32'hFFFF_FFC4);		// -60
		wb_read(3'd5);
		check("TZ -60 sign-extended", rdata, 32'hFFFF_FFC4);

		// 7. VALID clearable
		wb_write(3'd3, 32'd0);
		wb_read(3'd3);
		check("CTRL valid cleared", rdata, 32'd0);

		// reserved registers read 0
		wb_read(3'd6);
		check("reserved reg 6", rdata, 32'd0);

		if (errors == 0)
			$display("\ntb_rtc: PASS");
		else
			$display("\ntb_rtc: FAIL (%0d)", errors);

		$finish;

	end

endmodule
