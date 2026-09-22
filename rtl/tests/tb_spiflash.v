/*
 * tb_spiflash.v -- rtl/spiflash.v against a behavioural W25Q16
 * (w25q16_model.v). `make spiflash` in rtl/tests.
 *
 * The bus master behaves as rtl/sysctl.v's does: a byte address with
 * bits 27:0 of the 0x1 nibble, one transaction at a time, strobe held
 * until ack. Every check prints ok or FAIL; the run ends with the
 * model's three tripwires, which must all read zero.
 */
`timescale 1ns/1ps
module tb_spiflash;

	reg clk = 0;
	always #10.4 clk = ~clk;			// ~48 MHz
	reg rst = 1;

	reg [31:0] adr, dat_w;
	reg [3:0] sel;
	reg we, cyc, stb;
	wire [31:0] dat_r;
	wire ack;
	wire ss, sck, mosi, miso;

	spiflash_wb dut (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
		.wb_sel_i(sel), .wb_we_i(we), .wb_cyc_i(cyc), .wb_stb_i(stb), .wb_ack_o(ack),
		.ss(ss), .sck(sck), .mosi(mosi), .miso(miso)
	);
	w25q16_model flash (.cs_n(ss), .sck(sck), .mosi(mosi), .miso(miso));

	integer fails = 0, checks = 0;

	task check(input cond, input [8*72-1:0] what);
		begin
			checks = checks + 1;
			if (cond) $display("ok   %0s", what);
			else begin $display("FAIL %0s", what); fails = fails + 1; end
		end
	endtask

	// the bus: byte addresses within the 0x1 nibble, as sysctl passes them
	task bus_read(input [31:0] a, output [31:0] d);
		begin
			@(posedge clk);
			adr <= a & 32'h0FFF_FFFF; we <= 0; sel <= 4'hF; cyc <= 1; stb <= 1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			d = dat_r;
			cyc <= 0; stb <= 0;
		end
	endtask
	task bus_write(input [31:0] a, input [31:0] d, input [3:0] s);
		begin
			@(posedge clk);
			adr <= a & 32'h0FFF_FFFF; dat_w <= d; we <= 1; sel <= s; cyc <= 1; stb <= 1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			cyc <= 0; stb <= 0; we <= 0;
		end
	endtask

	localparam R = 32'h1F00_0000;
	localparam MAGIC = R + 32'h00, STATUS = R + 32'h04, ID = R + 32'h08, LOCK = R + 32'h0C,
	           ADDR = R + 32'h10, ARM = R + 32'h14, CMD = R + 32'h18, LEN = R + 32'h1C,
	           BUF = R + 32'h100;
	localparam KEY = 32'h5A46_5752;

	reg [31:0] d, st;
	integer i, t0;
	reg [31:0] pattern [0:63];

	task wait_done(output [31:0] s);
		begin
			bus_read(STATUS, s);
			while (s[0]) bus_read(STATUS, s);
		end
	endtask

	initial begin
		cyc = 0; stb = 0; we = 0; sel = 0; adr = 0; dat_w = 0;
		// known contents: byte at a is a[7:0] ^ a[15:8] ^ 8'h5A
		for (i = 0; i < 2 * 1024 * 1024; i = i + 1) flash.mem[i] = i[7:0] ^ i[15:8] ^ 8'h5A;
		repeat (4) @(posedge clk);
		rst = 0;

		// -- identity ----------------------------------------------------------
		bus_read(MAGIC, d);
		check(d == 32'h5A46_4C53, "MAGIC reads ZFLS");
		bus_read(STATUS, st);
		check(st[0], "busy from reset, while the ID is read");
		wait_done(st);
		bus_read(ID, d);
		check(d == 32'h00EF_4015, "ID: JEDEC EF 40 15, read at reset");
		bus_read(LOCK, d);
		check(d == 32'h0004_0000, "LOCK reads 0x040000");

		// -- the window, as spiflashro served it --------------------------------
		bus_read(32'h1000_0000, d);
		check(d == { 8'h5A ^ 8'h03, 8'h5A ^ 8'h02, 8'h5A ^ 8'h01, 8'h5A ^ 8'h00 },
			"window read at 0x1000_0000, little-endian");
		bus_read(32'h1012_3454, d);
		check(d == { 8'h57 ^ 8'h34 ^ 8'h5A, 8'h56 ^ 8'h34 ^ 8'h5A, 8'h55 ^ 8'h34 ^ 8'h5A, 8'h54 ^ 8'h34 ^ 8'h5A },
			"window read at 0x1012_3454");
		bus_write(32'h1000_0000, 32'hDEAD_BEEF, 4'hF);
		check(1, "a write to the window is acknowledged (it used to hang the bus)");
		bus_read(32'h1000_0000, d);
		check(d == 32'h5958_5B5A && flash.n_pp == 0 && flash.n_se == 0,
			"... and ignored: the flash is untouched");

		// -- refusals: nothing may reach the pins ----------------------------
		bus_write(ADDR, 32'h1D_0000, 4'hF);
		bus_write(CMD, 1, 4'hF);
		bus_read(STATUS, st);
		check(st[3] && !st[0] && flash.n_se == 0, "erase without arming: refused, nothing sent");

		bus_write(ARM, 32'h1234_5678, 4'hF);
		bus_write(CMD, 1, 4'hF);
		bus_read(STATUS, st);
		check(st[3] && flash.n_se == 0, "a wrong key does not arm");

		bus_write(ARM, KEY, 4'b0011);
		bus_write(CMD, 1, 4'hF);
		bus_read(STATUS, st);
		check(st[3] && flash.n_se == 0, "the key as a partial store does not arm");

		bus_write(ADDR, 32'h0, 4'hF);
		bus_write(ARM, KEY, 4'hF);
		bus_write(CMD, 1, 4'hF);
		bus_read(STATUS, st);
		check(st[2] && !st[0] && flash.n_se == 0 && flash.n_wren == 0,
			"erase of sector 0 (the bootloader): refused, nothing sent");

		bus_write(ADDR, 32'h03_FFFF, 4'hF);
		bus_write(ARM, KEY, 4'hF);
		bus_write(CMD, 1, 4'hF);
		bus_read(STATUS, st);
		check(st[2] && flash.n_se == 0, "erase at 0x03FFFF, the lock's last byte: refused");

		bus_write(ADDR, 32'h00_0100, 4'hF);
		bus_write(LEN, 16, 4'hF);
		bus_write(ARM, KEY, 4'hF);
		bus_write(CMD, 2, 4'hF);
		bus_read(STATUS, st);
		check(st[2] && flash.n_pp == 0, "program at 0x000100: refused, nothing sent");

		// -- erase ---------------------------------------------------------------------
		bus_write(ADDR, 32'h1D_0123, 4'hF);		// anywhere in the sector
		bus_write(ARM, KEY, 4'hF);
		bus_write(CMD, 1, 4'hF);
		bus_read(STATUS, st);
		check(st[0], "erase at 0x1D0123: busy");
		// a window read while it erases waits, and sees the result
		t0 = $time;
		bus_read(32'h101D_0000, d);
		check(d == 32'hFFFF_FFFF && ($time - t0) > 15000,
			"a window read during the erase waits for it, and reads erased");
		wait_done(st);
		check(st[1] && !st[2] && !st[3], "erase: done, no errors");
		check(flash.mem[24'h1CFFFF] != 8'hFF && flash.mem[24'h1D1000] != 8'hFF,
			"erase touched only its own 4 KB sector");

		bus_write(CMD, 1, 4'hF);
		bus_read(STATUS, st);
		check(st[3], "arming is one-shot: a second erase is refused");

		// -- program ---------------------------------------------------------------------
		for (i = 0; i < 64; i = i + 1) begin
			pattern[i] = { 8'(4*i + 3), 8'(4*i + 2), 8'(4*i + 1), 8'(4*i) } ^ 32'hA5C3_0F96;
			bus_write(BUF + 4 * i, pattern[i], 4'hF);
		end
		bus_read(BUF + 4 * 17, d);
		check(d == 0, "the page buffer is write-only: reads as 0");
		bus_write(ADDR, 32'h1D_0000, 4'hF);
		bus_write(LEN, 256, 4'hF);
		bus_write(ARM, KEY, 4'hF);
		bus_write(CMD, 2, 4'hF);
		wait_done(st);
		check(st[1] && !st[4], "program 256 bytes at 0x1D0000: done");
		d = 0;
		for (i = 0; i < 64; i = i + 1) begin
			bus_read(32'h101D_0000 + 4 * i, st);
			if (st != pattern[i]) begin
				if (d < 4) $display("     word %0d: read %08x, wrote %08x; flash bytes %02x %02x %02x %02x", i, st, pattern[i],
					flash.mem[24'h1D0000 + 4*i], flash.mem[24'h1D0000 + 4*i + 1], flash.mem[24'h1D0000 + 4*i + 2], flash.mem[24'h1D0000 + 4*i + 3]);
				d = d + 1;
			end
		end
		check(d == 0, "all 256 bytes read back through the window");

		bus_write(ADDR, 32'h1D_01F8, 4'hF);
		bus_write(LEN, 16, 4'hF);
		bus_write(ARM, KEY, 4'hF);
		bus_write(CMD, 2, 4'hF);
		bus_read(STATUS, st);
		check(st[4] && flash.n_pp == 1, "a program crossing a page boundary: refused");

		bus_write(ADDR, 32'h1D_0104, 4'hF);
		bus_write(LEN, 4, 4'hF);
		bus_write(ARM, KEY, 4'hF);
		bus_write(CMD, 2, 4'hF);
		wait_done(st);
		bus_read(32'h101D_0104, d);
		check(d == pattern[0], "a 4-byte program mid-page: the buffer's first word lands there");

		// -- the pins, released between commands ----------------------------------
		repeat (40) @(posedge clk);
		check(sck === 1'bz && mosi === 1'bz && ss === 1'b1, "SCK and MOSI released, chip select high, when idle");

		// -- the tripwires ----------------------------------------------------------
		check(flash.illegal == 0, "the flash saw no command but 03 05 06 20 02 9F");
		check(flash.prot_hits == 0, "the flash saw no erase or program below 0x040000");
		check(flash.while_busy == 0, "the flash saw no command but RDSR while busy");

		$display("%0d checks, %0d failed (%0d reads, %0d erases, %0d programs, %0d WREN, %0d status polls)",
			checks, fails, flash.n_read, flash.n_se, flash.n_pp, flash.n_wren, flash.n_rdsr);
		$finish;
	end

	initial begin
		#20000000;
		$display("FAIL timeout");
		$finish;
	end

endmodule
