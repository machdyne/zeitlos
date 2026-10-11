/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/gpio_i2cs.v, the GPIO I2C slave, inside a whole
 * rtl/gpio.v (one port, one stream engine, four slots).
 *
 *   make test_gpio_i2cs
 *
 * The master is modelled on the LS10's (a Zwölf module's bit-banged
 * C/D bus, which is what the slave is for): open drain on both lines,
 * a fixed time per line change -- 4 us, about 40 kHz -- and no
 * patience for clock stretching. Part of the test runs at 400 kHz
 * too. SCL is port 0 pin 2 and SDA pin 3, both with pull-ups.
 *
 * What this covers:
 *   1. CONFIG reports the slave; the engine next to it still answers.
 *   2. The mirror loads and reads back through IMADDR/IMDATA.
 *   3. Probes: a mapped address ACKs, an unmapped one does not.
 *   4. Writes with pair toggle (a TCA9535): the mirror and the log.
 *   5. A write to a read-only register: ACKed, ignored, logged.
 *   6. Write then read with a repeated start; the read flag and the
 *      register the read started at.
 *   7. The pointer persists: a bare read starts where the last
 *      transaction left it.
 *   8. Linear auto-increment, wrapping at 255; no auto-increment.
 *   9. The log drains in order, and reads 0 when empty.
 *  10. Log overflow at 512 entries, and IFLUSH.
 *  11. Pin ownership: DIR and OUT do not reach SCL and SDA.
 *  12. A 50 ns glitch on SCL is filtered out.
 *  13. Disabled, it answers nothing and lets go of the pins.
 *  Throughout: the slave never drives SCL, and never drives SDA high.
 */

`timescale 1ns / 1ps

module tb_gpio_i2cs;

	// word addresses
	localparam W_CONFIG = 26'h003;
	localparam W_PORT0 = 26'h400;
	localparam W_ENG0 = 26'h800;
	localparam W_ICTL = 26'h840, W_ISTAT = 26'h841, W_IMADDR = 26'h842,
	           W_IMDATA = 26'h843, W_ISLOT0 = 26'h844, W_ILOG = 26'h848,
	           W_IFLUSH = 26'h849;

	localparam SCL = 2, SDA = 3;

	reg clk = 0;
	reg rst = 1;
	always #5 clk = ~clk;

	reg [31:0] adr = 0;
	reg [31:0] dat_i = 0;
	wire [31:0] dat_o;
	reg we = 0;
	reg stb = 0;
	reg cyc = 0;
	wire ack;
	wire led;
	wire [7:0] leds;
	wire [63:0] gpio_dir, gpio_out, gpio_in;

	gpio_wb #(.NPORTS(1), .ENGINES(1), .I2CS_SLOTS(4)) dut (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr), .wb_dat_i(dat_i), .wb_dat_o(dat_o),
		.wb_we_i(we), .wb_sel_i(4'b1111), .wb_stb_i(stb), .wb_ack_o(ack),
		.wb_cyc_i(cyc),
		.led(led), .leds(leds),
		.gpio_dir_o(gpio_dir), .gpio_out_o(gpio_out), .gpio_in_i(gpio_in)
	);

	// -- pads: gpio.v's drivers, the master's, and pull-ups --

	reg m_scl_low = 0, m_sda_low = 0;
	reg glitch = 0;			// a short pulse on SCL, from outside
	wire [7:0] pad;

	genvar g;
	generate
		for (g = 0; g < 8; g = g + 1) begin : pads
			assign pad[g] = gpio_dir[g] ? gpio_out[g] : 1'bz;
			pullup(pad[g]);
		end
	endgenerate
	assign pad[SCL] = (m_scl_low || glitch) ? 1'b0 : 1'bz;
	assign pad[SDA] = m_sda_low ? 1'b0 : 1'bz;
	assign gpio_in = { 56'd0, pad };

	integer errors = 0;

	// the rules that hold at every moment
	always @(posedge clk) begin
		if (!rst && gpio_dir[SCL] && dut.i2cs.slave.live) begin
			$display("FAIL: the slave drives SCL");
			errors = errors + 1;
		end
		if (!rst && dut.i2cs.slave.live && gpio_dir[SDA] && gpio_out[SDA]) begin
			$display("FAIL: the slave drives SDA high");
			errors = errors + 1;
		end
	end

`ifdef DEBUG
	always @(dut.i2cs.slave.ps or dut.i2cs.slave.bitn or dut.i2cs.slave.start or dut.i2cs.slave.stop)
		$display("%t ps=%0d bitn=%0d sh=%02x start=%b stop=%b scl=%b sda=%b", $time,
		         dut.i2cs.slave.ps, dut.i2cs.slave.bitn, dut.i2cs.slave.sh,
		         dut.i2cs.slave.start, dut.i2cs.slave.stop, dut.i2cs.slave.scl_f, dut.i2cs.slave.sda_f);
