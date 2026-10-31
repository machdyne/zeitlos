/*
 * rtl/usb_hid.v's pointer speed (register 5): the signature, the reset
 * value, writes acked and read back, and the cursor moving by
 * delta * m / 16 with the remainder carried -- the same cases as
 * tb_usb_host.v's case 9 for the USB host controller's compat block.
 * The HID host engine (rtl/ext/usb_hid_host) is in the build but its
 * report is forced: what is under test is what this block does with one.
 *
 *   make test_usb_hid
 */
`timescale 1ns/1ps
module tb;
	reg clk = 0, rst = 0;
	always #10 clk = ~clk;
	reg [31:0] adr = 0, dat_i = 0; reg we = 0, stb = 0, cyc = 0;
	wire [31:0] dat_o; wire ack; wire [9:0] cx, cy; wire [1:0] typ; wire irq;
	wire dm, dp;
	usb_hid_wb dut (.wb_clk_i(clk), .wb_rst_i(rst), .wb_adr_i(adr), .wb_dat_i(dat_i),
		.wb_dat_o(dat_o), .wb_we_i(we), .wb_sel_i(4'hf), .wb_stb_i(stb), .wb_ack_o(ack),
		.wb_cyc_i(cyc), .usb_clk(clk), .usb_dm(dm), .usb_dp(dp), .int_o(irq),
		.curs_x(cx), .curs_y(cy), .typ(typ));
	integer fails = 0;
	task bus(input w, input [2:0] a, input [31:0] d);
		begin
			@(posedge clk); adr = a; dat_i = d; we = w; stb = 1; cyc = 1;
			@(posedge clk); while (!ack) @(posedge clk);
			stb = 0; cyc = 0; we = 0;
		end
	endtask
	task report(input signed [7:0] x, input signed [7:0] y);
		begin
			force dut.uhh_mouse_dx = x; force dut.uhh_mouse_dy = y;
			@(posedge clk); force dut.uhh_report = 1;
			repeat (4) @(posedge clk); force dut.uhh_report = 0;
			repeat (12) @(posedge clk);
		end
	endtask
	task check(input [8*24:1] what, input integer got, input integer want);
		begin
			if (got != want) begin fails = fails + 1; $display("FAIL %0s: %0d, want %0d", what, got, want); end
			else $display("ok   %0s = %0d", what, got);
		end
	endtask
	integer x0, y0;
	initial begin
		force dut.uhh_report = 0;
		rst = 1; repeat (3) @(posedge clk); rst = 0;
		force dut.curs_x = 320; force dut.curs_y = 240; @(posedge clk);
		release dut.curs_x; release dut.curs_y;
		bus(0, 0, 0); bus(0, 5, 0);
		check("signature", dat_o[31:8], 24'h535045);
		check("speed resets 16", dat_o[5:0], 16);
		x0 = cx; y0 = cy; report(2, 3);
		check("16: x 1:1", cx - x0, 2); check("16: y 1:1", cy - y0, 3);
		bus(1, 5, 32); bus(0, 5, 0);
		check("write acked, reads 32", dat_o[5:0], 32);
		x0 = cx; y0 = cy; report(2, 3);
		check("32: x doubled", cx - x0, 4); check("32: y doubled", cy - y0, 6);
		bus(1, 5, 8);
		x0 = cx; report(3, 0); check("8: half of 3", cx - x0, 1);
		report(3, 0); check("8: carried", cx - x0, 3);
		bus(1, 5, 20); x0 = cx;
		report(1, 0); report(1, 0); report(1, 0); report(1, 0);
		check("20: four 1s are 5", cx - x0, 5);
		bus(1, 5, 0); x0 = cx; report(2, 0); check("0 reads as 16", cx - x0, 2);
		bus(1, 5, 63); x0 = cx; report(-100, 0); report(-100, 0); report(-100, 0);
		check("saturates at 0", cx, 0);
		if (fails) $display("FAIL -- %0d", fails);
		else $display("PASS -- usb_hid pointer speed");
		$finish;
	end
endmodule
