/*
 * Zeitlos SOC GPU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zeitlos32g.v -- the small RV32IM core inside T2, the triangle engine
 * that runs the whole frame (docs/zgl_t2.md). It runs the geometry
 * unit's program (sw/t2fw, t2fw.hex) from its own memories, and talks
 * to the engine through I/O cycles. Nothing else: no CSRs, interrupts,
 * traps, caches or bus; an illegal instruction is executed as a no-op.
 *
 * Memory map (byte addresses):
 *   0x00000-  code (IMEM_WORDS words, from IMEM_HEX); loads read it too
 *   0x10000-  data (DMEM_WORDS words)
 *   0x20000-  I/O: io_rd / io_wr cycles to the engine (gpu_t2.v)
 *
 * Three stages: F presents the fetch address to the code memory; D
 * decodes the word it returns and reads the registers (forwarded from E
 * when E writes one D reads); E executes. A taken branch or a jump costs
 * one bubble; a load 3 cycles, a multiply 3, a divide or remainder 19;
 * an I/O cycle until the engine answers (io_ready). Every multiplier
 * input and partial product is registered, and loaded data is
 * registered before it is used (timing: the engine is to stay off the
 * SOC's critical path).
 *
 * Plain Verilog-2005, no local declarations, one driver per signal.
 */

