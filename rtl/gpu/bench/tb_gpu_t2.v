`timescale 1ns / 1ps

// tb_gpu_t2.v -- T2 whole (rtl/gpu/gpu_t2.v, docs/zgl_t2.md): a frame's
// stream (t2/sN.hex, from sw/common/zgl/tests/test_t2 with T2_VEC=...)
// written word by word into CMD through the slave port, the geometry
// core running the real firmware, the raster and the output, and every
// chunky VRAM word then held to the C model's picture (t2/pN.hex).
//
//   iverilog -g2005 -DVEC=2 -o /tmp/tb_t2 tb_gpu_t2.v ../gpu_t2.v ../zeitlos32g.v
//   cd rtl/gpu/bench && vvp /tmp/tb_t2
//
// -DLINEAR: T2 built LINEAR (`VRAM_SDRAM): the picture in memory a line
// of 320 bytes after another, held to the same model's picture.
// -DSLOW: every write acked after 4-12 cycles rather than the next, as
// main memory behind the bus (SDRAM) does -- what that costs a frame.
// -DWLAT=n: every write acked n cycles after it is offered (DDR3:
// about 19 a word without write combining, 8 with; tb_console_ddr3.v).
// -DPOST=10: the posted-write queue (POST_LOG2), as `VRAM_SDRAM builds it.
// -DSTRIP=6, -DFIFO=9: the strip height and the stream FIFO's size.
// -DTEX: T2 with textures (TEX_LOG2 13, t2/t2fw_tex.hex), its texture
// memory written through TEXADDR and TEXDATA from t2/tN.hex first; the
// textured frames are 10 and 11 (test_t2's "lists, textured").
// -DTWO: the frame twice, back to back, into two pictures: VBASE for
// the second written as soon as the first's stream is in, while T2 is
// still drawing and writing the first (as zgl's pages do), and both
// pictures held to the model -- each frame takes its VBASE in order.

`ifndef VEC
`define VEC 2
`endif

module tb_gpu_t2;

	reg clk = 0, rst = 1;
	always #5 clk = ~clk;

	reg         s_cyc = 0, s_stb = 0, s_we = 0;
	reg  [31:0] s_adr = 0, s_dat = 0;
	wire        s_ack;
	wire [31:0] s_q;

	wire        m_cyc, m_stb, m_we;
	wire [3:0]  m_sel;
	wire [31:0] m_adr, m_dat;
	reg         m_ack = 0;
	reg  [31:0] m_q = 0;

`ifdef LINEAR
	localparam LIN = 1;
`else
	localparam LIN = 0;
`endif
`ifndef POST
`define POST 0
`endif
`ifndef STRIP
`define STRIP 12
`endif
`ifndef FIFO
`define FIFO 11
`endif
`ifdef TEX
	localparam TEXL = 13;
