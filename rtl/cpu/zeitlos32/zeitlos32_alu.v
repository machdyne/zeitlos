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
 * zeitlos32_alu -- the integer ALU and branch compare shared by
 * zeitlos32 and zeitlos32p.
 *
 * Purely combinational. `out` is the OP / OP-IMM result selected by
 * funct3; `add` is a + b on its own, because both cores also use it as
 * the load/store address (rs1 + imm) and the jalr target; `taken` is
 * the branch decision for a BRANCH with the same funct3, from the same
 * comparator.
 *
 * Who chooses b (rs2 or an immediate) and the shift amount is the
 * core's business, not this module's: zeitlos32 chooses them a cycle
 * early, in ST_RS, to keep that mux off its execute path.
 */

module zeitlos32_alu (
	input  [31:0] a,
	input  [31:0] b,
	input   [4:0] shamt,
	input   [2:0] funct3,
	input         sub,		// funct3 0: a - b instead of a + b
	input         sra,		// funct3 5: arithmetic shift

	output reg [31:0] out,
	output     [31:0] add,
	output reg        taken
);

	assign add = a + b;

	wire [31:0] subr = a - b;
	wire        lt   = ($signed(a) < $signed(b));
	wire        ltu  = (a < b);
	wire        eq   = (a == b);
	wire [31:0] shl  = a << shamt;
	wire [32:0] shr  = $signed({sra & a[31], a}) >>> shamt;

	always @* begin
		case (funct3)
			3'b000:  out = sub ? subr : add;
			3'b001:  out = shl;
			3'b010:  out = {31'd0, lt};
			3'b011:  out = {31'd0, ltu};
			3'b100:  out = a ^ b;
			3'b101:  out = shr[31:0];
			3'b110:  out = a | b;
			default: out = a & b;
		endcase
	end

	always @* begin
		case (funct3)
			3'b000:  taken =  eq;		// beq
			3'b001:  taken = !eq;		// bne
			3'b100:  taken =  lt;		// blt
			3'b101:  taken = !lt;		// bge
			3'b110:  taken =  ltu;		// bltu
			3'b111:  taken = !ltu;		// bgeu
			default: taken = 1'b0;		// funct3 2/3 illegal
		endcase
	end

endmodule
