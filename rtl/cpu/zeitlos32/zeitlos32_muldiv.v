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
 * zeitlos32 -- RV32M multiply and divide
 *
 * -- Approach --
 *
 * Both operations run on magnitudes and fix the sign afterwards.
 * That is a deliberate choice over the more compact trick of
 * sign-extending the operands and letting the arithmetic sort itself
 * out: the magnitude version is directly checkable against the
 * definition of the instruction, so a failing case in tests/ points
 * at one line rather than at an argument about sign extension. The
 * cost is three conditional negations, which is a rounding error next
 * to the 32-step loop itself.
 *
 * Multiply is a 32-step shift-add. Divide is 32-step restoring
 * division. Both share the step counter but not the adder -- sharing
 * the adder saves perhaps 40 LUTs and costs a good deal of clarity,
 * so it is left as a future optimisation and noted in
 * docs/zeitlos32.md rather than done here.
 *
 * -- FAST_MUL --
 *
 * With FAST_MUL set, multiply becomes a single 33x33 signed multiply
 * expression, registered once. On ECP5 yosys maps that onto MULT18X18D
 * blocks and it completes in 2 cycles instead of ~35. This is the same
 * trade picorv32 makes with ENABLE_FAST_MUL, and docs/muldiv.md
 * measured it as both faster AND smaller there (the multiply leaves
 * the LUT fabric entirely).
 *
 * It is NOT the default, because it is only a good trade on FPGAs with
 * DSP blocks that the flow will actually use. rtl/../Makefile passes
 * -nomult to synth_gatemate for the Lebkuchen and Koelsch boards,
 * which means a FAST_MUL build there gets a full 32x32 combinational
 * multiplier in LUTs -- large, and squarely on the critical path.
 * rtl/boards.vh's `CPU_MUL_FAST selects it; leave that undefined on
 * GateMate.
 *
 * -- Latency --
 *
 *   FAST_MUL=0  mul: 34 cycles   div: 35 cycles
 *   FAST_MUL=1  mul:  3 cycles   div: 35 cycles
 *
 * Divide is always sequential. docs/muldiv.md notes divides are far
 * rarer than multiplies in this codebase, and a fast divider is a
 * much worse area/timing trade than a fast multiplier.
 *
 */