`else
	localparam TEXL = 0;
`endif
	gpu_t2_wb #(.FW_HEX("t2/t2fw.hex"), .LINEAR(LIN), .POST_LOG2(`POST),
		.STRIP_H(`STRIP), .FIFO_LOG2(`FIFO), .TEX_LOG2(TEXL),
		.TEX_FW_HEX("t2/t2fw_tex.hex"), .RECIP_HEX("../t2_recip.hex")) dut (
		.clk(clk), .rst(rst),
		.wb_cyc_i(s_cyc), .wb_stb_i(s_stb), .wb_we_i(s_we), .wb_sel_i(4'hf),
		.wb_adr_i(s_adr), .wb_dat_i(s_dat), .wb_ack_o(s_ack), .wb_dat_o(s_q),
		.m_cyc_o(m_cyc), .m_stb_o(m_stb), .m_we_o(m_we), .m_sel_o(m_sel),
		.m_adr_o(m_adr), .m_dat_o(m_dat), .m_dat_i(m_q), .m_ack_i(m_ack),
		.beam_y(9'h1ff), .vblank(1'b1)
	);

	// VRAM: acks the cycle after stb, as rtl/mem/vram.v
	reg [31:0] vram [0:38399];        // two pictures (-DTWO)
	integer    vwrites = 0, bad_addr = 0;
	wire [31:0] vword = (m_adr - 32'h20000000) >> 2;
	integer    wait_n = 0;
	reg [31:0] rnd = 32'h2468ace1;
	always @(posedge clk) begin
		m_ack <= 1'b0;
		rnd = rnd * 32'd1103515245 + 32'd12345;
`ifdef WLAT
		if (m_cyc && m_stb && !m_ack && wait_n == 0) wait_n = `WLAT;
		if (wait_n > 0) wait_n = wait_n - 1;
		if (m_cyc && m_stb && !m_ack && wait_n == 0) begin
`elsif SLOW
		if (m_cyc && m_stb && !m_ack && wait_n == 0) wait_n = 4 + (rnd[18:16] + rnd[20:19]);
		if (wait_n > 0) wait_n = wait_n - 1;
		if (m_cyc && m_stb && !m_ack && wait_n == 0) begin
`else
		if (m_cyc && m_stb && !m_ack) begin
`endif
			m_ack <= 1'b1;
			m_q <= (vword < 38400) ? vram[vword] : 32'd0;
			if (m_we) begin
				vwrites = vwrites + 1;
				if (vword >= 38400) bad_addr = bad_addr + 1;
				else begin
					if (m_sel[0]) vram[vword][7:0]   <= m_dat[7:0];
					if (m_sel[1]) vram[vword][15:8]  <= m_dat[15:8];
					if (m_sel[2]) vram[vword][23:16] <= m_dat[23:16];
					if (m_sel[3]) vram[vword][31:24] <= m_dat[31:24];
				end
			end
		end
	end

	task wb_write(input [31:0] a, input [31:0] d);
		begin
			@(posedge clk);
			s_cyc <= 1; s_stb <= 1; s_we <= 1; s_adr <= a; s_dat <= d;
			@(posedge clk);
			while (!s_ack) @(posedge clk);
			s_cyc <= 0; s_stb <= 0; s_we <= 0;
		end
	endtask

	task wb_read(input [31:0] a, output [31:0] d);
		begin
			@(posedge clk);
			s_cyc <= 1; s_stb <= 1; s_we <= 0; s_adr <= a;
			@(posedge clk);
			while (!s_ack) @(posedge clk);
			d = s_q;
			s_cyc <= 0; s_stb <= 0;
		end
	endtask

	reg [31:0] stream [0:262143];
	reg [31:0] want [0:19199];
	reg [31:0] texw [0:2047];
	reg [31:0] hdr [0:1];
	reg [8*64-1:0] fn;
	reg [31:0] st, idv;
	integer i, ns, bad, t0, t1, cyc, fr, sg, li, pic, npic;
	always @(posedge clk) cyc = cyc + 1;

	initial begin
		cyc = 0;
		$sformat(fn, "t2/s%0d.hex", `VEC); $readmemh(fn, stream);
		$sformat(fn, "t2/p%0d.hex", `VEC); $readmemh(fn, want);
		$sformat(fn, "t2/r%0d.hex", `VEC); $readmemh(fn, hdr);
		ns = hdr[0];
		for (i = 0; i < 38400; i = i + 1) vram[i] = 32'ha5a5a5a5;
		repeat (5) @(posedge clk);
		rst = 0;
		wb_read(32'd7, idv);
		if (idv != 32'h5A545232) $display("FAIL: ID %08x", idv);
`ifdef TEX
		wb_read(32'd11, idv);
		if (idv != 32'h00000d03) $display("FAIL: CAPS %08x", idv);
		$sformat(fn, "t2/t%0d.hex", `VEC); $readmemh(fn, texw);
		wb_write(32'd12, 32'd0);
		for (i = 0; i < 2048; i = i + 1) wb_write(32'd13, texw[i]);
`endif
		t0 = cyc;
`ifdef TWO
		npic = 2;
		// as zglc_open(): a VBASE, then RESET (which empties the queue
		// and makes it the current one) -- no frame of its own
		wb_write(32'd3, 32'h20000000 + 32'd153600);
		wb_write(32'd5, 32'd1);
		repeat (20) @(posedge clk);
		wb_write(32'd3, 32'h20000000);
		for (i = 0; i < ns; i = i + 1) wb_write(32'd2, stream[i]);
		// the next frame's picture, the first still under way
		wb_read(32'd1, st);
		if (!st[0]) $display("FAIL: -DTWO wants T2 still busy here");
		wb_write(32'd3, 32'h20000000 + 32'd76800);
		for (i = 0; i < ns; i = i + 1) wb_write(32'd2, stream[i]);
`else
		npic = 1;
		for (i = 0; i < ns; i = i + 1) wb_write(32'd2, stream[i]);
`endif
		t1 = cyc;
		// until it is all out
		st = 1;
		while (st[0]) begin
			repeat (200) @(posedge clk);
			wb_read(32'd1, st);
		end
		bad = 0;
		for (pic = 0; pic < npic; pic = pic + 1)
		for (i = 0; i < 19200; i = i + 1) begin
			// the model's word i is chunky VRAM's: row i / 20, its
			// segment the row's quarter; LINEAR, line r's 80 words
			fr = i / 20;
			sg = fr / 240;
			li = pic * 19200 + (LIN ? ((fr - 240 * sg) * 80 + sg * 20 + i % 20) : i);
			if (vram[li] !== want[i]) begin
				if (bad < 8) $display("FAIL: picture %0d, VRAM word %0d (row %0d): %08x, the model's %08x", pic, li, i / 20, vram[li], want[i]);
				bad = bad + 1;
			end
		end
		$display("frame %0d: %0d stream words in %0d cycles; done after %0d cycles; %0d VRAM writes",
			`VEC, ns, t1 - t0, cyc - t0, vwrites);
		if (bad == 0 && bad_addr == 0) $display("tb_gpu_t2: PASS");
		else $display("tb_gpu_t2: FAIL (%0d words, %0d bad addresses)", bad, bad_addr);
		$finish;
	end

	initial begin
		#2000000000;
		$display("FAIL: timeout");
		$finish;
	end
endmodule
