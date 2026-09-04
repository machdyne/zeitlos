/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/gpio.v.
 *
 *   $ iverilog -g2005 -o /tmp/tb_gpio rtl/tb/tb_gpio.v rtl/gpio.v \
 *       && /tmp/tb_gpio
 *
 * or `make tb_gpio` from the project root.
 *
 * NPORTS is 2 here rather than the 1 both current boards build, so the
 * per-port address arithmetic is actually exercised -- with one port,
 * every wrong shift of the port index still lands on port 0 and the
 * test passes. It also puts an UNBUILT port (2) immediately above a
 * built one, which is the case worth checking: writes to it must be
 * dropped rather than aliasing onto port 1.
 *
 * The pads are driven from the testbench through a pull-up model
 * (`pullup1` below): a floating pin reads high, a driven pin reads
 * whatever gpio.v drives, and an external driver wins over a floating
 * pin. That is what a real PMOD with pull-ups looks like, and it is
 * what makes the open-drain sequence in section 7 meaningful.
 *
 * What this covers, in order:
 *   1. MAGIC and CONFIG read back as documented, including the
 *      signature in CONFIG's top half.
 *   2. LED and LEDS still live at words 0 and 1 and still work --
 *      this is the compatibility that lets sw/bios/bios.c go
 *      untouched, so it is checked first among the behaviours.
 *   3. Reset state: every port floats, OUT is zero. A regression here
 *      means a bitstream that drives somebody else's outputs from the
 *      moment it loads.
 *   4. DIR/OUT round-trip and actually reach the pads.
 *   5. IN reads the pad, not OUT -- driven from the testbench side
 *      against a floating pin.
 *   6. OUTSET/OUTCLR/DIRSET/DIRCLR, including that they read back the
 *      register they modify rather than 0.
 *   7. The open-drain idiom end to end: OUT parked at 0, DIR toggled,
 *      pin alternating between driven-low and pulled-high. This is
 *      the sequence every bit of every I2C transfer will use.
 *   8. Port isolation: writing port 1 does not disturb port 0.
 *   9. An unbuilt port reads 0 and swallows writes.
 *  10. The address aliasing rtl/gpio.v documents is real, so nobody
 *      later "fixes" the decode without noticing it was a choice.
 */