module zeitlos32_muldiv #(
	parameter [0:0] ENABLE_MUL = 1,
	parameter [0:0] ENABLE_DIV = 1,
	parameter [0:0] FAST_MUL   = 0
) (
	input             clk,
	input             rst,

	// single-cycle pulse; op/a/b must be valid on that cycle
	input             start,
	input       [2:0] op,			// funct3: 0-3 mul family, 4-7 div family
	input      [31:0] a,
	input      [31:0] b,

	output reg        ready,
	output reg [31:0] result
);

	localparam MD_IDLE = 2'd0;
	localparam MD_RUN  = 2'd1;
	localparam MD_FIX  = 2'd2;
	localparam MD_FAST = 2'd3;

	reg [1:0]  state;
	reg [5:0]  cnt;
	reg [2:0]  op_q;

	reg [63:0] prod;
	reg [31:0] mplier;
	reg [31:0] mcand;

	reg [32:0] rem;
	reg [31:0] quot;
	reg [31:0] dvnd;
	reg [31:0] dsor;

	reg        neg_q;			// negate the quotient / product
	reg        neg_r;			// negate the remainder

	// ------------------------------------------------------------
	// operand classification
	// ------------------------------------------------------------

	// MUL only needs the low 32 bits, and those are identical whether
	// the operands are read as signed or unsigned -- so it is treated
	// as unsigned and skips the sign fix entirely.
	wire mul_a_signed = (op == 3'b001) || (op == 3'b010);	// mulh, mulhsu
	wire mul_b_signed = (op == 3'b001);						// mulh
	wire div_signed   = (op == 3'b100) || (op == 3'b110);	// div, rem

	wire a_neg = (op[2] ? div_signed : mul_a_signed) && a[31];
	wire b_neg = (op[2] ? div_signed : mul_b_signed) && b[31];

	wire [31:0] a_abs = a_neg ? (~a + 32'd1) : a;
	wire [31:0] b_abs = b_neg ? (~b + 32'd1) : b;

	wire [63:0] prod_neg = ~prod + 64'd1;
	wire [31:0] quot_neg = ~quot + 32'd1;
	wire [31:0] rem_neg  = ~rem[31:0] + 32'd1;

	// ------------------------------------------------------------
	// one shift-add step
	//
	// sum is 33 bits so the carry out of the partial product survives
	// the right shift that follows it.
	// ------------------------------------------------------------

	wire [32:0] mul_sum = {1'b0, prod[63:32]} + {1'b0, mcand};
	wire [32:0] mul_top = mplier[0] ? mul_sum : {1'b0, prod[63:32]};

	// ------------------------------------------------------------
	// one restoring-division step
	// ------------------------------------------------------------

	// One subtractor, not a comparator AND a subtractor. The borrow
	// out of the subtract already answers "was it big enough", so
	// asking separately builds a second 33-bit carry chain for an
	// answer that is sitting right there.
	wire [33:0] rem_shifted = {1'b0, rem[31:0], dvnd[31]};
	wire [33:0] rem_sub = rem_shifted - {2'b00, dsor};
	wire        rem_ge = !rem_sub[33];

	// ------------------------------------------------------------
	// fast multiply (optional)
	// ------------------------------------------------------------

	// Wrapped in a generate so that a FAST_MUL=0 build cannot infer a
	// multiplier at all, rather than relying on the synthesiser to
	// notice the result is unreachable.
	// The DSP operands are REGISTERED before the multiply. Feeding
	// fm_a/fm_b straight from a/b makes one combinational path out of
	// the register file read, the sign-extension mux, four cascaded
	// MULT18X18D blocks and the partial-product adder tree -- around
	// 11ns on ECP5, which is most of a 60MHz cycle spent in an
	// instruction that already takes several. Registering costs one
	// cycle (fast multiply goes from 2 to 3) and buys back the clock.
	reg signed [32:0] fm_a;
	reg signed [32:0] fm_b;
	wire signed [65:0] fm_p;

	wire signed [32:0] fm_a_sel = $signed({mul_a_signed & a[31], a});
	wire signed [32:0] fm_b_sel = $signed({mul_b_signed & b[31], b});

	generate
		if (FAST_MUL) begin : g_fast_mul
			assign fm_p = fm_a * fm_b;
		end else begin : g_no_fast_mul
			assign fm_p = 66'd0;
		end
	endgenerate

	// ENABLE_MUL and ENABLE_DIV have to reach the datapath, not just
	// the caller. zeitlos32.v already refuses to issue a disabled
	// operation, but the synthesiser cannot know that, so without
	// these the divider is built in full for an ENABLE_DIV=0 build --
	// the parameter looks like it works and saves nothing.
	wire do_mul = !op[2] && ENABLE_MUL;
	wire do_div =  op[2] && ENABLE_DIV;
	wire mul_run = !op_q[2] && ENABLE_MUL;
	wire div_run =  op_q[2] && ENABLE_DIV;

	always @(posedge clk) begin

		if (rst) begin

			state <= MD_IDLE;
			ready <= 1'b0;
			cnt <= 6'd0;
			op_q <= 3'd0;
			prod <= 64'd0;
			mplier <= 32'd0;
			mcand <= 32'd0;
			rem <= 33'd0;
			quot <= 32'd0;
			dvnd <= 32'd0;
			dsor <= 32'd0;
			fm_a <= 33'd0;
			fm_b <= 33'd0;
			neg_q <= 1'b0;
			neg_r <= 1'b0;

		end else begin

			ready <= 1'b0;

			case (state)

			MD_IDLE: begin
				if (start) begin
					op_q <= op;
					cnt <= 6'd32;
					if (do_mul) begin
						// ---- multiply ----
						neg_r <= 1'b0;
						if (FAST_MUL) begin
							// The signed multiply already
							// produces a correctly signed 64-bit
							// product, so there is nothing left for
							// the sign fix to do. Leaving neg_q set
							// here negates it a second time, which is
							// wrong only for operand pairs whose
							// signs differ -- i.e. it passes every
							// test that uses positive numbers.
							neg_q <= 1'b0;
							fm_a <= fm_a_sel;
							fm_b <= fm_b_sel;
							state <= MD_FAST;
						end else begin
							neg_q <= (a_neg ^ b_neg);
							prod <= 64'd0;
							mcand <= a_abs;
							mplier <= b_abs;
							state <= MD_RUN;
						end
					end else if (do_div) begin
						// ---- divide ----
						neg_q <= (a_neg ^ b_neg);
						neg_r <= a_neg;
						rem <= 33'd0;
						quot <= 32'd0;
						dvnd <= a_abs;
						dsor <= b_abs;
						if (b == 32'd0) begin
							// Division by zero is defined, not a
							// fault: quotient all ones, remainder the
							// dividend. RISC-V spec, "Division
							// Operations".
							quot <= 32'hffff_ffff;
							rem <= {1'b0, a};
							neg_q <= 1'b0;
							neg_r <= 1'b0;
							state <= MD_FIX;
						end else if (div_signed && (a == 32'h8000_0000) &&
								(b == 32'hffff_ffff)) begin
							// Signed overflow: quotient is the
							// dividend, remainder zero. Also defined.
							quot <= 32'h8000_0000;
							rem <= 33'd0;
							neg_q <= 1'b0;
							neg_r <= 1'b0;
							state <= MD_FIX;
						end else begin
							state <= MD_RUN;
						end
					end
				end
			end

			// FAST_MUL only: the cycle the DSP array gets to itself.
			MD_FAST: begin
				prod <= fm_p[63:0];
				state <= MD_FIX;
			end

			MD_RUN: begin
				if (mul_run) begin
					prod <= {mul_top, prod[31:1]};
					mplier <= {1'b0, mplier[31:1]};
				end else if (div_run) begin
					dvnd <= {dvnd[30:0], 1'b0};
					rem <= rem_ge ? rem_sub[32:0] : rem_shifted[32:0];
					quot <= {quot[30:0], rem_ge};
				end
				cnt <= cnt - 6'd1;
				if (cnt == 6'd1) state <= MD_FIX;
			end

			// One cycle to let the final MD_RUN step (or the fast/
			// special path above) land in prod/quot/rem before those
			// are read. The sign fix itself is combinational, in the
			// result mux below -- doing it here as well would negate
			// twice.
			MD_FIX: begin
				state <= MD_IDLE;
				ready <= 1'b1;
			end

			default: state <= MD_IDLE;

			endcase

		end
	end

	// Result selection and sign correction, read on the cycle `ready`
	// is high. op_q, neg_q, neg_r and the datapath registers all hold
	// their values until the next start, so this is stable.
	//
	// This block is the ONLY driver of `result`. It is combinational,
	// so it must not also be assigned in the sequential block above --
	// not even in the reset branch. Doing so is legal Verilog and
	// simulates fine (the last write in source order wins), but yosys
	// sees two drivers on one net, warns "multiple conflicting
	// drivers", and resolves the conflict by tying the net to the
	// constant from the reset branch. The result is a core that
	// passes every simulation and returns zero from every multiply
	// and divide on real hardware.
	always @* begin
		case (op_q)
			3'b000:  result = neg_q ? prod_neg[31:0]  : prod[31:0];		// mul
			3'b001,
			3'b010,
			3'b011:  result = neg_q ? prod_neg[63:32] : prod[63:32];	// mulh*
			3'b100,
			3'b101:  result = neg_q ? quot_neg : quot;					// div, divu
			default: result = neg_r ? rem_neg  : rem[31:0];				// rem, remu
		endcase
	end

endmodule
