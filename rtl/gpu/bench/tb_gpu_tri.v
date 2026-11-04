`timescale 1ns / 1ps

// Self-checking testbench for the triangle engine (rtl/gpu/gpu_tri.v,
// docs/gpu_tri.md).
//
// The vectors are zgl's own: sw/common/zgl/tests/t_vectors.c draws four
// frames on the build host as the engine's commands, and the chunky
// VRAM each must leave from the engine's C model, which test_frames
// holds to the software strips pixel for pixel. Here every command
// word goes in through the slave port (CMD, waiting while the FIFO is
// full), the engine draws and writes out, and when STATUS says idle
// every VRAM word is compared.
//
//   1. gldemo's scene: a lit smooth sphere, a flat cube, a ring of lines,
//      depth-tested
//   2. 120 random triangles and lines, depth, smooth, clear index 3
//   3. orthographic, scissored (37,21 211x150: strips off the 16-row
//      grid, byte selects at both sides), flat, no depth, clear 7
//   4. 1. again: buffers cleared to 7 and 3 before go back to 0
//
// Then WAIT_BEAM: frame 4 again with the beam held inside strip 5's
// rows for a while, which must hold that strip back (and only that);
// and VSYNC: frame 4 again outside vertical blanking, which must hold
// the whole frame until blanking comes.
//
// Running it (from the repo root):
//
//   make -C sw/common/zgl vectors
//   iverilog -g2005 -o /tmp/tb_tri rtl/gpu/bench/tb_gpu_tri.v rtl/gpu/gpu_tri.v
//   cd rtl/gpu/bench && vvp /tmp/tb_tri
//
// About 20 M cycles; iverilog takes several minutes. +frames=N stops
// after N frames, +trace prints each command as it is decoded, and
// +progress a heartbeat.

module tb_gpu_tri;

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

	reg  [8:0]  beam_y = 9'h1ff;
	reg         vblank = 1'b1;

	gpu_tri_wb dut (
		.clk(clk), .rst(rst),
		.wb_cyc_i(s_cyc), .wb_stb_i(s_stb), .wb_we_i(s_we), .wb_sel_i(4'hf),
		.wb_adr_i(s_adr), .wb_dat_i(s_dat), .wb_ack_o(s_ack), .wb_dat_o(s_q),
		.m_cyc_o(m_cyc), .m_stb_o(m_stb), .m_we_o(m_we), .m_sel_o(m_sel),
		.m_adr_o(m_adr), .m_dat_o(m_dat), .m_dat_i(m_q), .m_ack_i(m_ack),
		.beam_y(beam_y), .vblank(vblank)
	);

	// VRAM: acks the cycle after stb, as rtl/mem/vram.v
	reg [31:0] vram [0:19199];
	integer    vwrites = 0, bad_addr = 0;
	wire [31:0] vword = (m_adr - 32'h20000000) >> 2;
	always @(posedge clk) begin
		m_ack <= 1'b0;
		if (m_cyc && m_stb && !m_ack) begin
			m_ack <= 1'b1;
			m_q <= (vword < 19200) ? vram[vword] : 32'd0;
			if (m_we) begin
				if (vword >= 19200) bad_addr = bad_addr + 1;
				else begin
					if (m_sel[0]) vram[vword][7:0]   <= m_dat[7:0];
					if (m_sel[1]) vram[vword][15:8]  <= m_dat[15:8];
					if (m_sel[2]) vram[vword][23:16] <= m_dat[23:16];
					if (m_sel[3]) vram[vword][31:24] <= m_dat[31:24];
				end
				vwrites = vwrites + 1;
			end
		end
	end

	reg [31:0] cmd    [0:399999];
	reg [31:0] frames [0:6*8-1];
	reg [31:0] expect [0:6*19200-1];
	reg [7:0]  lumt   [0:255];
	integer    fbase  [0:5];        // each frame's first command word

	task wb_write(input [3:0] a, input [31:0] d);
		begin
			@(posedge clk);
			s_cyc <= 1; s_stb <= 1; s_we <= 1; s_adr <= {28'd0, a}; s_dat <= d;
			@(posedge clk);
			while (!s_ack) @(posedge clk);
			s_cyc <= 0; s_stb <= 0; s_we <= 0;
		end
	endtask

	reg [31:0] rd;
	task wb_read(input [3:0] a);
		begin
			@(posedge clk);
			s_cyc <= 1; s_stb <= 1; s_we <= 0; s_adr <= {28'd0, a};
			@(posedge clk);
			while (!s_ack) @(posedge clk);
			rd = s_q;
			s_cyc <= 0; s_stb <= 0;
		end
	endtask

	task wait_idle;
		begin
			rd = 1;
			while (rd[0]) wb_read(4'd1);
		end
	endtask

	integer nfr;
	integer f, i, w, base, errs, total_errs, t0, t1, strips0;

	task check(input integer fr, input [8*48-1:0] name);
		begin
			errs = 0;
			for (i = 0; i < 19200; i = i + 1)
				if (vram[i] !== expect[fr * 19200 + i]) begin
					if (errs < 8)
						$display("  word %0d (row %0d, i %0d): %08x, want %08x", i, i / 20, i % 20,
							vram[i], expect[fr * 19200 + i]);
					errs = errs + 1;
				end
			if (errs == 0) $display("PASS %0s", name);
			else $display("FAIL %0s: %0d words differ", name, errs);
			total_errs = total_errs + errs;
		end
	endtask

	initial begin
		$readmemh("tri/cmd.hex", cmd);
		$readmemh("tri/frames.hex", frames);
		$readmemh("tri/expect.hex", expect);
		$readmemh("tri/lum.hex", lumt);
		for (i = 0; i < 19200; i = i + 1) vram[i] = 32'ha5a5a5a5;
		total_errs = 0;
		repeat (4) @(posedge clk);
		rst <= 0;
		repeat (4) @(posedge clk);

		for (i = 0; i < 256; i = i + 1) wb_write(4'd6, {19'd0, lumt[i][4:0], i[7:0]});
		base = 0;
		for (f = 0; f < 6; f = f + 1) begin fbase[f] = base; base = base + frames[f * 8]; end
		nfr = 6;
		if ($value$plusargs("frames=%d", nfr)) ;
		for (f = 0; f < nfr; f = f + 1) begin
			// chunky or MONO, and MONO's window and rectangle
			wb_write(4'd0, {29'd0, frames[f * 8 + 1][0], 2'b00});
			wb_write(4'd8, {frames[f * 8 + 3][15:0], frames[f * 8 + 2][15:0]});
			wb_write(4'd9, {frames[f * 8 + 5][15:0], frames[f * 8 + 4][15:0]});
			wb_write(4'd10, {frames[f * 8 + 7][15:0], frames[f * 8 + 6][15:0]});
			t0 = $time;
			for (w = 0; w < frames[f * 8]; w = w + 1) wb_write(4'd2, cmd[fbase[f] + w]);
			wait_idle;
			t1 = $time;
			$display("frame %0d: %0d words, %0d cycles", f, frames[f * 8], (t1 - t0) / 10);
			case (f)
			0: check(0, "frame 1: gldemo's scene");
			1: check(1, "frame 2: random, depth, smooth");
			2: check(2, "frame 3: ortho, scissored, flat");
			3: check(3, "frame 4: gldemo's again");
			4: check(4, "frame 5: MONO, gldemo's, window at 101,57");
			default: check(5, "frame 6: MONO, random, window at 37,200, clipped");
			endcase
		end
		wb_write(4'd0, 32'd0);

		if (nfr < 6) $finish;
		// WAIT_BEAM: the beam parked in strip 5's rows holds that strip
		wb_write(4'd0, 32'd1);
		beam_y <= 9'd85;
		base = fbase[3];
		for (i = 0; i < 19200; i = i + 1) vram[i] = 32'ha5a5a5a5;
		wb_read(4'd4); strips0 = rd;
		// the commands go in while the beam sits there: the FIFO fills
		// and CMD waits, so they go in alongside
		fork
			for (w = 0; w < frames[3 * 8]; w = w + 1) wb_write(4'd2, cmd[base + w]);
			begin
				repeat (60000) @(posedge clk);
				if (dut.strips - strips0 == 5 && vram[85 * 20] === 32'ha5a5a5a5 &&
				    vram[79 * 20] !== 32'ha5a5a5a5)
					$display("PASS wait_beam: 5 strips out, strip 5 held");
				else begin
					$display("FAIL wait_beam: %0d strips out", dut.strips - strips0);
					total_errs = total_errs + 1;
				end
				beam_y <= 9'h1ff;
			end
		join
		wait_idle;
		check(3, "wait_beam: the frame, once the beam moved on");

		// VSYNC: out of vertical blanking, a frame's first strip waits
		wb_write(4'd0, 32'd2);
		vblank <= 1'b0;
		for (i = 0; i < 19200; i = i + 1) vram[i] = 32'ha5a5a5a5;
		strips0 = dut.strips;
		fork
			for (w = 0; w < frames[3 * 8]; w = w + 1) wb_write(4'd2, cmd[base + w]);
			begin
				repeat (40000) @(posedge clk);
				if (dut.strips == strips0 && vram[0] === 32'ha5a5a5a5)
					$display("PASS vsync: nothing out before vertical blanking");
				else begin
					$display("FAIL vsync: %0d strips out", dut.strips - strips0);
					total_errs = total_errs + 1;
				end
				vblank <= 1'b1;
			end
		join
		wait_idle;
		check(3, "vsync: the frame, from vertical blanking on");

		wb_read(4'd1);
		if (rd[1]) begin $display("FAIL: a bad command was seen"); total_errs = total_errs + 1; end
		if (bad_addr != 0) begin $display("FAIL: %0d writes outside VRAM", bad_addr); total_errs = total_errs + 1; end
		$display("%0d VRAM writes", vwrites);
		if (total_errs == 0) $display("tb_gpu_tri: ALL PASS");
		else $display("tb_gpu_tri: %0d FAILURES", total_errs);
		$finish;
	end

	// +trace: every command as the engine decodes it
	always @(posedge clk)
		if ($test$plusargs("trace") && dut.st == 4'd1 && dut.rpend)
			$display("  [%0d] cmd %08x (fifo %0d, d_sel %0d, ready %b)", cyc, dut.f_q, dut.f_n, dut.d_sel, dut.ready);

	// a watchdog, and a heartbeat for a long run (+progress)
	integer cyc = 0;
	always @(posedge clk) begin
		cyc = cyc + 1;
		if ($test$plusargs("progress") && cyc % 200000 == 0)
			$display("  [%0d] st %0d ost %0d fifo %0d strips %0d", cyc, dut.st, dut.ost, dut.f_n, dut.strips);
		if (cyc > 30000000) begin
			$display("tb_gpu_tri: TIMEOUT (st %0d ost %0d fifo %0d ready %b)", dut.st, dut.ost, dut.f_n, dut.ready);
			$finish;
		end
	end
endmodule