`timescale 1ns / 1ps

module tb_gpio;

	localparam NPORTS = 2;

	// Word addresses. Ports start at byte 0xe000_1000 = word 0x400.
	localparam W_LED    = 26'h000;
	localparam W_LEDS   = 26'h001;
	localparam W_MAGIC  = 26'h002;
	localparam W_CONFIG = 26'h003;
	localparam W_PORT0  = 26'h400;

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

	wire led;
	wire [7:0] leds;

	wire [63:0] gpio_dir;
	wire [63:0] gpio_out;
	wire [63:0] gpio_in;

	integer errors = 0;

	always #5 clk = ~clk;

	gpio_wb #(.NPORTS(NPORTS)) dut (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(adr),
		.wb_dat_i(dat_i),
		.wb_dat_o(dat_o),
		.wb_we_i(we),
		.wb_sel_i(sel),
		.wb_stb_i(stb),
		.wb_ack_o(ack),
		.wb_cyc_i(cyc),
		.led(led),
		.leds(leds),
		.gpio_dir_o(gpio_dir),
		.gpio_out_o(gpio_out),
		.gpio_in_i(gpio_in)
	);

	// -- pad model --
	//
	// The same tri-state buffers rtl/sysctl.v builds, plus a pull-up
	// and an external driver so the pins behave like a real connector.
	// `ext_dir` bit set means the testbench is driving that pad.
	reg [15:0] ext_dir = 16'h0000;
	reg [15:0] ext_out = 16'h0000;

	wire [15:0] pad;

	genvar gb;
	generate
		for (gb = 0; gb < 16; gb = gb + 1) begin : pads
			// gpio.v's driver
			assign pad[gb] = gpio_dir[gb] ? gpio_out[gb] : 1'bz;
			// the other end of the connector
			assign pad[gb] = ext_dir[gb] ? ext_out[gb] : 1'bz;
			// and the pull-up that makes an undriven pin read 1
			pullup(pad[gb]);
		end
	endgenerate

	assign gpio_in[15:0] = pad;
	assign gpio_in[63:16] = 48'd0;

	reg [31:0] rdata;

	task wb_read(input [25:0] a);
	begin
		@(posedge clk);
		adr <= { 6'd0, a };
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

	task wb_write(input [25:0] a, input [31:0] d);
	begin
		@(posedge clk);
		adr <= { 6'd0, a };
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

	// Word address of register `r` of port `p`. Mirrors the map in
	// rtl/gpio.v: eight words per port.
	function [25:0] preg;
		input integer p;
		input integer r;
		begin
			preg = W_PORT0 + p * 8 + r;
		end
	endfunction

	initial begin

		repeat (4) @(posedge clk);
		rst = 0;
		repeat (2) @(posedge clk);

		// 1. identity
		wb_read(W_MAGIC);
		check("MAGIC", rdata, 32'h5A47_5049);
		wb_read(W_CONFIG);
		check("CONFIG", rdata, { 16'h4750, 12'd0, NPORTS[3:0] });

		// 2. the LED registers rtl/debug.v used to own, at the same
		//    addresses. sw/bios/bios.c depends on both.
		check("LED lit at reset", { 31'd0, led }, 32'd1);
		wb_read(W_LED);
		check("LED reads back", rdata, 32'd1);
		wb_write(W_LED, 32'd0);
		check("LED cleared", { 31'd0, led }, 32'd0);
		wb_write(W_LEDS, 32'h0000_00a5);
		check("LEDS", { 24'd0, leds }, 32'h0000_00a5);
		wb_read(W_LEDS);
		check("LEDS reads back", rdata, 32'h0000_00a5);

		// 3. reset state -- everything floats. The pull-ups mean the
		//    pads read all ones, which is the visible evidence that
		//    nothing is being driven.
		wb_read(preg(0, 0));
		check("port0 DIR at reset", rdata, 32'd0);
		wb_read(preg(0, 1));
		check("port0 OUT at reset", rdata, 32'd0);
		check("pads float at reset", { 16'd0, pad }, 32'h0000_ffff);

		// 4. drive some pins
		wb_write(preg(0, 1), 32'h0000_005a);	// OUT
		wb_write(preg(0, 0), 32'h0000_00ff);	// DIR = all outputs
		#1;
		check("port0 pads driven", { 24'd0, pad[7:0] }, 32'h0000_005a);
		wb_read(preg(0, 0));
		check("port0 DIR reads back", rdata, 32'h0000_00ff);

		// 5. IN follows the pad, not OUT. Float the low nibble and let
		//    the testbench drive it to the complement of what OUT
		//    holds, so a block that quietly returned OUT would fail.
		wb_write(preg(0, 0), 32'h0000_00f0);	// low nibble = inputs
		ext_dir[3:0] = 4'b1111;
		ext_out[3:0] = 4'b0101;
		repeat (4) @(posedge clk);				// synchroniser depth
		wb_read(preg(0, 2));					// IN
		check("port0 IN", rdata, 32'h0000_0055);
		ext_dir[3:0] = 4'b0000;

		// 6. the set/clear aliases
		wb_write(preg(0, 1), 32'h0000_0000);	// OUT = 0
		wb_write(preg(0, 3), 32'h0000_0033);	// OUTSET
		wb_read(preg(0, 1));
		check("OUTSET", rdata, 32'h0000_0033);
		wb_read(preg(0, 3));
		check("OUTSET reads back OUT", rdata, 32'h0000_0033);
		wb_write(preg(0, 4), 32'h0000_0011);	// OUTCLR
		wb_read(preg(0, 1));
		check("OUTCLR", rdata, 32'h0000_0022);

		wb_write(preg(0, 0), 32'h0000_0000);	// DIR = 0
		wb_write(preg(0, 5), 32'h0000_00c0);	// DIRSET
		wb_read(preg(0, 0));
		check("DIRSET", rdata, 32'h0000_00c0);
		wb_read(preg(0, 6));
		check("DIRCLR reads back DIR", rdata, 32'h0000_00c0);
		wb_write(preg(0, 6), 32'h0000_0040);	// DIRCLR
		wb_read(preg(0, 0));
		check("DIRCLR", rdata, 32'h0000_0080);

		// 7. the open-drain sequence, which is what all of the above
		//    exists for. OUT parked at 0, DIR is the data.
		wb_write(preg(1, 1), 32'h0000_0000);	// OUT = 0, once
		wb_write(preg(1, 0), 32'h0000_0000);	// DIR = 0, all float
		#1;
		check("od: floating reads high", { 24'd0, pad[15:8] }, 32'h0000_00ff);
		wb_write(preg(1, 5), 32'h0000_0001);	// DIRSET bit 0 -> drive low
		#1;
		check("od: driven low", { 24'd0, pad[15:8] }, 32'h0000_00fe);
		wb_write(preg(1, 6), 32'h0000_0001);	// DIRCLR bit 0 -> release
		#1;
		check("od: released", { 24'd0, pad[15:8] }, 32'h0000_00ff);

		// 8. ports do not bleed into each other
		wb_write(preg(0, 0), 32'h0000_00aa);
		wb_write(preg(1, 0), 32'h0000_0055);
		wb_read(preg(0, 0));
		check("port0 DIR after port1 write", rdata, 32'h0000_00aa);
		wb_read(preg(1, 0));
		check("port1 DIR", rdata, 32'h0000_0055);

		// 9. an unbuilt port. Writing it must not land on a real one.
		wb_write(preg(2, 0), 32'h0000_00ff);
		wb_read(preg(2, 0));
		check("unbuilt port reads 0", rdata, 32'd0);
		wb_read(preg(1, 0));
		check("port1 undisturbed by write to port 2", rdata, 32'h0000_0055);

		// 10. the documented aliasing. rtl/gpio.v decodes bit 10 and
		//     bits 5:0 and nothing above, so the map repeats every 8KB.
		//     Checked here so that if somebody later tightens the
		//     decode they find out it was load-bearing documentation
		//     rather than an oversight -- and if they tighten it
		//     deliberately, this is the test to delete along with the
		//     paragraph in gpio.v.
		wb_read(W_MAGIC + 26'h800);
		check("MAGIC aliases at +8KB", rdata, 32'h5A47_5049);
		wb_read(preg(1, 0) + 26'h800);
		check("port1 DIR aliases at +8KB", rdata, 32'h0000_0055);

		if (errors == 0) $display("\ntb_gpio: PASS");
		else $display("\ntb_gpio: FAIL (%0d errors)", errors);

		$finish;

	end

endmodule
