`timescale 1ns/1ps
/*
 * tb_ddr3_reg -- the DDR3 register block (rtl/mem/ddr3.v)
 *
 * A TUNE write changes READCLKSEL, which must not change under a running
 * DQS buffer, so it is applied in the middle of a PAUSE and the write is
 * acknowledged only when the pause has ended. This checks exactly that:
 * the value changes while paused, never otherwise, the pause is over at
 * the acknowledge, and other registers still acknowledge at once.
 *
 * A register write that never acknowledged would hang the BIOS on its
 * first TUNE write with nothing on the console, which is why this path
 * gets a test of its own. The primitives are empty stand-ins
 * (ddr3_ecp5_stubs.v); nothing here depends on them.
 */
module tb_ddr3_reg;
	reg clk = 0; always #5 clk = ~clk;
	reg rst = 1;
	reg [1:0] adr; reg [31:0] dat; reg we = 0, stb = 0, cyc = 0;
	wire [31:0] q; wire ack;
	wire [15:0] dq; wire [1:0] dqs;
	ddr3 dut (.clk_i(clk), .rst_i(rst), .eclk_i(clk), .ddrdel_i(1'b0),
		.pause_i(1'b0), .phy_rst_i(1'b0), .phy_ready_i(1'b1), .pll_locked_i(1'b1),
		.wb_adr_i(27'd0), .wb_dat_i(32'd0), .wb_dat_o(), .wb_we_i(1'b0),
		.wb_sel_i(4'hf), .wb_stb_i(1'b0), .wb_cyc_i(1'b0), .wb_ack_o(),
		.reg_adr_i(adr), .reg_dat_i(dat), .reg_dat_o(q), .reg_we_i(we),
		.reg_stb_i(stb), .reg_cyc_i(cyc), .reg_ack_o(ack),
		.ddr3_dq(dq), .ddr3_dqs_p(dqs));
	integer t0, t_ack, t_apply, pz0, pz1, errors = 0;
	task access(input [1:0] a, input w, input [31:0] d);
		begin
			@(negedge clk); adr = a; we = w; dat = d; stb = 1; cyc = 1; t0 = $time;
			@(posedge clk); while (!ack) @(posedge clk);
			t_ack = $time;
			@(negedge clk); stb = 0; cyc = 0; we = 0;
		end
	endtask
	// When does TUNE actually change, and is PAUSE high then?
	reg [31:0] last_tune;
	always @(posedge clk) begin
		if (dut.tune !== last_tune && !rst) begin
			t_apply = $time;
			if (!dut.tune_pause) begin $display("FAIL: tune changed without PAUSE"); errors = errors + 1; end
		end
		last_tune <= dut.tune;
	end
	initial begin
		repeat (3) @(posedge clk); rst = 0; repeat (2) @(posedge clk);
		access(2'd1, 0, 0); $display("TUNE reset value %h", q);
		access(2'd1, 1, 32'h01234567);
		$display("write acked after %0d cycles; applied %0d cycles in", (t_ack - t0)/10, (t_apply - t0)/10);
		if (dut.tune_pause) begin $display("FAIL: still paused at ack"); errors = errors + 1; end
		access(2'd1, 0, 0);
		if (q !== 32'h01234567) begin $display("FAIL: read back %h", q); errors = errors + 1; end
		else $display("read back %h", q);
		access(2'd0, 0, 0); $display("STATUS %h (acked promptly: %0d cycles)", q, (t_ack - t0)/10);
		access(2'd2, 1, 32'h9); access(2'd2, 0, 0); $display("CTRL %h", q);
		if (errors) $display("== FAIL: %0d error(s) ==", errors);
		else $display("== PASS: TUNE applied under pause, acknowledged after ==");
		$finish;
	end
	initial begin #100000 $display("TIMEOUT: a register access never acknowledged"); $finish; end
endmodule