`endif

	// -- the bus --

	reg [31:0] rdata;

	task wb_read(input [25:0] a);
	begin
		@(posedge clk);
		adr <= { 6'd0, a }; we <= 1'b0; stb <= 1'b1; cyc <= 1'b1;
		@(posedge clk);
		while (!ack) @(posedge clk);
		rdata = dat_o;
		stb <= 1'b0; cyc <= 1'b0;
		@(posedge clk);
	end
	endtask

	task wb_write(input [25:0] a, input [31:0] d);
	begin
		@(posedge clk);
		adr <= { 6'd0, a }; dat_i <= d; we <= 1'b1; stb <= 1'b1; cyc <= 1'b1;
		@(posedge clk);
		while (!ack) @(posedge clk);
		stb <= 1'b0; cyc <= 1'b0; we <= 1'b0;
		@(posedge clk);
	end
	endtask

	task check(input [639:0] what, input [31:0] got, input [31:0] want);
	begin
		if (got !== want) begin
			$display("FAIL: %0s: got 0x%08x, want 0x%08x", what, got, want);
			errors = errors + 1;
		end else
			$display("ok:   %0s = 0x%08x", what, got);
	end
	endtask

	// -- the master, LS10-style --

	integer T = 4000;		// ns per line change

	task hold; #(T); endtask

	task m_start;
	begin
		m_sda_low = 0; m_scl_low = 0; hold;
		m_sda_low = 1; hold;
		m_scl_low = 1; hold;
	end
	endtask

	task m_restart;
	begin
		m_sda_low = 0; hold;
		m_scl_low = 0; hold;
		m_sda_low = 1; hold;
		m_scl_low = 1; hold;
	end
	endtask

	task m_stop;
	begin
		m_sda_low = 1; hold;
		m_scl_low = 0; hold;
		m_sda_low = 0; hold;
	end
	endtask

	// a byte out; returns 1 if it was ACKed
	reg acked;
	task m_out(input [7:0] b);
		integer n;
	begin
		for (n = 7; n >= 0; n = n - 1) begin
			m_sda_low = !b[n]; hold;
			m_scl_low = 0; hold;
			m_scl_low = 1;
		end
		m_sda_low = 0; hold;
		m_scl_low = 0; hold;
		acked = !pad[SDA];
		m_scl_low = 1; hold;
	end
	endtask

	// a byte in; `a` says whether to ACK it
	reg [7:0] got;
	task m_in(input a);
		integer n;
	begin
		m_sda_low = 0;
		for (n = 7; n >= 0; n = n - 1) begin
			hold;
			m_scl_low = 0; hold;
			got[n] = pad[SDA];
			m_scl_low = 1;
		end
		m_sda_low = a; hold;
		m_scl_low = 0; hold;
		m_scl_low = 1; hold;
		m_sda_low = 0;
	end
	endtask

	// I2C a, r, v... : write
	task i2c_write(input [6:0] a, input [7:0] r, input integer n,
	               input [7:0] v0, input [7:0] v1, input [7:0] v2);
	begin
		m_start;
		m_out({ a, 1'b0 });
		if (!acked) begin $display("FAIL: address 0x%02x NACKed", a); errors = errors + 1; end
		m_out(r);
		if (!acked) begin $display("FAIL: command NACKed"); errors = errors + 1; end
		if (n > 0) begin m_out(v0); if (!acked) begin $display("FAIL: data NACKed"); errors = errors + 1; end end
		if (n > 1) begin m_out(v1); if (!acked) begin $display("FAIL: data NACKed"); errors = errors + 1; end end
		if (n > 2) begin m_out(v2); if (!acked) begin $display("FAIL: data NACKed"); errors = errors + 1; end end
		m_stop;
	end
	endtask

	// the log's next entry, as { valid, slot, reg, data }
	task expect_log(input [639:0] what, input [1:0] s, input [7:0] r, input [7:0] d);
	begin
		wb_read(W_ILOG);
		check(what, rdata, { 1'b1, 5'd0, s, 8'd0, r, d });
	end
	endtask

	function [7:0] tca;		// the TCA9535's mirror contents, loaded below
		input integer r;
		begin
			tca = 8'h11 * (r + 1);
		end
	endfunction

	integer i;
	reg [7:0] b0, b1, b2, b3;

	initial begin
		repeat (4) @(posedge clk);
		rst = 0;
		repeat (4) @(posedge clk);

		// 1. identity
		wb_read(W_CONFIG);
		check("CONFIG: 4 slots, an engine, one port", rdata, 32'h4750_4F11);
		wb_write(W_ENG0 + 2, 32'h0000_1234);
		wb_read(W_ENG0 + 2);
		check("the engine next door still answers", rdata, 32'h0000_1234);

		// 2. the mirror. Slot 0 is a TCA9535 at 0x20: registers 0-1 the
		//    input port (read only), 2-7 output, polarity, config.
		wb_write(W_IMADDR, { 2'd0, 8'd0 });
		for (i = 0; i < 8; i = i + 1)
			wb_write(W_IMDATA, { 23'd0, (i >= 2), tca(i) });
		// slot 1, linear, all 256 registers writable
		wb_write(W_IMADDR, { 2'd1, 8'd0 });
		for (i = 0; i < 256; i = i + 1)
			wb_write(W_IMDATA, { 23'd0, 1'b1, i[7:0] ^ 8'hA5 });
		// slot 2, no auto-increment
		wb_write(W_IMADDR, { 2'd2, 8'd5 });
		wb_write(W_IMDATA, { 23'd0, 1'b0, 8'h5A });

		wb_write(W_IMADDR, { 2'd0, 8'd1 });
		wb_read(W_IMDATA);
		check("mirror slot 0 reg 1", rdata, { 23'd0, 1'b0, tca(1) });
		wb_read(W_IMDATA);
		check("mirror slot 0 reg 2 (auto-increment)", rdata, { 23'd0, 1'b1, tca(2) });

		wb_write(W_ISLOT0 + 0, { 22'd0, 2'd2, 1'b1, 7'h20 });	// pair toggle
		wb_write(W_ISLOT0 + 1, { 22'd0, 2'd1, 1'b1, 7'h50 });	// linear
		wb_write(W_ISLOT0 + 2, { 8'd5, 8'd0, 1'b1, 5'd0, 2'd0, 1'b1, 7'h21 });	// none, ptr 5
		wb_write(W_ICTL, { 10'd0, 6'd3, 2'd0, 6'd2, 7'd0, 1'b1 });
		wb_read(W_ISTAT);
		check("ISTAT: live, nothing logged", rdata, 32'h0010_0000);

		// 3. probes
		m_start; m_out({ 7'h20, 1'b0 }); m_stop;
		check("probe 0x20 ACKed", acked, 1);
		m_start; m_out({ 7'h22, 1'b0 }); m_stop;
		check("probe 0x22 not ACKed", acked, 0);
		m_start; m_out({ 7'h50, 1'b0 }); m_stop;
		check("probe 0x50 ACKed", acked, 1);

		// 4. writes, pair toggle: 2, 3, 2
		i2c_write(7'h20, 8'd2, 3, 8'hAA, 8'h55, 8'h33);
		wb_write(W_IMADDR, { 2'd0, 8'd2 });
		wb_read(W_IMDATA);
		check("reg 2 after 2,3,2", rdata, { 23'd0, 1'b1, 8'h33 });
		wb_read(W_IMDATA);
		check("reg 3", rdata, { 23'd0, 1'b1, 8'h55 });

		// 5. a read-only register: ACKed (i2c_write checks), unchanged
		i2c_write(7'h20, 8'd0, 1, 8'hEE, 0, 0);
		wb_write(W_IMADDR, { 2'd0, 8'd0 });
		wb_read(W_IMDATA);
		check("read-only reg 0 unchanged", rdata, { 23'd0, 1'b0, tca(0) });

		wb_read(W_ISTAT);
		check("ISTAT: four logged", rdata[9:0], 4);

		// 6. write the command, repeated start, read two (pair toggle)
		wb_write(W_ISTAT, 32'h000F_0000);		// clear the read flags
		m_start; m_out({ 7'h20, 1'b0 }); m_out(8'd0);
		m_restart; m_out({ 7'h20, 1'b1 });
		check("read address ACKed", acked, 1);
		m_in(1); b0 = got;
		m_in(0); b1 = got;
		m_stop;
		check("read reg 0", b0, tca(0));
		check("read reg 1", b1, tca(1));
		wb_read(W_ISTAT);
		check("read flag for slot 0 only", rdata[19:16], 4'b0001);
		wb_read(W_ISLOT0);
		check("read started at 0; pointer back at 0", rdata[31:16], 16'h0000);

		// 7. the pointer persists: a bare read starts at 0, and after a
		//    write that only sets it, at 1
		m_start; m_out({ 7'h20, 1'b1 }); m_in(0); m_stop;
		check("bare read: reg 0", got, tca(0));
		m_start; m_out({ 7'h20, 1'b0 }); m_out(8'd1); m_stop;
		m_start; m_out({ 7'h20, 1'b1 }); m_in(0); m_stop;
		check("bare read after setting 1: reg 1", got, tca(1));

		// 8. linear, across the wrap at 255; and no auto-increment
		m_start; m_out({ 7'h50, 1'b0 }); m_out(8'hFE);
		m_restart; m_out({ 7'h50, 1'b1 });
		m_in(1); b0 = got; m_in(1); b1 = got; m_in(0); b2 = got;
		m_stop;
		check("linear 0xFE", b0, 8'hFE ^ 8'hA5);
		check("linear 0xFF", b1, 8'hFF ^ 8'hA5);
		check("linear wraps to 0", b2, 8'h00 ^ 8'hA5);
		m_start; m_out({ 7'h21, 1'b1 });
		m_in(1); b0 = got; m_in(1); b1 = got; m_in(0); b2 = got;
		m_stop;
		check("no increment: 5, 5, 5", { b0, b1, b2 }, 24'h5A5A5A);

		// 9. the log, in order
		expect_log("log 1", 2'd0, 8'd2, 8'hAA);
		expect_log("log 2", 2'd0, 8'd3, 8'h55);
		expect_log("log 3", 2'd0, 8'd2, 8'h33);
		expect_log("log 4: the read-only write", 2'd0, 8'd0, 8'hEE);
		wb_read(W_ILOG);
		check("log empty reads 0", rdata, 0);

		// 12. a 50 ns glitch on SCL in the middle of a write's byte
		m_start; m_out({ 7'h50, 1'b0 }); m_out(8'h10);
		// a glitch while SCL is high for bit 6 of 0xC3: unfiltered, it
		// would add a clock and shift every bit after it
		fork
			m_out(8'hC3);
			begin #(T * 3 + T / 2); glitch = 1; #50; glitch = 0; end
		join
		m_stop;
		check("data byte ACKed through the glitch", acked, 1);
		expect_log("the glitch did not shift the byte", 2'd1, 8'h10, 8'hC3);

		// 11. ownership: DIR/OUT drive every other pin, not SCL/SDA
		wb_write(W_PORT0 + 1, 32'h00);		// OUT
		wb_write(W_PORT0 + 0, 32'hFF);		// DIR
		repeat (2) @(posedge clk);
		check("DIR reaches pins 0,1,4-7 but not SCL/SDA", gpio_dir[7:0], 8'hF3);
		// and the bus still works around it
		m_start; m_out({ 7'h20, 1'b0 }); m_stop;
		check("probe with DIR set", acked, 1);
		wb_write(W_PORT0 + 0, 32'h00);

		// 10. overflow, at 400 kHz: 512 entries fit, the 513th does not
		T = 625;
		m_start; m_out({ 7'h50, 1'b0 }); m_out(8'h00);
		// byte i carries i/2, so the 513th (0x00) differs from the
		// 257th (0x80) that wrote register 0 before it
		for (i = 0; i < 513; i = i + 1) m_out(i[8:1]);
		m_stop;
		wb_read(W_ISTAT);
		check("full: 512 logged, overflow", { rdata[12], rdata[9:0] }, { 1'b1, 10'd512 });
		expect_log("the first of them", 2'd1, 8'h00, 8'h00);
		wb_write(W_IFLUSH, 32'd1);
		wb_read(W_ISTAT);
		check("IFLUSH: empty, flag clear", { rdata[12], rdata[9:0] }, 11'd0);
		// the mirror took every byte, logged or not: register 255 was
		// written last by byte 511 (0xFF), register 0 by the 513th (0x00)
		wb_write(W_IMADDR, { 2'd1, 8'd255 });
		wb_read(W_IMDATA);
		check("mirror reg 255: byte 511", rdata, { 23'd0, 1'b1, 8'hFF });
		wb_write(W_IMADDR, { 2'd1, 8'd0 });
		wb_read(W_IMDATA);
		check("mirror reg 0: the unlogged 513th byte", rdata, { 23'd0, 1'b1, 8'h00 });

		// 13. disabled: no answer, pins let go
		wb_write(W_ICTL, 32'd0);
		m_start; m_out({ 7'h20, 1'b0 }); m_stop;
		check("disabled: probe not ACKed", acked, 0);
		wb_write(W_PORT0 + 0, 32'h0C);
		repeat (2) @(posedge clk);
		check("disabled: DIR reaches SCL/SDA again", gpio_dir[3:2], 2'b11);
		wb_write(W_PORT0 + 0, 32'h00);

		if (errors == 0) $display("tb_gpio_i2cs: PASS");
		else $display("tb_gpio_i2cs: %0d FAILED", errors);
		$finish;
	end

endmodule
