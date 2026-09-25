/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 controller testbench (phase 1)
 *
 * Runs the real rtl/mem/ddr3_ctrl.v against rtl/tb/ddr3_model.v, a
 * DDR3 model that checks the protocol rather than merely storing
 * data, through a behavioural PHY. Every timing and sequencing
 * complaint printed by the model counts as a failure here.
 *
 * Run:
 *   iverilog -g2012 -o tb tb_ddr3.v ddr3_model.v ddr3_phy_sim.v \
 *       ../mem/ddr3_ctrl.v
 *   ./tb
 */

`timescale 1ps/1ps

module tb_ddr3;

	localparam TCK_PS = 10417;          // 96MHz DRAM clock
	localparam ROW_BITS = 15;
	localparam COL_BITS = 10;
	localparam BANK_BITS = 3;
	localparam ROW_ALIAS = 6;

	reg ck;
	reg clk;
	reg rst;

	wire [26:0] wb_adr;
	reg [26:0] wb_adr_r;
	reg [31:0] wb_dat_w;
	wire [31:0] wb_dat_r;
	reg wb_we;
	reg [3:0] wb_sel;
	reg wb_stb;
	reg wb_cyc;
	wire wb_ack;

	assign wb_adr = wb_adr_r;

	wire init_done;

	wire c_reset_n, c_cke, c_odt, c_cs_n, c_ras_n, c_cas_n, c_we_n;
	wire [15:0] c_a;
	wire [2:0] c_ba;
	wire c_wren, c_rden, c_rvalid;
	wire [127:0] c_wdata, c_rdata;
	wire [15:0] c_wmask;

	wire d_reset_n, d_cke, d_odt, d_cs_n, d_ras_n, d_cas_n, d_we_n;
	wire [15:0] d_a;
	wire [2:0] d_ba;
	wire [1:0] d_dm;
	wire [15:0] d_dq;
	wire [1:0] d_dqs;

	integer errors;
	integer i;
	reg [31:0] expect_dat;
	reg [31:0] got;
	integer c0, c1, w;
	reg wr_noread = 1'b0;
	reg rdbuf_off = 1'b0;
	reg [15:0] pk;
	integer bb;
	reg [25:0] addr_list [0:15];
	reg [31:0] data_list [0:15];

	// -- clocks ---------------------------------------------------
	// The controller clock is exactly half the DRAM clock and rises
	// with it, which is what the ECP5 CLKDIVF arrangement produces on
	// hardware (see docs/ddr3.md).
	initial begin
		ck = 1'b0;
		forever #(TCK_PS / 2) ck = ~ck;
	end

	initial begin
		clk = 1'b0;
		@(negedge ck);
		forever begin
			@(posedge ck);
			clk = 1'b1;
			@(posedge ck);
			clk = 1'b0;
		end
	end

	// -- DUT ------------------------------------------------------
	ddr3_ctrl #(
		.ROW_BITS(ROW_BITS),
		.COL_BITS(COL_BITS),
		.BANK_BITS(BANK_BITS),
		.TRFC_NS(260),
		.CLK_MHZ(48),
		// Power-on waits are supply settling times, not DRAM
		// internals; shortening them costs no coverage and saves
		// well over a million cycles of simulation.
		.INIT_DIV(1000)
	) dut (
		.clk_i(clk),
		.rst_i(rst),
		.wb_adr_i(wb_adr),
		.wb_dat_i(wb_dat_w),
		.wb_dat_o(wb_dat_r),
		.wb_we_i(wb_we),
		.wb_sel_i(wb_sel),
		.wb_stb_i(wb_stb),
		.wb_cyc_i(wb_cyc),
		.wb_ack_o(wb_ack),
		.phy_ready_i(1'b1),
		.init_done_o(init_done),
		.rd_timeout_o(),
		.no_refresh_i(1'b0),
		.wr_noread_i(wr_noread),
		.rdbuf_off_i(rdbuf_off),
		.phy_reset_n_o(c_reset_n),
		.phy_cke_o(c_cke),
		.phy_odt_o(c_odt),
		.phy_cs_n_o(c_cs_n),
		.phy_ras_n_o(c_ras_n),
		.phy_cas_n_o(c_cas_n),
		.phy_we_n_o(c_we_n),
		.phy_a_o(c_a),
		.phy_ba_o(c_ba),
		.phy_wren_o(c_wren),
		.phy_wdata_o(c_wdata),
		.phy_wmask_o(c_wmask),
		.phy_rden_o(c_rden),
		.phy_rvalid_i(c_rvalid),
		.phy_rdata_i(c_rdata)
	);

	ddr3_phy_sim #(
		.TCK_PS(TCK_PS),
		.CL(6),
		.CWL(6)
	) phy (
		.clk_i(clk),
		.ck_i(ck),
		.rst_i(rst),
		.ctl_reset_n_i(c_reset_n),
		.ctl_cke_i(c_cke),
		.ctl_odt_i(c_odt),
		.ctl_cs_n_i(c_cs_n),
		.ctl_ras_n_i(c_ras_n),
		.ctl_cas_n_i(c_cas_n),
		.ctl_we_n_i(c_we_n),
		.ctl_a_i(c_a),
		.ctl_ba_i(c_ba),
		.ctl_wren_i(c_wren),
		.ctl_wdata_i(c_wdata),
		.ctl_wmask_i(c_wmask),
		.ctl_rden_i(c_rden),
		.ctl_rvalid_o(c_rvalid),
		.ctl_rdata_o(c_rdata),
		.ddr_reset_n(d_reset_n),
		.ddr_cke(d_cke),
		.ddr_odt(d_odt),
		.ddr_cs_n(d_cs_n),
		.ddr_ras_n(d_ras_n),
		.ddr_cas_n(d_cas_n),
		.ddr_we_n(d_we_n),
		.ddr_a(d_a),
		.ddr_ba(d_ba),
		.ddr_dm(d_dm),
		.ddr_dq(d_dq),
		.ddr_dqs(d_dqs)
	);

	ddr3_model #(
		.ROW_BITS(ROW_BITS),
		.COL_BITS(COL_BITS),
		.BANK_BITS(BANK_BITS),
		.ROW_ALIAS(ROW_ALIAS),
		.TCK_PS(TCK_PS)
	) dram (
		.ck(ck),
		.reset_n(d_reset_n),
		.cke(d_cke),
		.cs_n(d_cs_n),
		.ras_n(d_ras_n),
		.cas_n(d_cas_n),
		.we_n(d_we_n),
		.a(d_a),
		.ba(d_ba),
		.dm(d_dm),
		.dq(d_dq),
		.dqs(d_dqs)
	);

	// -- bus tasks ------------------------------------------------
	task wb_write(input [25:0] addr, input [31:0] data, input [3:0] sel);
	begin
		@(posedge clk);
		wb_adr_r <= addr;
		wb_dat_w <= data;
		wb_sel <= sel;
		wb_we <= 1'b1;
		wb_stb <= 1'b1;
		wb_cyc <= 1'b1;
		@(posedge clk);
		while (!wb_ack) @(posedge clk);
		wb_stb <= 1'b0;
		wb_cyc <= 1'b0;
		wb_we <= 1'b0;
		@(posedge clk);
	end
	endtask

	task wb_read(input [25:0] addr, output [31:0] data);
	begin
		@(posedge clk);
		wb_adr_r <= addr;
		wb_sel <= 4'hf;
		wb_we <= 1'b0;
		wb_stb <= 1'b1;
		wb_cyc <= 1'b1;
		@(posedge clk);
		while (!wb_ack) @(posedge clk);
		data = wb_dat_r;
		wb_stb <= 1'b0;
		wb_cyc <= 1'b0;
		@(posedge clk);
	end
	endtask

	task check(input [1023:0] name, input [31:0] exp, input [31:0] act);
	begin
		if (exp !== act) begin
			$display("  FAIL %0s: expected %08h, got %08h", name, exp, act);
			errors = errors + 1;
		end else begin
			$display("  ok   %0s: %08h", name, act);
		end
	end
	endtask

	// -- latency measurement --------------------------------------
	integer t0, t1;
	task timed_read(input [25:0] addr, output [31:0] data, output integer cycles);
	begin
		@(posedge clk);
		t0 = $time;
		wb_adr_r <= addr;
		wb_sel <= 4'hf;
		wb_we <= 1'b0;
		wb_stb <= 1'b1;
		wb_cyc <= 1'b1;
		@(posedge clk);
		while (!wb_ack) @(posedge clk);
		t1 = $time;
		data = wb_dat_r;
		wb_stb <= 1'b0;
		wb_cyc <= 1'b0;
		cycles = (t1 - t0) / (TCK_PS * 2);
		@(posedge clk);
	end
	endtask

	integer rd_cycles;
	integer wr_cycles;

	initial begin
		errors = 0;
		rst = 1'b1;
		wb_adr_r = 26'd0;
		wb_dat_w = 32'd0;
		wb_we = 1'b0;
		wb_sel = 4'h0;
		wb_stb = 1'b0;
		wb_cyc = 1'b0;

		repeat (10) @(posedge clk);
		rst = 1'b0;

		$display("");
		$display("== waiting for DDR3 initialisation ==");
		wait (init_done);
		$display("[%0t] controller reports init_done", $time);

		if (dram.errors != 0) begin
			$display("model reported %0d error(s) during init", dram.errors);
			errors = errors + dram.errors;
		end

		// -- basic write/read --------------------------------------
		$display("");
		$display("== single word write/read ==");
		wb_write(26'h000_0000, 32'hdeadbeef, 4'hf);
		wb_read(26'h000_0000, got);
		check("word 0", 32'hdeadbeef, got);

		// -- all four words of one burst ---------------------------
		// The four words share a 16-byte burst, so this also proves
		// that DM is masking correctly: each write must leave its
		// three neighbours alone.
		$display("");
		$display("== four words within one BL8 burst ==");
		wb_write(26'h000_0000, 32'h11111111, 4'hf);
		wb_write(26'h000_0001, 32'h22222222, 4'hf);
		wb_write(26'h000_0002, 32'h33333333, 4'hf);
		wb_write(26'h000_0003, 32'h44444444, 4'hf);
		wb_read(26'h000_0000, got); check("burst word 0", 32'h11111111, got);
		wb_read(26'h000_0001, got); check("burst word 1", 32'h22222222, got);
		wb_read(26'h000_0002, got); check("burst word 2", 32'h33333333, got);
		wb_read(26'h000_0003, got); check("burst word 3", 32'h44444444, got);

		// -- training writes: no read at all ---------------------
		//
		// The mode exists for a read path that does not work yet, so
		// it is checked WITHOUT trusting a read: the block register
		// is first loaded with a different block, then the target
		// block is written as four words with reads skipped, and the
		// DRAM model's own memory is inspected directly.
		$display("");
		$display("== training writes, verified by backdoor ==");
		wb_read(26'h000_0500, got);            // register holds 0x500
		wr_noread = 1'b1;
		wb_write(26'h000_0600, 32'h00020001, 4'hf);
		wb_write(26'h000_0601, 32'h00040003, 4'hf);
		wb_write(26'h000_0602, 32'h00060005, 4'hf);
		wb_write(26'h000_0603, 32'h00080007, 4'hf);
		wr_noread = 1'b0;
		repeat (40) @(posedge clk);
		// 0x600 words = block 0x180: column (0x180 & 0x7f)*8, bank
		// (0x180 >> 7) & 7, row 0.
		for (bb = 0; bb < 8; bb = bb + 1) begin
			pk = dram.mem[((3'd3) << (6 + 10)) | ((0) << 10) | ((0) * 8 + bb)];
			if (pk !== bb + 1) begin
				$display("  FAIL training write beat %0d: model holds %h, want %h", bb, pk, bb + 1);
				errors = errors + 1;
			end
		end
		$display("  checked 8 beats in the model directly");

		// -- read-buffer bypass reaches the DRAM ------------------
		//
		// Make the DRAM and the block register DISAGREE: write a
		// block, then change the DRAM behind the controller's back.
		// A read then says which one answered. The first hardware
		// scan passed at every setting because the register answered
		// every training read -- this is the test that would have
		// caught it.
		$display("");
		$display("== read-buffer bypass ==");
		wb_write(26'h000_0700, 32'h11110000, 4'hf);
		wb_write(26'h000_0701, 32'h11110001, 4'hf);
		wb_write(26'h000_0702, 32'h11110002, 4'hf);
		wb_write(26'h000_0703, 32'h11110003, 4'hf);
		repeat (40) @(posedge clk);
		// word 0x700: block 0x1c0 -> bank 3, column (0x1c0 & 0x7f)*8
		dram.mem[(3 << 16) | (8'h40 * 8)]     = 16'hbeef;
		dram.mem[(3 << 16) | (8'h40 * 8) + 1] = 16'hdead;
		wb_read(26'h000_0700, got);
		check("buffer answers when allowed", 32'h11110000, got);
		rdbuf_off = 1'b1;
		wb_read(26'h000_0700, got);
		check("bypass reads the DRAM", 32'hdeadbeef, got);
		rdbuf_off = 1'b0;

		// -- every beat distinct -------------------------------------
		//
		// Eight beats, eight different values. A burst shifted by one
		// beat, in either direction, fails this -- and it is the ONLY
		// test here that is guaranteed to.
		//
		// 44444444 has identical halves, so a beat that takes its
		// neighbour's value looks correct. The doubles had exactly
		// such a shift on BOTH the read and the write path, in
		// opposite directions, and every symmetric pattern passed
		// through the two cancelling errors untouched. A test finds
		// faults only along the axes its pattern varies on.
		$display("");
		$display("== every beat distinct ==");
		wb_write(26'h000_0400, 32'h00020001, 4'hf);
		wb_write(26'h000_0401, 32'h00040003, 4'hf);
		wb_write(26'h000_0402, 32'h00060005, 4'hf);
		wb_write(26'h000_0403, 32'h00080007, 4'hf);
		wb_read(26'h000_0400, got); check("beats 1,0", 32'h00020001, got);
		wb_read(26'h000_0401, got); check("beats 3,2", 32'h00040003, got);
		wb_read(26'h000_0402, got); check("beats 5,4", 32'h00060005, got);
		wb_read(26'h000_0403, got); check("beats 7,6", 32'h00080007, got);

		// -- byte enables ------------------------------------------
		$display("");
		$display("== byte enables ==");
		wb_write(26'h000_0010, 32'h00000000, 4'hf);
		wb_write(26'h000_0010, 32'haabbccdd, 4'b0011);
		wb_read(26'h000_0010, got);
		check("low half only", 32'h0000ccdd, got);
		wb_write(26'h000_0010, 32'h11223344, 4'b1100);
		wb_read(26'h000_0010, got);
		check("high half only", 32'h1122ccdd, got);
		wb_write(26'h000_0010, 32'hff00ff00, 4'b0010);
		wb_read(26'h000_0010, got);
		check("one byte only", 32'h1122ffdd, got);

		// -- address decode: banks, rows, columns ------------------
		$display("");
		$display("== address decode across banks/rows/columns ==");
		for (i = 0; i < 16; i = i + 1) begin
			// Spread across column, bank and row fields. Rows are
			// kept under 2**ROW_ALIAS so the model does not alias.
			addr_list[i] = (i * 26'h0000_0801) & 26'h0003_ffff;
			data_list[i] = 32'ha5a50000 + i;
			wb_write(addr_list[i], data_list[i], 4'hf);
		end
		for (i = 0; i < 16; i = i + 1) begin
			wb_read(addr_list[i], got);
			check("decode", data_list[i], got);
		end

		// -- refresh -----------------------------------------------
		// Idle long enough that several refreshes must have been
		// issued, then confirm data survived and the model saw them.
		$display("");
		$display("== refresh ==");
		wb_write(26'h000_0100, 32'hcafef00d, 4'hf);
		#(40 * 7800000);
		wb_read(26'h000_0100, got);
		check("data across refresh", 32'hcafef00d, got);
		if (dram.refreshes < 4) begin
			$display("  FAIL: only %0d refreshes issued in 40 tREFI periods",
				dram.refreshes);
			errors = errors + 1;
		end else begin
			$display("  ok   %0d refreshes issued", dram.refreshes);
		end

		// -- latency -----------------------------------------------
		$display("");
		$display("== access latency ==");
		timed_read(26'h000_0200, got, rd_cycles);
		$display("  random read: %0d controller cycles (%0d ns at 48MHz)",
			rd_cycles, (rd_cycles * 1000) / 48);

		// A four-word line fill, which is what the data cache asks
		// for and exactly one BL8 burst of the DRAM.
		//
		// The controller keeps the whole sixteen bytes of the burst
		// it just read, so the three words after the first are
		// answered from a register. Without that, the same burst is
		// fetched from the DRAM four times.
		begin
			wb_write(26'h000_0300, 32'h11111111, 4'hf);
			wb_write(26'h000_0301, 32'h22222222, 4'hf);
			wb_write(26'h000_0302, 32'h33333333, 4'hf);
			wb_write(26'h000_0303, 32'h44444444, 4'hf);

			timed_read(26'h000_0300, got, c0);
			check("line fill word 0", 32'h11111111, got);
			rd_cycles = 0;
			for (w = 1; w < 4; w = w + 1) begin
				timed_read(26'h000_0300 + w[25:0], got, c1);
				rd_cycles = rd_cycles + c1;
			end
			check("line fill word 3", 32'h44444444, got);
			$display("  line fill: %0d cycles for the first word, %0d for the next three",
				c0, rd_cycles);
			if (rd_cycles >= c0)
				$display("  NOTE: the read buffer is not helping");
		end

		// -- results -----------------------------------------------
		errors = errors + dram.errors;
		$display("");
		if (errors == 0)
			$display("== PASS: no errors ==");
		else
			$display("== FAIL: %0d error(s) ==", errors);
		$display("");
		$finish;
	end

	// Watchdog: a controller that never acknowledges would otherwise
	// hang the run rather than failing it.
	initial begin
		#(500 * 1000 * 1000);
		$display("== FAIL: timeout ==");
		$finish;
	end

endmodule
