/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Redistribution and use in source, binary or physical forms, with or
 * without modification, is permitted provided that the following
 * condition is met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * THIS HARDWARE, SOFTWARE, DATA AND/OR DOCUMENTATION ("THE ASSETS") IS
 * PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE ASSETS OR THE USE OR OTHER DEALINGS IN THE ASSETS. USE AT
 * YOUR OWN RISK.
 *
 */

/*
 * zeitlos32_decode -- instruction decode shared by zeitlos32 and
 * zeitlos32p.
 *
 * Purely combinational, from one 32-bit instruction word: its fields,
 * its five immediates, one wire per instruction class, and whether the
 * whole word is legal. Nothing here knows how instructions are
 * sequenced -- the multicycle core decodes its latched instruction, the
 * pipelined core the instruction in its decode stage -- which is why
 * it is a module of its own rather than part of either core.
 *
 * What is legal is picorv32's decode, reproduced exactly (see
 * zeitlos32.v's header): RV32IM, the four counter reads, FENCE as a
 * nop, and picorv32's custom-0 interrupt instructions.
 */

module zeitlos32_decode #(
	parameter [0:0] ENABLE_MUL      = 1,
	parameter [0:0] ENABLE_DIV      = 1,
	parameter [0:0] ENABLE_COUNTERS = 1
) (
	input  [31:0] instr,

	output  [6:0] opcode,
	output  [4:0] rd_idx,
	output  [2:0] funct3,
	output  [4:0] rs1_idx,
	output  [4:0] rs2_idx,
	output  [6:0] funct7,

	output [31:0] imm_i,
	output [31:0] imm_s,
	output [31:0] imm_b,
	output [31:0] imm_u,
	output [31:0] imm_j,

	output        is_lui,
	output        is_auipc,
	output        is_jal,
	output        is_jalr,
	output        is_branch,
	output        is_load,
	output        is_store,
	output        is_opimm,
	output        is_op,
	output        is_fence,
	output        is_system,
	output        is_custom,

	output        is_muldiv,

	output        is_getq,
	output        is_setq,
	output        is_retirq,
	output        is_maskirq,
	output        is_waitirq,

	output        is_ecall_ebreak,
	output        is_csr_ok,

	output        is_legal
);

	assign opcode  = instr[6:0];
	assign rd_idx  = instr[11:7];
	assign funct3  = instr[14:12];
	assign rs1_idx = instr[19:15];
	assign rs2_idx = instr[24:20];
	assign funct7  = instr[31:25];

	assign imm_i = {{20{instr[31]}}, instr[31:20]};
	assign imm_s = {{20{instr[31]}}, instr[31:25], instr[11:7]};
	assign imm_b =
		{{19{instr[31]}}, instr[31], instr[7], instr[30:25], instr[11:8], 1'b0};
	assign imm_u = {instr[31:12], 12'b0};
	assign imm_j =
		{{11{instr[31]}}, instr[31], instr[19:12], instr[20], instr[30:21], 1'b0};

	assign is_lui    = (opcode == 7'b0110111);
	assign is_auipc  = (opcode == 7'b0010111);
	assign is_jal    = (opcode == 7'b1101111);
	assign is_jalr   = (opcode == 7'b1100111) && (funct3 == 3'b000);
	assign is_branch = (opcode == 7'b1100011);
	assign is_load   = (opcode == 7'b0000011);
	assign is_store  = (opcode == 7'b0100011);
	assign is_opimm  = (opcode == 7'b0010011);
	assign is_op     = (opcode == 7'b0110011);
	assign is_fence  = (opcode == 7'b0001111);
	assign is_system = (opcode == 7'b1110011);
	assign is_custom = (opcode == 7'b0001011);

	wire is_mul = is_op && (funct7 == 7'b0000001) && !funct3[2];
	wire is_div = is_op && (funct7 == 7'b0000001) &&  funct3[2];
	assign is_muldiv = (is_mul && ENABLE_MUL) || (is_div && ENABLE_DIV);

	// picorv32's own encodings, see sw/bios/custom_ops.S
	assign is_getq    = is_custom && (funct7 == 7'b0000000) && (funct3 == 3'b100);
	assign is_setq    = is_custom && (funct7 == 7'b0000001) && (funct3 == 3'b010);
	assign is_retirq  = is_custom && (funct7 == 7'b0000010) && (funct3 == 3'b000);
	assign is_maskirq = is_custom && (funct7 == 7'b0000011) && (funct3 == 3'b110);
	assign is_waitirq = is_custom && (funct7 == 7'b0000100) && (funct3 == 3'b100);

	// ecall/ebreak: the whole instruction is zero apart from bit 20.
	// Matching picorv32's decode exactly rather than testing the
	// canonical encodings, so that anything it accepts, this accepts.
	assign is_ecall_ebreak = is_system && (instr[31:21] == 11'b0) && (instr[19:7] == 13'b0);

	// The only CSRs that exist. csrrs rd, <csr>, x0 with rs1 == x0 --
	// which is what the rdcycle/rdinstret pseudo-instructions expand
	// to. 0xc01 (time) aliases 0xc00 (cycle) because there is no
	// separate wall-clock counter on this SOC.
	wire is_csrread   = is_system && (funct3 == 3'b010) && (rs1_idx == 5'd0) && ENABLE_COUNTERS;
	wire csr_cycle    = is_csrread && ((instr[31:20] == 12'hc00) || (instr[31:20] == 12'hc01));
	wire csr_cycleh   = is_csrread && ((instr[31:20] == 12'hc80) || (instr[31:20] == 12'hc81));
	wire csr_instret  = is_csrread && (instr[31:20] == 12'hc02);
	wire csr_instreth = is_csrread && (instr[31:20] == 12'hc82);
	assign is_csr_ok = csr_cycle || csr_cycleh || csr_instret || csr_instreth;

	wire branch_legal = is_branch && (funct3 != 3'b010) && (funct3 != 3'b011);

	wire load_legal = is_load &&
		((funct3 == 3'b000) || (funct3 == 3'b001) || (funct3 == 3'b010) ||
		 (funct3 == 3'b100) || (funct3 == 3'b101));

	wire store_legal = is_store &&
		((funct3 == 3'b000) || (funct3 == 3'b001) || (funct3 == 3'b010));

	// The funct7 field is only meaningful for OP and for the two
	// shift-immediates; decoding it strictly rather than ignoring it
	// means a corrupted instruction word faults instead of quietly
	// executing something adjacent.
	wire op_legal = is_op &&
		((funct7 == 7'b0000000) ||
		 ((funct7 == 7'b0100000) && ((funct3 == 3'b000) || (funct3 == 3'b101))) ||
		 ((funct7 == 7'b0000001) && is_muldiv));

	wire opimm_legal = is_opimm &&
		((funct3 != 3'b001 && funct3 != 3'b101) ||
		 ((funct3 == 3'b001) && (funct7 == 7'b0000000)) ||
		 ((funct3 == 3'b101) && ((funct7 == 7'b0000000) || (funct7 == 7'b0100000))));

	assign is_legal =
		is_lui || is_auipc || is_jal || is_jalr || branch_legal ||
		load_legal || store_legal || opimm_legal || op_legal || is_fence ||
		is_ecall_ebreak || is_csr_ok ||
		is_getq || is_setq || is_retirq || is_maskirq || is_waitirq;

endmodule
