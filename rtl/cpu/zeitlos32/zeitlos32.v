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
 * zeitlos32 -- a small RV32IM core with a native wishbone master.
 *
 * -- What this is --
 *
 * A drop-in alternative to rtl/cpu/picorv32/picorv32.v's picorv32_wb,
 * selected by `CPU_ZEITLOS32 in rtl/boards.vh. It implements exactly
 * what this SOC and this OS actually use and nothing else:
 *
 *   - RV32IM (no compressed, no atomics, no CSR file beyond the
 *     four counter reads below)
 *   - rdcycle / rdcycleh / rdinstret / rdinstreth, because
 *     sw/os/kernel.c and sw/os/sh.c read them directly
 *   - picorv32's custom interrupt ABI (getq/setq/retirq/maskirq/
 *     waitirq), so sw/bios/boot_picorv32.S and sw/bios/custom_ops.S
 *     need no changes at all
 *   - a wishbone classic master, native -- there is no picorv32_wb
 *     style adapter wrapping a different internal bus
 *
 * Deliberately NOT implemented, because nothing in this tree uses
 * them: PCPI, the picorv32 IRQ timer (`ENABLE_IRQ_TIMER`, wired to 0
 * in rtl/sysctl.v), compressed instructions, trace, and the
 * two-cycle-ALU / non-barrel-shifter size options.
 *
 * -- Structure --
 *
 * A multicycle FSM, not a pipeline. Every instruction walks the same
 * short path and nothing is ever in flight in two places at once,
 * which is the whole point: there are no hazards to reason about, no
 * forwarding network, and no stall logic. The cost is cycles per
 * instruction, and on this SOC that cost is close to free -- main
 * memory takes ~11 cycles (SDRAM) to ~63 cycles (PSRAM) per word, so
 * fetch latency dominates CPI regardless of what the core does
 * internally. See docs/zeitlos32.md for the measured numbers.
 *
 *   ST_FETCH   decide whether to take an interrupt; if not, issue the
 *              instruction fetch on the bus
 *   ST_FWAIT   wait for ack, latch the instruction
 *   ST_RS      read rs1/rs2 out of the register file
 *   ST_EX      execute: ALU, branch decision, address calculation.
 *              Most instructions retire here.
 *   ST_LWAIT   load: wait for ack, extract/extend, retire
 *   ST_SWAIT   store: wait for ack, retire
 *   ST_MULDIV  wait for the sequential multiplier/divider
 *   ST_WAITIRQ waitirq stalled with nothing pending
 *   ST_TRAP    halted
 *
 * That is 5 cycles for an ALU instruction against a slave that acks
 * in one cycle, 8 for a load. picorv32 in the configuration this
 * replaces is ~5-6 and ~9-10 respectively (rtl/tb/tb_soc.v measures
 * IPC 0.172 for a register-only loop even WITH the instruction
 * cache), so this is roughly at parity. It is not trying to be
 * faster.
 *
 * -- Why the bus outputs are registered --
 *
 * Every wishbone output is driven from a flip-flop, same as
 * picorv32_wb. In rtl/sysctl.v the CPU's address feeds wb_mtu
 * (rtl/mtu.v) combinationally and then a large chip-select mux, so
 * an unregistered address out of the core would put the whole
 * decode fabric in the same timing path as the core's own logic.
 * The cost is one cycle of bus turnaround per access and it is worth
 * paying -- see docs/muldiv.md for how little margin this design has
 * historically had at 48MHz.
 *
 * -- Interrupts --
 *
 * The semantics here are picorv32's, reproduced deliberately rather
 * than reinvented, because sw/bios/boot_picorv32.S is a fixed
 * interface this core has to satisfy:
 *
 *   - irq_pending accumulates from the `irq` input every cycle.
 *     Bits set in LATCHED_IRQ stay set until they are delivered;
 *     bits clear in LATCHED_IRQ follow the input level. rtl/sysctl.v
 *     passes 32'hffff_ffef, i.e. everything latched except IRQ 4
 *     (the 16550 UART), which is level.
 *   - An interrupt is taken at an instruction boundary when
 *     (irq_pending & ~irq_mask) is nonzero and the core is not
 *     already in a handler. q0 gets the address of the instruction
 *     that did NOT run, q1 gets the delivered bitmask, and execution
 *     continues at PROGADDR_IRQ.
 *   - waitirq stalls until irq_pending is nonzero, then hands the
 *     whole pending set to software. It does NOT clear it: picorv32
 *     leaves the bits pending, and sw/bios/boot_picorv32.S relies on
 *     that -- its reset path is waitirq followed immediately by
 *     maskirq(zero, zero), and the interrupt it just waited for is
 *     supposed to be delivered the instant the mask opens. Clearing
 *     here would silently swallow the first timer tick. Delivery is
 *     the only thing that clears a pending bit.
 *   - retirq restores the PC from q0 and leaves the handler.
 *   - Exactly one instruction always executes after retirq before
 *     another interrupt can be taken (irq_delay below). This
 *     guarantees forward progress when a level-triggered source is
 *     still asserted on return -- without it a stuck UART interrupt
 *     would livelock the handler.
 *   - irq_mask resets to all-ones (everything masked). The BIOS's
 *     reset vector opens it with maskirq(zero, zero).
 *
 * A note on the reset vector, since it looks like a bug and is not:
 * sw/bios/boot_picorv32.S starts with waitirq. irq_pending fills
 * from the `irq` input regardless of irq_mask, so the core parks
 * there until the first RTC tick (rtl/sysctl.v's irq_timer, IRQ 3)
 * and then proceeds. That is the behaviour picorv32 has and the BIOS
 * depends on it.
 *
 * -- One deliberate superset --
 *
 * FENCE is accepted as a nop. picorv32 has no FENCE decode at all and
 * faults on it. Accepting an instruction picorv32 rejects cannot
 * break anything that runs on picorv32 today, and it means a future
 * compiler emitting fence for rv32im does not need a core change.
 * rtl/cpu/zeitlos32/tests/prog/t_alu.S covers it, and is consequently
 * the one test in that directory that does NOT also pass on
 * picorv32.
 *
 * -- Traps --
 *
 * Illegal instructions and ecall/ebreak raise IRQ 1; misaligned
 * accesses raise IRQ 2 -- again matching picorv32 with CATCH_ILLINSN
 * and CATCH_MISALIGN set, which is how rtl/sysctl.v instantiates it.
 * If the relevant interrupt is masked, or the core is already inside
 * a handler, it halts and asserts `trap` instead (rtl/sysctl.v shows
 * that on LED_R).
 *
 */

module zeitlos32_wb #(
	parameter [31:0] PROGADDR_RESET = 32'h0000_0000,
	parameter [31:0] PROGADDR_IRQ   = 32'h0000_0010,
	parameter [31:0] STACKADDR      = 32'hffff_ffff,
	parameter [31:0] LATCHED_IRQ    = 32'hffff_ffff,
	parameter [31:0] MASKED_IRQ     = 32'h0000_0000,
	parameter [0:0]  ENABLE_MUL     = 1,
	parameter [0:0]  ENABLE_DIV     = 1,
	parameter [0:0]  FAST_MUL       = 0,
	parameter [0:0]  ENABLE_COUNTERS = 1,
	// Zero the register file at power-on. picorv32 calls this
	// REGS_INIT_ZERO and defaults it off; here it defaults ON because
	// it costs nothing on any FPGA this targets (the initial values
	// are part of the LUT-RAM/BRAM init) and it makes simulation
	// deterministic, which matters a great deal for the test suite in
	// tests/.
	parameter [0:0]  REGS_INIT_ZERO = 1
) (
	// wishbone classic master
	input wb_clk_i,
	input wb_rst_i,

	output reg [31:0] wbm_adr_o,
	output reg [31:0] wbm_dat_o,
	input      [31:0] wbm_dat_i,
	output reg        wbm_we_o,
	output reg  [3:0] wbm_sel_o,
	output reg        wbm_stb_o,
	input             wbm_ack_i,
	output reg        wbm_cyc_o,

	// high for the whole of an instruction-fetch cycle. rtl/cache.v
	// uses this to cache fetches only, which is what keeps data
	// coherency out of the picture entirely.
	output reg        mem_instr,

	// interrupts
	input      [31:0] irq,
	output reg [31:0] eoi,

	// halted on an unrecoverable fault
	output reg        trap
);

	// ------------------------------------------------------------
	// state
	// ------------------------------------------------------------

	localparam ST_FETCH   = 4'd0;
	localparam ST_FWAIT   = 4'd1;
	localparam ST_RS      = 4'd2;
	localparam ST_EX      = 4'd3;
	localparam ST_LWAIT   = 4'd4;
	localparam ST_SWAIT   = 4'd5;
	localparam ST_MULDIV  = 4'd6;
	localparam ST_WAITIRQ = 4'd7;
	localparam ST_TRAP    = 4'd8;

	reg [3:0] state;

	reg [31:0] pc;
	reg [31:0] instr;
	reg [31:0] rs1_q;
	reg [31:0] rs2_q;

	// The ALU's second operand and shift amount, selected during
	// ST_RS rather than during ST_EX. Both are pure functions of the
	// already-latched instruction, so choosing them a cycle early
	// costs 37 flops and takes the immediate extraction and the
	// register-vs-immediate mux off the ST_EX critical path -- which
	// is otherwise instr -> decode -> mux -> 32-bit adder -> result
	// mux -> register file, in one cycle. Measured on ECP5 that was
	// the difference between missing and making 60MHz.
	reg [31:0] alu_b_q;
	reg  [4:0] shamt_q;
	reg [1:0]  ld_lo;			// low address bits of a pending load

	reg [31:0] regs [0:31];

	// The four picorv32 IRQ scratch registers. Kept as ordinary flops
	// rather than as entries 32-35 of the register file (which is what
	// picorv32 does): the file would grow to 36 entries and every read
	// port with it, whereas these four are only ever touched by five
	// instructions.
	reg [31:0] q0, q1, q2, q3;

	reg [31:0] irq_mask;
	reg [31:0] irq_pending;
	reg [31:0] irq_pending_n;		// blocking, see the always block
	reg        irq_active;
	reg        irq_delay;

	reg [63:0] count_cycle;
	reg [63:0] count_instr;

	reg        md_start;

	// The register file's single write port. These three are blocking
	// helpers, like irq_pending_n below: the FSM sets them wherever it
	// wants to write a register, and the ONE `regs[...] <=` statement
	// at the bottom of the always block commits it.
	//
	// Writing `regs[rd_idx] <= ...` directly from each branch instead
	// is the obvious way to express this and it is a trap. Every such
	// statement is a separate write port as far as yosys is concerned,
	// and merging eighteen of them into the one port a LUT-RAM
	// actually has costs well over a thousand LUT4s. picorv32 funnels
	// every write through one point (cpuregs_write / cpuregs_wrdata)
	// for exactly this reason.
	reg        rf_we;
	reg  [4:0] rf_idx;
	reg [31:0] rf_dat;

	// Instruction retirement, funnelled the same way and for the same
	// reason. `retire` says an instruction finished this cycle and
	// `next_pc` says where to go; both are committed once at the
	// bottom of the always block.
	//
	// Writing `pc <= ...` and `count_instr <= ...` from each branch
	// instead builds a priority mux chain as deep as the decode chain
	// -- and count_instr is 64 bits wide, so that alone was worth
	// hundreds of LUT4s. The FSM reads no more clearly for it.
	reg        retire;
	reg [31:0] next_pc;

	integer i;

	initial begin
		if (REGS_INIT_ZERO) begin
			for (i = 0; i < 32; i = i + 1) regs[i] = 32'd0;
		end
	end

	// ------------------------------------------------------------
	// decode (purely combinational, from the latched instruction)
	// ------------------------------------------------------------

	wire [6:0] opcode = instr[6:0];
	wire [4:0] rd_idx = instr[11:7];
	wire [2:0] funct3 = instr[14:12];
	wire [4:0] rs1_idx = instr[19:15];
	wire [4:0] rs2_idx = instr[24:20];
	wire [6:0] funct7 = instr[31:25];

	wire [31:0] imm_i = {{20{instr[31]}}, instr[31:20]};
	wire [31:0] imm_s = {{20{instr[31]}}, instr[31:25], instr[11:7]};
	wire [31:0] imm_b =
		{{19{instr[31]}}, instr[31], instr[7], instr[30:25], instr[11:8], 1'b0};
	wire [31:0] imm_u = {instr[31:12], 12'b0};
	wire [31:0] imm_j =
		{{11{instr[31]}}, instr[31], instr[19:12], instr[20], instr[30:21], 1'b0};

	wire is_lui    = (opcode == 7'b0110111);
	wire is_auipc  = (opcode == 7'b0010111);
	wire is_jal    = (opcode == 7'b1101111);
	wire is_jalr   = (opcode == 7'b1100111) && (funct3 == 3'b000);
	wire is_branch = (opcode == 7'b1100011);
	wire is_load   = (opcode == 7'b0000011);
	wire is_store  = (opcode == 7'b0100011);
	wire is_opimm  = (opcode == 7'b0010011);
	wire is_op     = (opcode == 7'b0110011);
	wire is_fence  = (opcode == 7'b0001111);
	wire is_system = (opcode == 7'b1110011);
	wire is_custom = (opcode == 7'b0001011);

	wire is_mul    = is_op && (funct7 == 7'b0000001) && !funct3[2];
	wire is_div    = is_op && (funct7 == 7'b0000001) &&  funct3[2];
	wire is_muldiv = (is_mul && ENABLE_MUL) || (is_div && ENABLE_DIV);

	// picorv32's own encodings, see sw/bios/custom_ops.S
	wire is_getq    = is_custom && (funct7 == 7'b0000000) && (funct3 == 3'b100);
	wire is_setq    = is_custom && (funct7 == 7'b0000001) && (funct3 == 3'b010);
	wire is_retirq  = is_custom && (funct7 == 7'b0000010) && (funct3 == 3'b000);
	wire is_maskirq = is_custom && (funct7 == 7'b0000011) && (funct3 == 3'b110);
	wire is_waitirq = is_custom && (funct7 == 7'b0000100) && (funct3 == 3'b100);

	// ecall/ebreak: the whole instruction is zero apart from bit 20.
	// Matching picorv32's decode exactly rather than testing the
	// canonical encodings, so that anything it accepts, this accepts.
	wire is_ecall_ebreak = is_system && (instr[31:21] == 11'b0) && (instr[19:7] == 13'b0);

	// The only CSRs that exist. csrrs rd, <csr>, x0 with rs1 == x0 --
	// which is what the rdcycle/rdinstret pseudo-instructions expand
	// to. 0xc01 (time) aliases 0xc00 (cycle) because there is no
	// separate wall-clock counter on this SOC.
	wire is_csrread = is_system && (funct3 == 3'b010) && (rs1_idx == 5'd0) && ENABLE_COUNTERS;
	wire csr_cycle   = is_csrread && ((instr[31:20] == 12'hc00) || (instr[31:20] == 12'hc01));
	wire csr_cycleh  = is_csrread && ((instr[31:20] == 12'hc80) || (instr[31:20] == 12'hc81));
	wire csr_instret = is_csrread && (instr[31:20] == 12'hc02);
	wire csr_instreth = is_csrread && (instr[31:20] == 12'hc82);
	wire is_csr_ok = csr_cycle || csr_cycleh || csr_instret || csr_instreth;

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

	wire is_legal =
		is_lui || is_auipc || is_jal || is_jalr || branch_legal ||
		load_legal || store_legal || opimm_legal || op_legal || is_fence ||
		is_ecall_ebreak || is_csr_ok ||
		is_getq || is_setq || is_retirq || is_maskirq || is_waitirq;

	// ------------------------------------------------------------
	// register file reads
	//
	// Asynchronous, two ports, exactly as picorv32 does it. On ECP5
	// and GateMate this maps to distributed RAM; the read happens in
	// ST_RS and the result is flopped, so nothing downstream ever
	// sees the RAM output combinationally.
	// ------------------------------------------------------------

	wire [31:0] rs1_rd = (rs1_idx == 5'd0) ? 32'd0 : regs[rs1_idx];
	wire [31:0] rs2_rd = (rs2_idx == 5'd0) ? 32'd0 : regs[rs2_idx];

	// ------------------------------------------------------------
	// ALU
	// ------------------------------------------------------------

	wire [31:0] alu_a = rs1_q;
	wire [31:0] alu_b = alu_b_q;
	wire [4:0]  shamt = shamt_q;

	// what ST_RS latches into those two
	wire [31:0] alu_b_sel = (is_op || is_branch) ? rs2_rd :
	                        is_store ? imm_s : imm_i;
	wire [4:0]  shamt_sel = is_op ? rs2_rd[4:0] : instr[24:20];
	wire        alu_sub = is_op && funct7[5];
	wire        alu_sra = funct7[5];

	wire [31:0] alu_add = alu_a + alu_b;
	wire [31:0] alu_subr = alu_a - alu_b;
	wire        alu_lt  = ($signed(alu_a) < $signed(alu_b));
	wire        alu_ltu = (alu_a < alu_b);
	wire        alu_eq  = (alu_a == alu_b);
	wire [31:0] alu_shl = alu_a << shamt;
	wire [32:0] alu_shr = $signed({alu_sra & alu_a[31], alu_a}) >>> shamt;

	reg [31:0] alu_out;

	always @* begin
		case (funct3)
			3'b000:  alu_out = alu_sub ? alu_subr : alu_add;
			3'b001:  alu_out = alu_shl;
			3'b010:  alu_out = {31'd0, alu_lt};
			3'b011:  alu_out = {31'd0, alu_ltu};
			3'b100:  alu_out = alu_a ^ alu_b;
			3'b101:  alu_out = alu_shr[31:0];
			3'b110:  alu_out = alu_a | alu_b;
			default: alu_out = alu_a & alu_b;
		endcase
	end

	reg branch_taken;

	always @* begin
		case (funct3)
			3'b000:  branch_taken =  alu_eq;		// beq
			3'b001:  branch_taken = !alu_eq;		// bne
			3'b100:  branch_taken =  alu_lt;		// blt
			3'b101:  branch_taken = !alu_lt;		// bge
			3'b110:  branch_taken =  alu_ltu;		// bltu
			3'b111:  branch_taken = !alu_ltu;		// bgeu
			default: branch_taken = 1'b0;			// funct3 2/3 illegal
		endcase
	end

	// ------------------------------------------------------------
	// addresses
	// ------------------------------------------------------------

	wire [31:0] pc_plus4 = pc + 32'd4;
	wire [31:0] mem_adr = alu_add;						// rs1 + imm_i / imm_s
	wire [31:0] jalr_adr = {alu_add[31:1], 1'b0};
	wire [31:0] branch_adr = pc + imm_b;
	wire [31:0] jal_adr = pc + imm_j;

	// misalignment of the access itself
	wire mem_misaligned =
		(funct3[1:0] == 2'b10) ? (mem_adr[1:0] != 2'b00) :
		(funct3[1:0] == 2'b01) ? (mem_adr[0]   != 1'b0)  : 1'b0;

	// ------------------------------------------------------------
	// store lane steering
	// ------------------------------------------------------------

	reg [3:0]  st_sel;
	reg [31:0] st_dat;

	always @* begin
		case (funct3[1:0])
			2'b00: begin							// sb
				st_dat = {4{rs2_q[7:0]}};
				case (mem_adr[1:0])
					2'b00: st_sel = 4'b0001;
					2'b01: st_sel = 4'b0010;
					2'b10: st_sel = 4'b0100;
					default: st_sel = 4'b1000;
				endcase
			end
			2'b01: begin							// sh
				st_dat = {2{rs2_q[15:0]}};
				st_sel = mem_adr[1] ? 4'b1100 : 4'b0011;
			end
			default: begin							// sw
				st_dat = rs2_q;
				st_sel = 4'b1111;
			end
		endcase
	end

	// ------------------------------------------------------------
	// load extraction
	// ------------------------------------------------------------

	reg [7:0]  ld_byte;
	reg [15:0] ld_half;
	reg [31:0] ld_out;

	always @* begin
		case (ld_lo)
			2'b00:   ld_byte = wbm_dat_i[7:0];
			2'b01:   ld_byte = wbm_dat_i[15:8];
			2'b10:   ld_byte = wbm_dat_i[23:16];
			default: ld_byte = wbm_dat_i[31:24];
		endcase
		ld_half = ld_lo[1] ? wbm_dat_i[31:16] : wbm_dat_i[15:0];
		case (funct3)
			3'b000:  ld_out = {{24{ld_byte[7]}}, ld_byte};		// lb
			3'b001:  ld_out = {{16{ld_half[15]}}, ld_half};		// lh
			3'b100:  ld_out = {24'd0, ld_byte};					// lbu
			3'b101:  ld_out = {16'd0, ld_half};					// lhu
			default: ld_out = wbm_dat_i;						// lw
		endcase
	end

	// ------------------------------------------------------------
	// sequential multiplier / divider
	// ------------------------------------------------------------

	wire        md_ready;
	wire [31:0] md_result;

	zeitlos32_muldiv #(
		.ENABLE_MUL(ENABLE_MUL),
		.ENABLE_DIV(ENABLE_DIV),
		.FAST_MUL(FAST_MUL)
	) muldiv_i (
		.clk(wb_clk_i),
		.rst(wb_rst_i),
		.start(md_start),
		.op(funct3),
		.a(rs1_q),
		.b(rs2_q),
		.ready(md_ready),
		.result(md_result)
	);

	// ------------------------------------------------------------
	// interrupt arbitration
	// ------------------------------------------------------------

	wire [31:0] irq_delivered = irq_pending & ~irq_mask;
	wire        irq_take = (|irq_delivered) && !irq_active && !irq_delay;

	// ------------------------------------------------------------
	// main sequential process
	//
	// Everything the core owns is driven from this one always block:
	// one driver per signal, no exceptions. irq_pending_n is the sole
	// blocking-assignment variable, used the same way picorv32 uses
	// next_irq_pending -- it collects the several independent things
	// that can modify the pending set in a single cycle (delivery,
	// waitirq, a fault, and the incoming `irq` lines) and is committed
	// once at the bottom.
	// ------------------------------------------------------------

	always @(posedge wb_clk_i) begin

		irq_pending_n = irq_pending & LATCHED_IRQ;

		rf_we = 1'b0;
		rf_idx = rd_idx;
		rf_dat = 32'd0;

		retire = 1'b0;

		// Where the next instruction comes from, if one retires this
		// cycle. Kept out of the opcode case below so that the case
		// selects 32 fewer bits per leg. Measured, this is a wash on
		// area -- but it keeps the redirect decision in one readable
		// place instead of spread across four legs of the case.
		next_pc = pc_plus4;
		if (is_jal)                    next_pc = jal_adr;
		else if (is_jalr)              next_pc = jalr_adr;
		else if (is_branch && branch_taken) next_pc = branch_adr;
		else if (is_retirq)            next_pc = q0;

		md_start <= 1'b0;

		if (ENABLE_COUNTERS) count_cycle <= count_cycle + 64'd1;

		if (wb_rst_i) begin

			pc <= PROGADDR_RESET;
			state <= ST_FETCH;
			instr <= 32'd0;
			rs1_q <= 32'd0;
			rs2_q <= 32'd0;
			alu_b_q <= 32'd0;
			shamt_q <= 5'd0;
			ld_lo <= 2'b00;

			wbm_adr_o <= 32'd0;
			wbm_dat_o <= 32'd0;
			wbm_we_o <= 1'b0;
			wbm_sel_o <= 4'd0;
			wbm_stb_o <= 1'b0;
			wbm_cyc_o <= 1'b0;
			mem_instr <= 1'b0;

			trap <= 1'b0;
			eoi <= 32'd0;

			q0 <= 32'd0;
			q1 <= 32'd0;
			q2 <= 32'd0;
			q3 <= 32'd0;

			irq_mask <= ~32'd0;
			irq_pending_n = 32'd0;
			irq_active <= 1'b0;
			irq_delay <= 1'b0;

			count_cycle <= 64'd0;
			count_instr <= 64'd0;

			// picorv32 initialises x2 from STACKADDR at reset and
			// sw/bios/boot_picorv32.S does not depend on it (it sets sp
			// itself), but code that runs before that does, so keep it.
			rf_we = 1'b1;
			rf_idx = 5'd2;
			rf_dat = STACKADDR;

		end else begin

			case (state)

			// --------------------------------------------------
			ST_FETCH: begin
				// Interrupt entry happens here, at the boundary
				// between two instructions, with pc holding the
				// address of the one that has not run yet.
				if (irq_take) begin
					q0 <= pc;
					q1 <= irq_delivered;
					eoi <= irq_delivered;
					irq_pending_n = irq_pending_n & irq_mask;
					irq_active <= 1'b1;
					pc <= PROGADDR_IRQ;
					// stay in ST_FETCH: next cycle issues the fetch
					// from the handler address
				end else begin
					irq_delay <= irq_active;
					if (pc[1:0] != 2'b00) begin
						// A misaligned PC can only come from a bad
						// branch offset or a bad q0 -- jalr masks bit
						// 0 and every other target is aligned by
						// encoding. Note irq_delay is cleared above
						// even on this path, so the fault is always
						// delivered on the next cycle rather than
						// spinning here.
						if (!irq_mask[2] && !irq_active)
							irq_pending_n[2] = 1'b1;
						else
							state <= ST_TRAP;
					end else begin
						wbm_adr_o <= {pc[31:2], 2'b00};
						wbm_dat_o <= 32'd0;
						wbm_we_o <= 1'b0;
						wbm_sel_o <= 4'b1111;
						wbm_stb_o <= 1'b1;
						wbm_cyc_o <= 1'b1;
						mem_instr <= 1'b1;
						state <= ST_FWAIT;
					end
				end
			end

			// --------------------------------------------------
			ST_FWAIT: begin
				if (wbm_ack_i) begin
					wbm_stb_o <= 1'b0;
					wbm_cyc_o <= 1'b0;
					mem_instr <= 1'b0;
					instr <= wbm_dat_i;
					state <= ST_RS;
				end
			end

			// --------------------------------------------------
			ST_RS: begin
				rs1_q <= rs1_rd;
				rs2_q <= rs2_rd;
				alu_b_q <= alu_b_sel;
				shamt_q <= shamt_sel;
				state <= ST_EX;
			end

			// --------------------------------------------------
			ST_EX: begin

				// Most instructions finish here. The ones that do not
				// clear `retire` and name their own next state.
				retire = 1'b1;

				if (!is_legal) begin

					// Illegal instruction. Not executed; retired, with
					// IRQ 1 raised for the next boundary. See the fault
					// notes in the header.
					if (!irq_mask[1] && !irq_active) begin
						irq_pending_n[1] = 1'b1;
					end else begin
						retire = 1'b0;
						state <= ST_TRAP;
					end

				end else begin

					// A case on the opcode, NOT an if/else chain over
					// the is_* decode wires. Those wires are all still
					// here and still drive the datapath, but selecting
					// between them one at a time builds a priority mux
					// as deep as the chain is long, on every signal
					// written from more than one branch. A case on
					// five bits gets a balanced tree instead. It was
					// worth about a thousand LUT4s.
					//
					// opcode[1:0] is 2'b11 for every instruction this
					// core accepts (that is what marks a 32-bit
					// instruction), so only opcode[6:2] carries
					// information.
					case (opcode[6:2])

					// ---- LUI ----
					5'b01101: begin
						rf_we = 1'b1;
						rf_dat = imm_u;
					end

					// ---- AUIPC ----
					5'b00101: begin
						rf_we = 1'b1;
						rf_dat = pc + imm_u;
					end

					// ---- JAL / JALR ----
					5'b11011, 5'b11001: begin
						rf_we = 1'b1;
						rf_dat = pc_plus4;
					end

					// ---- BRANCH ----
					5'b11000: begin
						// next_pc handles the redirect
					end

					// ---- LOAD ----
					5'b00000: begin
						if (mem_misaligned) begin
							if (!irq_mask[2] && !irq_active) begin
								irq_pending_n[2] = 1'b1;
							end else begin
								retire = 1'b0;
								state <= ST_TRAP;
							end
						end else begin
							wbm_adr_o <= {mem_adr[31:2], 2'b00};
							wbm_dat_o <= 32'd0;
							wbm_we_o <= 1'b0;
							wbm_sel_o <= 4'b1111;
							wbm_stb_o <= 1'b1;
							wbm_cyc_o <= 1'b1;
							ld_lo <= mem_adr[1:0];
							retire = 1'b0;		// deferred to ST_LWAIT
							state <= ST_LWAIT;
						end
					end

					// ---- STORE ----
					5'b01000: begin
						if (mem_misaligned) begin
							if (!irq_mask[2] && !irq_active) begin
								irq_pending_n[2] = 1'b1;
							end else begin
								retire = 1'b0;
								state <= ST_TRAP;
							end
						end else begin
							wbm_adr_o <= {mem_adr[31:2], 2'b00};
							wbm_dat_o <= st_dat;
							wbm_we_o <= 1'b1;
							wbm_sel_o <= st_sel;
							wbm_stb_o <= 1'b1;
							wbm_cyc_o <= 1'b1;
							retire = 1'b0;		// deferred to ST_SWAIT
							state <= ST_SWAIT;
						end
					end

					// ---- OP-IMM / OP ----
					5'b00100, 5'b01100: begin
						if (is_muldiv) begin
							md_start <= 1'b1;
							retire = 1'b0;	// deferred to ST_MULDIV
							state <= ST_MULDIV;
						end else begin
							rf_we = 1'b1;
							rf_dat = alu_out;
						end
					end

					// ---- FENCE ----
					//
					// Nothing to order on a single-issue in-order core
					// with a write-through bus. Accepted as a nop so a
					// newer compiler emitting fence does not fault.
					5'b00011: begin
					end

					// ---- SYSTEM ----
					5'b11100: begin
						if (is_ecall_ebreak) begin
							if (!irq_mask[1] && !irq_active) begin
								irq_pending_n[1] = 1'b1;
							end else begin
								retire = 1'b0;
								state <= ST_TRAP;
							end
						end else begin
							// The only CSRs there are. is_legal has
							// already rejected anything else, so this
							// picks between the four counter halves on
							// two bits.
							rf_we = 1'b1;
							case ({instr[27], instr[21]})
								2'b00: rf_dat = count_cycle[31:0];
								2'b01: rf_dat = count_instr[31:0];
								2'b10: rf_dat = count_cycle[63:32];
								default: rf_dat = count_instr[63:32];
							endcase
						end
					end

					// ---- picorv32 custom-0 ----
					default: begin
						case (funct7[2:0])

						3'b000: begin					// getq
							rf_we = 1'b1;
							case (rs1_idx[1:0])
								2'd0: rf_dat = q0;
								2'd1: rf_dat = q1;
								2'd2: rf_dat = q2;
								default: rf_dat = q3;
							endcase
						end

						3'b001: begin					// setq
							case (rd_idx[1:0])
								2'd0: q0 <= rs1_q;
								2'd1: q1 <= rs1_q;
								2'd2: q2 <= rs1_q;
								default: q3 <= rs1_q;
							endcase
						end

						3'b010: begin					// retirq
							irq_active <= 1'b0;
							eoi <= 32'd0;
						end

						3'b011: begin					// maskirq
							rf_we = 1'b1;
							rf_dat = irq_mask;
							irq_mask <= rs1_q;
						end

						default: begin					// waitirq
							// Reports the pending set without
							// consuming it -- see the interrupt notes
							// in the header.
							if (|irq_pending) begin
								rf_we = 1'b1;
								rf_dat = irq_pending;
							end else begin
								retire = 1'b0;
								state <= ST_WAITIRQ;
							end
						end

						endcase
					end

					endcase

				end
			end

			// --------------------------------------------------
			ST_LWAIT: begin
				if (wbm_ack_i) begin
					wbm_stb_o <= 1'b0;
					wbm_cyc_o <= 1'b0;
					wbm_we_o <= 1'b0;
					rf_we = 1'b1;
					rf_dat = ld_out;
					retire = 1'b1;
				end
			end

			// --------------------------------------------------
			ST_SWAIT: begin
				if (wbm_ack_i) begin
					wbm_stb_o <= 1'b0;
					wbm_cyc_o <= 1'b0;
					wbm_we_o <= 1'b0;
					retire = 1'b1;
				end
			end

			// --------------------------------------------------
			ST_MULDIV: begin
				if (md_ready) begin
					rf_we = 1'b1;
					rf_dat = md_result;
					retire = 1'b1;
				end
			end

			// --------------------------------------------------
			ST_WAITIRQ: begin
				// Note that no interrupt can be TAKEN from here --
				// entry is only ever decided in ST_FETCH. waitirq
				// hands the pending set to software and returns.
				if (|irq_pending) begin
					rf_we = 1'b1;
					rf_dat = irq_pending;
					retire = 1'b1;
				end
			end

			// --------------------------------------------------
			ST_TRAP: begin
				trap <= 1'b1;
				wbm_stb_o <= 1'b0;
				wbm_cyc_o <= 1'b0;
				wbm_we_o <= 1'b0;
				mem_instr <= 1'b0;
			end

			// --------------------------------------------------
			default: state <= ST_FETCH;

			endcase

		end

		// The one and only register file write.
		if (rf_we && (rf_idx != 5'd0)) regs[rf_idx] <= rf_dat;

		// ...and the one and only instruction retirement.
		if (retire) begin
			pc <= next_pc;
			state <= ST_FETCH;
			if (ENABLE_COUNTERS) count_instr <= count_instr + 64'd1;
		end

		// Incoming lines are folded in last, after any clearing above,
		// so a level-triggered source that is still asserted re-pends
		// immediately -- see the LATCHED_IRQ discussion in the header.
		irq_pending_n = irq_pending_n | irq;
		irq_pending <= irq_pending_n & ~MASKED_IRQ;

	end

endmodule
