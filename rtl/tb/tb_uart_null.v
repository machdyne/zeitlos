`timescale 1ns/1ps
//
// rtl/uart_null.v -- the phantom UART built when `UART0 is absent.
//
// The assertions here are not abstract: each one is a loop copied from
// the software that would otherwise hang. sw/bios/bios.c's putchar()
// spins until LSR bit 5, and sw/os/uart.c's receive drain spins while
// LSR bit 0 -- so "LSR reads 0x60" is the entire contract, and these
// two tests are the reason the value is 0x60 and not something else.
//
//   iverilog -g2005 -o /tmp/tb rtl/tb/tb_uart_null.v rtl/uart_null.v
//   vvp /tmp/tb
//
module tb;
	reg clk=0, rst=1, we=0, stb=0, cyc=0;
	reg [25:0] adr=0;
	reg [31:0] dat_i=0;
	wire [31:0] dat_o;
	wire ack, int_o;
	integer errors = 0;

	uart_null dut(.wb_clk_i(clk), .wb_rst_i(rst), .wb_adr_i(adr),
		.wb_dat_i(dat_i), .wb_dat_o(dat_o), .wb_we_i(we),
		.wb_sel_i(4'hf), .wb_stb_i(stb), .wb_cyc_i(cyc),
		.wb_ack_o(ack), .int_o(int_o));

	always #5 clk = ~clk;

	task xfer(input [25:0] a, input w, output [31:0] d);
		integer n;
		begin
			@(posedge clk); adr<=a; we<=w; stb<=1; cyc<=1;
			n = 0;
			while (!ack && n < 20) begin @(posedge clk); n = n + 1; end
			if (!ack) begin $display("FAIL: no ack for word %0d", a); errors=errors+1; end
			d = dat_o;
			@(posedge clk); stb<=0; cyc<=0; we<=0;
			@(posedge clk);
		end
	endtask

	reg [31:0] d;
	integer spins;
	initial begin
		repeat(4) @(posedge clk); rst<=0; @(posedge clk);

		xfer(26'd5, 0, d);
		if (d[7:0] !== 8'h60) begin $display("FAIL: LSR=%02x want 60", d[7:0]); errors=errors+1; end
		else $display("ok  LSR reads 0x%02x (THRE|TEMT set, DR clear)", d[7:0]);

		xfer(26'd2, 0, d);
		if (d[7:0] !== 8'h01) begin $display("FAIL: IIR=%02x want 01", d[7:0]); errors=errors+1; end
		else $display("ok  IIR reads 0x%02x (no interrupt pending)", d[7:0]);

		xfer(26'd0, 0, d);
		if (d !== 32'h0) begin $display("FAIL: RBR=%08x", d); errors=errors+1; end
		else $display("ok  RBR reads zero");

		// a write must ack -- this is what putchar does after the spin
		xfer(26'd0, 1, d);
		$display("ok  write to THR acked and discarded");

		if (int_o !== 1'b0) begin $display("FAIL: int_o asserted"); errors=errors+1; end
		else $display("ok  int_o tied low");

		// the actual bios putchar loop: while ((lsr & 0x20)==0);
		spins = 0;
		xfer(26'd5, 0, d);
		while (((d & 32'h20) == 0) && spins < 100) begin
			spins = spins + 1; xfer(26'd5, 0, d);
		end
		if (spins != 0) begin $display("FAIL: putchar spun %0d times", spins); errors=errors+1; end
		else $display("ok  bios putchar() spin loop exits on the first read");

		// os/uart.c rx drain: while (lsr & 0x01)
		xfer(26'd5, 0, d);
		if ((d & 32'h1) != 0) begin $display("FAIL: DR set, rx drain would loop"); errors=errors+1; end
		else $display("ok  rx drain loop exits immediately");

		if (errors == 0) $display("\nPASS -- software written for a real UART runs unchanged");
		else $display("\nFAIL -- %0d problem(s)", errors);
		$finish;
	end
endmodule
