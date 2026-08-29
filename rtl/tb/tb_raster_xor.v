// Testbench for the two-bit raster op in rtl/gpu/gpu_raster.v.
//
//   iverilog -g2005 -o tb_raster_xor rtl/test/tb_raster_xor.v \
//            rtl/gpu/gpu_raster.v && ./tb_raster_xor
//
// Models VRAM as a small behavioural memory acking in one cycle -- the
// same shape rtl/mem/vram.v presents -- and checks the property the
// whole feature rests on: XOR is its own inverse, so drawing a line
// twice restores every word it touched.

`timescale 1ns/1ps

module tb_raster_xor;

	reg clk = 0, rst = 1;
	always #5 clk = ~clk;

	reg  [31:0] wb_adr, wb_dat_i;
	reg         wb_cyc = 0, wb_stb = 0, wb_we = 0;
	wire [31:0] wb_dat_o;
	wire        wb_ack;

	wire [31:0] m_adr, m_dat_o;
	wire        m_cyc, m_stb, m_we;
	wire [3:0]  m_sel;
	reg  [31:0] m_dat_i;
	reg         m_ack = 0;

	reg [31:0] vram [0:9599];

	integer i;
	integer errors = 0;
	reg [31:0] snapshot [0:9599];


	gpu_raster_wb dut (
		.clk(clk), .rst(rst),
		.wb_adr_i(wb_adr), .wb_dat_i(wb_dat_i), .wb_dat_o(wb_dat_o),
		.wb_sel_i(4'b1111),
		.wb_we_i(wb_we), .wb_cyc_i(wb_cyc), .wb_stb_i(wb_stb), .wb_ack_o(wb_ack),
		.m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i),
		.m_we_o(m_we), .m_sel_o(m_sel), .m_cyc_o(m_cyc), .m_stb_o(m_stb),
		.m_ack_i(m_ack)
	);

	// one-cycle-ack VRAM. The framebuffer base is 0x20000000, whose
	// low 16 bits are zero, so the word index is simply m_adr[15:2]
	// for any offset inside the 38400-byte framebuffer.
	wire [13:0] vword = m_adr[15:2];

	always @(posedge clk) begin
		m_ack <= 1'b0;
		if (m_cyc && m_stb && !m_ack) begin
			m_dat_i <= vram[vword];
			if (m_we) vram[vword] <= m_dat_o;
			m_ack <= 1'b1;
		end
	end

	task wr(input [4:0] a, input [31:0] d);
	begin
		@(posedge clk);
		wb_adr = {27'd0, a}; wb_dat_i = d; wb_we = 1; wb_cyc = 1; wb_stb = 1;
		@(posedge clk);
		while (!wb_ack) @(posedge clk);
		wb_cyc = 0; wb_stb = 0; wb_we = 0;
	end
	endtask

	task line(input [9:0] x0, input [9:0] y0,
	          input [9:0] x1, input [9:0] y1, input [1:0] op);
	begin
		wr(5'd0, {22'd0, x0});
		wr(5'd1, {22'd0, y0});
		wr(5'd2, {22'd0, x1});
		wr(5'd3, {22'd0, y1});
		wr(5'd4, {30'd0, op});
		wr(5'd5, 32'd1);
		repeat (4000) @(posedge clk);
	end
	endtask

	initial begin
		for (i = 0; i < 9600; i = i + 1)
			vram[i] = 32'hA5A5_5A5A ^ i;

		repeat (4) @(posedge clk);
		rst = 0;
		repeat (4) @(posedge clk);

		wr(5'd15, 32'd0);   // clip disabled

		for (i = 0; i < 9600; i = i + 1) snapshot[i] = vram[i];

		$display("-- XOR a diagonal line, then XOR it again --");
		line(10'd7, 10'd3, 10'd200, 10'd97, 2'd2);

		errors = 0;
		for (i = 0; i < 9600; i = i + 1)
			if (vram[i] !== snapshot[i]) errors = errors + 1;
		if (errors == 0) begin
			$display("FAIL: first XOR changed nothing -- op not decoded?");
			$finish;
		end
		$display("   first pass altered %0d words", errors);

		line(10'd7, 10'd3, 10'd200, 10'd97, 2'd2);

		errors = 0;
		for (i = 0; i < 9600; i = i + 1)
			if (vram[i] !== snapshot[i]) errors = errors + 1;
		if (errors == 0)
			$display("   second pass restored VRAM exactly  OK");
		else
			$display("FAIL: %0d words differ after the second XOR", errors);

		$display("-- op 0 and op 1 still behave as clear and set --");
		line(10'd20, 10'd20, 10'd60, 10'd20, 2'd1);
		if (vram[20*20 + 0][20] !== 1'b1)
			$display("FAIL: set did not set");
		line(10'd20, 10'd20, 10'd60, 10'd20, 2'd0);
		if (vram[20*20 + 0][20] !== 1'b0)
			$display("FAIL: clear did not clear");
		$display("   set/clear unchanged  OK");

		$display("done (%0d errors)", errors);
		$finish;
	end

endmodule
