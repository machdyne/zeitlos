/*
 * tb_zeitlos32g.v -- zeitlos32g running the geometry unit's real
 * firmware (t2/t2fw.hex) on frames' streams (t2/sN.hex), its records
 * held to the C model's (t2/rN.hex), word for word. The vectors come
 * from sw/common/zgl/tests/test_t2 (T2_VEC=...). One frame a run:
 *
 *   iverilog -g2005 -DVEC=2 -o /tmp/tb_z32g tb_zeitlos32g.v ../zeitlos32g.v
 *   cd rtl/gpu/bench && vvp /tmp/tb_z32g
 */
`timescale 1ns/1ps
`ifndef VEC
`define VEC 2
`endif

module tb_zeitlos32g;
	reg clk = 0, rst = 1;
	always #10 clk = ~clk;

	wire        io_rd, io_wr;
	wire [17:0] io_addr;
	wire [31:0] io_wdata;
	reg  [31:0] io_rdata;
	reg         io_ready;

	zeitlos32g #(.IMEM_HEX("t2/t2fw.hex")) cpu (
		.clk(clk), .rst(rst),
		.io_rd(io_rd), .io_wr(io_wr), .io_addr(io_addr), .io_wdata(io_wdata),
		.io_rdata(io_rdata), .io_ready(io_ready)
	);

	reg [31:0] stream [0:262143];
	reg [31:0] want [0:65535];
	reg [31:0] list [0:8191];
	integer ns, nwant, si, bad, i, cycles, nlist, done;
	reg [8*64-1:0] sfile, rfile;

	initial begin
		$sformat(sfile, "t2/s%0d.hex", `VEC);
		$sformat(rfile, "t2/r%0d.hex", `VEC);
		$readmemh(sfile, stream);
		$readmemh(rfile, want);
		ns = want[0];
		nwant = want[1];
		si = 0; bad = 0; cycles = 0; done = 0;
		io_ready = 0;
		repeat (4) @(posedge clk);
		rst = 0;
	end

	// the engine's side of the I/O cycles: the stream, the registers, the list
	always @(posedge clk) begin
		io_ready <= 1'b0;
		if (!rst) cycles = cycles + 1;
		if (io_rd) begin
			if (io_addr == 18'h20000) begin
				if (si < ns) begin
					io_rdata <= stream[si];
					si = si + 1;
					io_ready <= 1'b1;
				end else begin
					$display("FAIL: the core asked for more of the stream (%0d words) without ending the frame", ns);
					$finish;
				end
			end else begin
				io_rdata <= 32'd0;               // STATUS: the raster is idle
				io_ready <= 1'b1;
			end
		end
		if (io_wr) begin
			io_ready <= 1'b1;
			if (io_addr[17:16] == 2'b11) list[io_addr[15:2]] = io_wdata;
			if (io_addr == 18'h20014) begin  // GO
				nlist = io_wdata;
				if (nlist != nwant) begin
					$display("FAIL: %0d record words, the model's %0d", nlist, nwant);
					bad = bad + 1;
				end
				for (i = 0; i < nwant && i < nlist; i = i + 1)
					if (list[i] !== want[2 + i]) begin
						if (bad < 8) $display("FAIL: record word %0d: %08x, the model's %08x", i, list[i], want[2 + i]);
						bad = bad + 1;
					end
				$display("frame %0d: %0d stream words, %0d record words, %0d cycles", `VEC, ns, nlist, cycles);
				show_kinds;
				if (bad == 0) $display("tb_zeitlos32g: PASS");
				else $display("tb_zeitlos32g: FAIL (%0d)", bad);
				$finish;
			end
		end
	end

	// where the cycles go: E's kind of instruction each cycle, or a bubble
	integer kc [0:11];
	integer ki;
	initial for (ki = 0; ki < 12; ki = ki + 1) kc[ki] = 0;
	always @(posedge clk) if (!rst) begin
		if (!cpu.e_valid) kc[11] = kc[11] + 1;
		else kc[cpu.e_kind] = kc[cpu.e_kind] + 1;
	end
	task show_kinds;
		begin
			$display("cycles: alu %0d lui %0d auipc %0d jal %0d jalr %0d br %0d load %0d store %0d mul %0d div %0d nop %0d bubble %0d",
				kc[0], kc[1], kc[2], kc[3], kc[4], kc[5], kc[6], kc[7], kc[8], kc[9], kc[10], kc[11]);
		end
	endtask

	// every register write, for a lockstep check against the C simulator's
	integer tf;
	initial tf = $fopen("/tmp/claude-0/rtl.tr", "w");
	always @(posedge clk)
		if (cpu.e_valid && cpu.e_done && cpu.wb_en && cpu.wb_rd != 0)
			$fdisplay(tf, "%08x %2d %08x", cpu.e_pc, cpu.wb_rd, cpu.wb_val);

	initial begin
		#400000000;
		$display("FAIL: timeout, %0d of %0d stream words taken, pc in E %08x", si, ns, cpu.e_pc);
		$finish;
	end
endmodule
