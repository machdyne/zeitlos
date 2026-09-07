`timescale 1ns/1ps
module tb;
	reg clk=0, resetn=0;
	reg [31:0] adr=0, dat_w=0;
	reg we=0, stb=0, cyc=0;
	wire [31:0] dat_r; wire ack;
	montmul #(.LIMBS(12)) dut (.clk(clk), .resetn(resetn),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
		.wb_we_i(we), .wb_sel_i(4'hf), .wb_stb_i(stb),
		.wb_ack_o(ack), .wb_cyc_i(cyc));
	always #10 clk = ~clk;
	integer n, k;
	task wbw(input [31:0] a, input [31:0] d);
		begin
			while (ack) @(posedge clk);
			adr=a; dat_w=d; we=1; stb=1; cyc=1;
			while (!ack) @(posedge clk);
			@(posedge clk); stb=0; cyc=0; we=0; @(posedge clk);
		end
	endtask
	initial begin
		repeat(4) @(posedge clk); resetn=1; repeat(2) @(posedge clk);
		wbw(3, 32'h00000001);
		for (k=0;k<12;k=k+1) wbw(16+k, 32'hffffffff);
		for (k=0;k<12;k=k+1) wbw(28+k, 32'hffffffff);
		for (k=0;k<12;k=k+1) wbw(40+k, (k==0)?32'hffffffff:32'hfffffffe);
		// Cycles from START to BUSY dropping, counted at the clock
		// rather than by polling -- polling adds its own bus traffic
		// and would overstate the engine's own cost.
		wbw(1, 1);
		n = 0;
		while (dut.st != 0 && n < 5000) begin @(posedge clk); n = n + 1; end
		$display("%0d cycles per multiply at LIMBS=12", n);
		$finish;
	end
endmodule