module zeitlos32g #(
	parameter IMEM_WORDS = 3072,
	parameter DMEM_WORDS = 1024,
	parameter IMEM_HEX = "sw/t2fw/t2fw.hex"
) (
	input clk,
	input rst,

	output reg        io_rd,
	output reg        io_wr,
	output reg [17:0] io_addr,
	output reg [31:0] io_wdata,
	input      [31:0] io_rdata,
	input             io_ready
);

	// ------------------------------------------------------------------
	// memories
	// ------------------------------------------------------------------

	reg [31:0] imem [0:IMEM_WORDS-1];
	reg [31:0] dmem [0:DMEM_WORDS-1];
	// the data memory's word address bits: 1,024 words (4 KB) or 2,048
	// (8 KB, T2 with textures: sw/t2fw/link_tex.ld)
	localparam DM_HI = (DMEM_WORDS > 1024) ? 12 : 11;
	initial $readmemh(IMEM_HEX, imem);

	reg  [31:0] f_do;             // the code memory's word for D
	reg  [31:0] d_rdata;          // the code memory's word for a load
	reg  [31:0] m_rdata;          // the data memory's word for a load

	wire [31:0] fa;               // this cycle's fetch address
	wire        f_en;             // fetch (else D holds its word)
	reg  [31:0] e_maddr;          // E's load/store address
	reg         e_mrd_i, e_mrd_d; // E reads code / data memory this cycle
	reg  [3:0]  e_mwe;            // E writes data memory (byte lanes)
	reg  [31:0] e_mwdata;

	always @(posedge clk) begin
		if (f_en) f_do <= imem[fa[31:2]];
	end

	always @(posedge clk) begin
		if (e_mrd_i) d_rdata <= imem[e_maddr[31:2]];
	end

	always @(posedge clk) begin
		if (e_mrd_d) m_rdata <= dmem[e_maddr[DM_HI:2]];
		if (e_mwe[0]) dmem[e_maddr[DM_HI:2]][7:0]   <= e_mwdata[7:0];
		if (e_mwe[1]) dmem[e_maddr[DM_HI:2]][15:8]  <= e_mwdata[15:8];
		if (e_mwe[2]) dmem[e_maddr[DM_HI:2]][23:16] <= e_mwdata[23:16];
		if (e_mwe[3]) dmem[e_maddr[DM_HI:2]][31:24] <= e_mwdata[31:24];
	end

	// ------------------------------------------------------------------
	// registers
	// ------------------------------------------------------------------

	reg  [31:0] rf [0:31];
	reg         wb_en;            // E writes rd this cycle
	reg  [4:0]  wb_rd;
	reg  [31:0] wb_val;

	integer k;
	initial begin
		for (k = 0; k < 32; k = k + 1) rf[k] = 32'd0;
	end

	always @(posedge clk) begin
		if (wb_en && wb_rd != 5'd0) rf[wb_rd] <= wb_val;
	end

	// ------------------------------------------------------------------
	// D: decode the fetched word, read the registers
	// ------------------------------------------------------------------

	reg  [31:0] d_pc;             // the address of f_do
	reg         d_valid;
	wire [31:0] ir = f_do;
	wire [6:0]  d_op  = ir[6:0];
	wire [4:0]  d_rd  = ir[11:7];
	wire [2:0]  d_f3  = ir[14:12];
	wire [4:0]  d_rs1 = ir[19:15];
	wire [4:0]  d_rs2 = ir[24:20];
	wire [6:0]  d_f7  = ir[31:25];

	wire [31:0] imm_i = {{20{ir[31]}}, ir[31:20]};
	wire [31:0] imm_s = {{20{ir[31]}}, ir[31:25], ir[11:7]};
	wire [31:0] imm_b = {{19{ir[31]}}, ir[31], ir[7], ir[30:25], ir[11:8], 1'b0};
	wire [31:0] imm_u = {ir[31:12], 12'd0};
	wire [31:0] imm_j = {{11{ir[31]}}, ir[31], ir[19:12], ir[20], ir[30:21], 1'b0};

	wire [31:0] rs1_rf = rf[d_rs1];
	wire [31:0] rs2_rf = rf[d_rs2];
	wire [31:0] d_a = (wb_en && wb_rd == d_rs1 && d_rs1 != 5'd0) ? wb_val : rs1_rf;
	wire [31:0] d_b = (wb_en && wb_rd == d_rs2 && d_rs2 != 5'd0) ? wb_val : rs2_rf;

	// ------------------------------------------------------------------
	// E: execute
	// ------------------------------------------------------------------

	localparam [3:0] K_ALU = 4'd0, K_LUI = 4'd1, K_AUIPC = 4'd2, K_JAL = 4'd3,
	                 K_JALR = 4'd4, K_BR = 4'd5, K_LOAD = 4'd6, K_STORE = 4'd7,
	                 K_MUL = 4'd8, K_DIV = 4'd9, K_NOP = 4'd10;

	reg         e_valid;
	reg  [3:0]  e_kind;
	reg  [31:0] e_pc;
	reg  [31:0] e_a, e_b, e_imm;
	reg  [4:0]  e_rd;
	reg  [2:0]  e_f3;
	reg         e_alt;            // SUB / SRA
	reg         e_immop;          // the ALU's second operand is the immediate
	reg  [2:0]  e_st;             // a multi-cycle instruction's step
	reg         e_done;           // ...finishing this cycle (combinational)

	// the ALU
	wire [31:0] alu_b = e_immop ? e_imm : e_b;
	wire [32:0] alu_sub = {1'b0, e_a} - {1'b0, alu_b};
	wire        alu_lt  = (e_a[31] != alu_b[31]) ? e_a[31] : alu_sub[32];
	wire        alu_ltu = alu_sub[32];
	wire [4:0]  shamt = alu_b[4:0];
	wire signed [31:0] alu_sra = $signed(e_a) >>> shamt;   // its own wire: in a
	                                     // ?: with an unsigned arm >>> would not extend
	reg  [31:0] alu_y;
	always @(*) begin
		case (e_f3)
		3'd0: alu_y = (e_alt && !e_immop) ? alu_sub[31:0] : e_a + alu_b;
		3'd1: alu_y = e_a << shamt;
		3'd2: alu_y = {31'd0, alu_lt};
		3'd3: alu_y = {31'd0, alu_ltu};
		3'd4: alu_y = e_a ^ alu_b;
		3'd5: alu_y = e_alt ? alu_sra : (e_a >> shamt);
		3'd6: alu_y = e_a | alu_b;
		default: alu_y = e_a & alu_b;
		endcase
	end

	// branches: the ALU's subtract (a branch's second operand is rs2)
	wire        br_eq  = (alu_sub[31:0] == 32'd0);
	wire        br_lt  = alu_lt;
	wire        br_ltu = alu_ltu;
	reg         br_take;
	always @(*) begin
		case (e_f3)
		3'd0: br_take = br_eq;
		3'd1: br_take = !br_eq;
		3'd4: br_take = br_lt;
		3'd5: br_take = !br_lt;
		3'd6: br_take = br_ltu;
		default: br_take = !br_ltu;
		endcase
	end

	wire [31:0] e_addr = e_a + e_imm;         // loads, stores, JALR
	wire [31:0] e_pc4  = e_pc + 32'd4;

	// the multiplier: 33x33 signed from 17-bit pieces, every product registered
	// the operands, sign- or zero-extended, straight from E's registers
	wire [32:0] m_a = {(e_f3 == 3'd3) ? 1'b0 : e_a[31], e_a};    // mulhu: a unsigned
	wire [32:0] m_b = {(e_f3[1]) ? 1'b0 : e_b[31], e_b};         // mulhsu, mulhu: b unsigned
	reg  [33:0] p_ll;              // a_lo * b_lo (unsigned 17x17)
	reg  signed [34:0] p_lh, p_hl; // a_lo * b_hi, a_hi * b_lo
	reg  signed [33:0] p_hh;       // a_hi * b_hi
	reg  [63:0] m_p;
	wire [16:0] ma_lo = m_a[16:0], mb_lo = m_b[16:0];
	wire signed [15:0] ma_hi = m_a[32:17], mb_hi = m_b[32:17];
	wire signed [65:0] m_sum = ($signed({{32{1'b0}}, p_ll})) +
	                           ($signed({{14{p_lh[34]}}, p_lh, 17'd0})) +
	                           ($signed({{14{p_hl[34]}}, p_hl, 17'd0})) +
	                           ($signed({p_hh, 34'd0}) >>> 0);

	// the divider: a bit a clock, on magnitudes
	reg  [31:0] dv_q, dv_r, dv_d;
	reg  [5:0]  dv_n;
	reg         dv_negq, dv_negr;
	// two bits a clock: two restoring steps in a row
	wire [33:0] dv_try  = {1'b0, dv_r, dv_q[31]} - {2'b00, dv_d};
	wire [31:0] dv_r1   = dv_try[33] ? {dv_r[30:0], dv_q[31]} : dv_try[31:0];
	wire [31:0] dv_q1   = {dv_q[30:0], !dv_try[33]};
	wire [33:0] dv_try2 = {1'b0, dv_r1, dv_q1[31]} - {2'b00, dv_d};

	// loads: the byte or half the address picks, extended
	// (the memories' word registered first: straight into the
	// write-back, two-cycle loads put the core at 62 MHz out of context)
	reg  [31:0] ld_word;
	reg  [31:0] ld_val;
	always @(*) begin
		case (e_f3[1:0])
		2'd0: case (e_addr[1:0])
		      2'd0: ld_val = {{24{ld_word[7]  & !e_f3[2]}}, ld_word[7:0]};
		      2'd1: ld_val = {{24{ld_word[15] & !e_f3[2]}}, ld_word[15:8]};
		      2'd2: ld_val = {{24{ld_word[23] & !e_f3[2]}}, ld_word[23:16]};
		      default: ld_val = {{24{ld_word[31] & !e_f3[2]}}, ld_word[31:24]};
		      endcase
		2'd1: ld_val = e_addr[1] ? {{16{ld_word[31] & !e_f3[2]}}, ld_word[31:16]}
		                         : {{16{ld_word[15] & !e_f3[2]}}, ld_word[15:0]};
		default: ld_val = ld_word;
		endcase
	end

	// redirect: a taken branch or a jump, decided in E's (only) cycle
	wire        e_redirect = e_valid && ((e_kind == K_BR && br_take) || e_kind == K_JAL || e_kind == K_JALR);
	wire [31:0] e_target   = (e_kind == K_JALR) ? {e_addr[31:1], 1'b0} : e_pc + e_imm;

	// E is busy (D and F hold) unless it has nothing or finishes now
	wire        e_free = !e_valid || e_done;
	assign f_en = e_free;
	assign fa = e_redirect ? e_target : (d_valid ? d_pc + 32'd4 : d_pc);

	// E's result this cycle (combinational), and its multi-cycle steps
	always @(*) begin
		e_done = 1'b1;
		wb_en = 1'b0;
		wb_rd = e_rd;
		wb_val = alu_y;
		e_mrd_i = 1'b0;
		e_mrd_d = 1'b0;
		e_mwe = 4'd0;
		e_maddr = e_addr;
		e_mwdata = e_b;
		if (e_valid) begin
			case (e_kind)
			K_ALU:   wb_en = 1'b1;
			K_LUI:   begin wb_en = 1'b1; wb_val = e_imm; end
			K_AUIPC: begin wb_en = 1'b1; wb_val = e_pc + e_imm; end
			K_JAL, K_JALR: begin wb_en = 1'b1; wb_val = e_pc4; end
			K_LOAD: begin
				// step 0: the address out; 1: the memory's word in,
				// written back (an I/O read waits for io_ready)
				if (e_st == 3'd0) begin
					e_done = 1'b0;
					e_mrd_i = (e_addr[17:16] == 2'b00);
					e_mrd_d = (e_addr[17:16] == 2'b01);
				end else if (e_st == 3'd1) begin
					e_done = 1'b0;
				end else begin
					wb_en = 1'b1;
					wb_val = ld_val;
				end
			end
			K_STORE: begin
				if (e_addr[17:16] == 2'b01) begin
					case (e_f3[1:0])
					2'd0: e_mwe = 4'b0001 << e_addr[1:0];
					2'd1: e_mwe = e_addr[1] ? 4'b1100 : 4'b0011;
					default: e_mwe = 4'b1111;
					endcase
					e_mwdata = (e_f3[1:0] == 2'd0) ? {4{e_b[7:0]}} :
					           (e_f3[1:0] == 2'd1) ? {2{e_b[15:0]}} : e_b;
				end else if (e_addr[17] == 1'b1) begin
					// I/O: step 0 asks, step 1 waits for the answer
					e_done = (e_st == 3'd1) && io_ready;
				end
			end
			K_MUL: begin
				// steps 0, 1: products, sum; 2: written back
				e_done = (e_st == 3'd2);
				wb_en = (e_st == 3'd2);
				wb_val = (e_f3 == 3'd0) ? m_p[31:0] : m_p[63:32];
			end
			K_DIV: begin
				e_done = (e_st == 3'd3);
				wb_en = (e_st == 3'd3);
				wb_val = e_f3[1] ? (dv_negr ? 32'd0 - dv_r : dv_r) : (dv_negq ? 32'd0 - dv_q : dv_q);
			end
			default: ;
			endcase
		end
	end

	// the I/O and multi-cycle state
	always @(posedge clk) begin
		if (rst) begin
			e_st <= 3'd0;
			io_rd <= 1'b0;
			io_wr <= 1'b0;
		end else begin
			io_rd <= 1'b0;
			io_wr <= 1'b0;
			if (e_valid && !e_done) begin
				case (e_kind)
				K_LOAD: begin
					if (e_st == 3'd0) begin
						if (e_addr[17]) begin
							io_rd <= 1'b1;
							io_addr <= e_addr[17:0];
						end
						e_st <= 3'd1;
					end else if (e_st == 3'd1) begin
						if (!e_addr[17]) begin
							ld_word <= (e_addr[16] ? m_rdata : d_rdata);
							e_st <= 3'd2;
						end else if (io_ready) begin
							ld_word <= io_rdata;
							e_st <= 3'd2;
						end
					end
				end
				K_STORE: begin
					if (e_st == 3'd0) begin
						io_wr <= 1'b1;
						io_addr <= e_addr[17:0];
						io_wdata <= e_b;
						e_st <= 3'd1;
					end
				end
				K_MUL: begin
					if (e_st == 3'd0) begin
						p_ll <= ma_lo * mb_lo;
						p_lh <= $signed({1'b0, ma_lo}) * mb_hi;
						p_hl <= ma_hi * $signed({1'b0, mb_lo});
						p_hh <= ma_hi * mb_hi;
					end else if (e_st == 3'd1) begin
						m_p <= m_sum[63:0];
					end
					e_st <= e_st + 3'd1;
				end
				K_DIV: begin
					if (e_st == 3'd0) begin
						// signed: magnitudes, the signs fixed at the end;
						// by zero: q all ones, r the dividend (RISC-V)
						dv_negq <= !e_f3[0] && (e_a[31] ^ e_b[31]) && (e_b != 32'd0);
						dv_negr <= !e_f3[0] && e_a[31];
						dv_q <= (!e_f3[0] && e_a[31]) ? 32'd0 - e_a : e_a;
						dv_d <= (!e_f3[0] && e_b[31]) ? 32'd0 - e_b : e_b;
						dv_r <= 32'd0;
						dv_n <= 6'd16;
						e_st <= 3'd1;
					end else if (e_st == 3'd1) begin
						dv_r <= dv_try2[33] ? {dv_r1[30:0], dv_q1[31]} : dv_try2[31:0];
						dv_q <= {dv_q1[30:0], !dv_try2[33]};
						dv_n <= dv_n - 6'd1;
						if (dv_n == 6'd1) e_st <= 3'd2;
					end else if (e_st == 3'd2) begin
						e_st <= 3'd3;
					end
				end
				default: ;
				endcase
			end else
				e_st <= 3'd0;
		end
	end

	// ------------------------------------------------------------------
	// the pipeline: F -> D -> E
	// ------------------------------------------------------------------

	always @(posedge clk) begin
		if (rst) begin
			d_valid <= 1'b0;
			d_pc <= 32'd0;              // the first fetch: d_pc itself (d_valid 0)
			e_valid <= 1'b0;
		end else if (e_free) begin
			// D's word moves to E (unless a redirect squashes it), and
			// the fetch address becomes D's
			d_pc <= fa;
			d_valid <= 1'b1;
			e_valid <= d_valid && !e_redirect;
			e_pc <= d_pc;
			e_rd <= d_rd;
			e_f3 <= d_f3;
			e_a <= d_a;
			e_b <= d_b;
			e_alt <= d_f7[5];
			e_immop <= 1'b0;
			e_imm <= imm_i;
			case (d_op)
			7'b0110111: begin e_kind <= K_LUI; e_imm <= imm_u; end
			7'b0010111: begin e_kind <= K_AUIPC; e_imm <= imm_u; end
			7'b1101111: begin e_kind <= K_JAL; e_imm <= imm_j; end
			7'b1100111: e_kind <= K_JALR;
			7'b1100011: begin e_kind <= K_BR; e_imm <= imm_b; end
			7'b0000011: e_kind <= K_LOAD;
			7'b0100011: begin e_kind <= K_STORE; e_imm <= imm_s; end
			7'b0010011: begin
				e_kind <= K_ALU;
				e_immop <= 1'b1;
				// SRAI's f7 bit is in the immediate; ADDI has none
				e_alt <= (d_f3 == 3'd5) ? d_f7[5] : 1'b0;
			end
			7'b0110011: e_kind <= (d_f7 == 7'b0000001) ? (d_f3[2] ? K_DIV : K_MUL) : K_ALU;
			default: e_kind <= K_NOP;
			endcase
		end
	end

endmodule
