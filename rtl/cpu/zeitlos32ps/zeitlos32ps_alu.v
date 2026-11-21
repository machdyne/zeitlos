/*
 * Zeitlos SOC -- zeitlos32ps ALU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zeitlos32_alu's ports and results, plus (ENABLE_ROT) Zbb's ror, rol
 * and rori, as picorv32's ENABLE_ROT and rtl/boards.vh's `CPU_ROT.
 *
 * All the shifts are one funnel shifter: the low 32 bits of
 * {hi, lo} >> k.
 *
 *   srl, sra   hi = 0 or a's sign      lo = a   k = n
 *   ror        hi = a                  lo = a   k = n
 *   rol        hi = a                  lo = a   k = -n (mod 32)
 *   sll        hi = a                  lo = 0   k = -n (mod 32)
 *              (n = 0: lo = a, so that the result is a, not 0)
 *
 * which replaces zeitlos32_alu's separate left and right shifters with
 * one, slightly wider; the rotates come almost free.
 *
 * rot: the instruction is a rotate (funct7 0110000); funct3 001 is rol,
 * 101 ror/rori. The core decides whether it is legal.
 */

module zeitlos32ps_alu #(
	parameter [0:0] ENABLE_ROT = 0
) (
	input  [31:0] a,
	input  [31:0] b,
	input   [4:0] shamt,
	input   [2:0] funct3,
	input         sub,		// funct3 0: a - b instead of a + b
	input         sra,		// funct3 5: arithmetic shift
	input         rot,		// funct3 1/5: rotate (ENABLE_ROT)
	input         cmp,		// a branch, slt or sltu: a - b (not a load/store's lw/sw funct3)

	output reg [31:0] out,
	output     [31:0] add,
	output reg        taken
);

	// One adder for add, sub and the compares: a + ~b + 1 when
	// subtracting (sub, slt/sltu, branches), whose carry and sign give
	// ltu and lt. add is only used (addresses, jalr) when it adds.
	wire        inv  = cmp || ((funct3 == 3'b000) && sub);
	wire [32:0] sum  = {1'b0, a} + {1'b0, b ^ {32{inv}}} + {32'd0, inv};
	assign add = sum[31:0];
	wire        ltu  = !sum[32];
	wire        lt   = (a[31] ^ b[31]) ? a[31] : sum[31];
	wire        eq   = (a == b);

	wire        r    = ENABLE_ROT && rot;
	wire        left = !funct3[2];			// sll, rol
	wire  [4:0] k    = left ? (5'd0 - shamt) : shamt;
	wire        lo_a = !left || r || (shamt == 5'd0);
	wire [31:0] hi   = (left || r) ? a : {32{sra & a[31]}};
	wire [31:0] lo   = lo_a ? a : 32'd0;
	// five 2:1 stages, each only as wide as what can still reach the
	// low 32 bits. (Kept as separate modules (keep_hierarchy) the
	// shifter alone is a third smaller, but the whole Lakritz build came
	// out 169 cells larger, so they are not kept.)
	wire [62:0] s0 = {hi[30:0], lo};
	wire [46:0] s1;
	wire [38:0] s2;
	wire [34:0] s3;
	wire [32:0] s4;
	wire [31:0] sh;
	zeitlos32ps_mx2 #(47) st16 (k[4], s0[46:0], s0[62:16], s1);
	zeitlos32ps_mx2 #(39) st8  (k[3], s1[38:0], s1[46:8],  s2);
	zeitlos32ps_mx2 #(35) st4  (k[2], s2[34:0], s2[38:4],  s3);
	zeitlos32ps_mx2 #(33) st2  (k[1], s3[32:0], s3[34:2],  s4);
	zeitlos32ps_mx2 #(32) st1  (k[0], s4[31:0], s4[32:1],  sh);

	always @* begin
		case (funct3)
			3'b000:  out = sum[31:0];
			3'b001:  out = sh;
			3'b010:  out = {31'd0, lt};
			3'b011:  out = {31'd0, ltu};
			3'b100:  out = a ^ b;
			3'b101:  out = sh;
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

// a 2:1 mux, W wide (see the shifter above)
module zeitlos32ps_mx2 #(
	parameter W = 1
) (
	input          s,
	input  [W-1:0] a,
	input  [W-1:0] b,
	output [W-1:0] y
);
	assign y = s ? b : a;
endmodule
