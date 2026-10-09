/*
 * Zeitlos -- rtl/gpio_stream.v's 8b/10b encoder and decoder against
 * every symbol tools/gen_8b10b.py knows (8b10b_vectors.txt): all 256
 * data bytes and all twelve control symbols, from both running
 * disparities. 536 encodes and 536 decodes, bit for bit.
 *
 *   make -C rtl/tests 8b10b
 *
 * The script checks the code's properties (DC balance, run length,
 * comma position) before it writes a vector, so a pass here means the
 * RTL's tables are the standard code, not merely self-consistent.
 */

`timescale 1ns / 1ps

module tb_8b10b;

	reg clk = 0, rst = 1;
	wire done;
	wire [31:0] rdo;
	wire [63:0] own, own_dir, own_out;

	gpio_stream #(.NPINS(8)) dut (
		.clk(clk), .rst(rst),
		.acc(1'b0), .we(1'b0), .ra(4'd0), .wd(32'd0),
		.done(done), .rdo(rdo),
		.pin_in(64'd0), .own(own), .own_dir(own_dir), .own_out(own_out)
	);

	integer fd, n, i, r;
	integer rd_in, k, b, sym, rd_out;
	integer errors = 0;
	reg [10:0] e;
	reg [9:0] d;

	initial begin
		fd = $fopen("8b10b_vectors.txt", "r");
		if (fd == 0) begin
			$display("tb_8b10b: cannot open 8b10b_vectors.txt");
			$finish;
		end
		r = $fscanf(fd, "%d", n);
		for (i = 0; i < n; i = i + 1) begin
			r = $fscanf(fd, "%d %d %h %h %d", rd_in, k, b, sym, rd_out);
			e = dut.enc8b10b(k[0], b[7:0], rd_in[0]);
			if (e !== { rd_out[0], sym[9:0] }) begin
				$display("FAIL enc rd=%0d k=%0d b=%02x: got %03x rd %0d, want %03x rd %0d",
				         rd_in, k, b, e[9:0], e[10], sym, rd_out);
				errors = errors + 1;
			end
			d = dut.dec8b10b(sym[9:0]);
			if (d !== { 1'b1, k[0], b[7:0] }) begin
				$display("FAIL dec %03x: got v=%0d k=%0d b=%02x, want k=%0d b=%02x",
				         sym, d[9], d[8], d[7:0], k, b);
				errors = errors + 1;
			end
		end
		// two things that are not symbols must not decode
		d = dut.dec8b10b(10'h000);
		if (d[9]) begin $display("FAIL 0000000000 decoded"); errors = errors + 1; end
		d = dut.dec8b10b(10'h3FF);
		if (d[9]) begin $display("FAIL 1111111111 decoded"); errors = errors + 1; end
		$fclose(fd);
		if (errors == 0) $display("tb_8b10b: %0d symbols, PASS", n);
		else $display("tb_8b10b: FAIL (%0d errors)", errors);
		$finish;
	end

endmodule
