// picorv32 with ENABLE_ROT: rot.S checks ror, rol and rori against the
// base ISA (writes -1 at 0x400 when all agree). With ENABLE_ROT=0 the
// first rotate must trap as an illegal instruction instead.
`timescale 1ns/1ps
module tb_rot;
	reg clk = 0, resetn = 0;
	always #5 clk = !clk;

	reg [31:0] mem [0:1023];
	initial $readmemh("rot.hex", mem);

	wire mem_valid, mem_instr;
	reg  mem_ready;
	wire [31:0] mem_addr, mem_wdata;
	wire [3:0] mem_wstrb;
	reg  [31:0] mem_rdata;
	wire trap;

	picorv32 #(
		.BARREL_SHIFTER(1),
		.ENABLE_ROT(`ROT),
		.ENABLE_MUL(0), .ENABLE_DIV(0),
		.CATCH_ILLINSN(1)
	) cpu (
		.clk(clk), .resetn(resetn), .trap(trap),
		.mem_valid(mem_valid), .mem_instr(mem_instr), .mem_ready(mem_ready),
		.mem_addr(mem_addr), .mem_wdata(mem_wdata), .mem_wstrb(mem_wstrb), .mem_rdata(mem_rdata),
		.mem_la_read(), .mem_la_write(), .mem_la_addr(), .mem_la_wdata(), .mem_la_wstrb(),
		.pcpi_valid(), .pcpi_insn(), .pcpi_rs1(), .pcpi_rs2(),
		.pcpi_wr(1'b0), .pcpi_rd(32'b0), .pcpi_wait(1'b0), .pcpi_ready(1'b0),
		.irq(32'b0), .eoi(),
		.trace_valid(), .trace_data()
	);

	always @(posedge clk) begin
		mem_ready <= 0;
		if (mem_valid && !mem_ready) begin
			mem_ready <= 1;
			mem_rdata <= mem[mem_addr[11:2]];
			if (mem_wstrb[0]) mem[mem_addr[11:2]][ 7: 0] <= mem_wdata[ 7: 0];
			if (mem_wstrb[1]) mem[mem_addr[11:2]][15: 8] <= mem_wdata[15: 8];
			if (mem_wstrb[2]) mem[mem_addr[11:2]][23:16] <= mem_wdata[23:16];
			if (mem_wstrb[3]) mem[mem_addr[11:2]][31:24] <= mem_wdata[31:24];
		end
	end

	integer cycles = 0;
	initial begin
		mem[256] = 0;
		repeat (10) @(posedge clk);
		resetn = 1;
		while (!trap && cycles < 4000000) begin @(posedge clk); cycles = cycles + 1; end
		repeat (5) @(posedge clk);
		if (`ROT) begin
			if (mem[256] === 32'hffffffff) $display("tb_rot: ENABLE_ROT=1: ror, rol, rori agree with the base ISA (%0d cycles)", cycles);
			else begin $display("tb_rot: FAIL: case %0d differs (0x400 = %h)", mem[256], mem[256]); $finish_and_return(1); end
		end else begin
			if (trap && mem[256] === 32'h0) $display("tb_rot: ENABLE_ROT=0: the first rotate traps, as before");
			else begin $display("tb_rot: FAIL: without ENABLE_ROT a rotate ran (0x400 = %h)", mem[256]); $finish_and_return(1); end
		end
		$finish;
	end
endmodule
