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
 * zeitlos32ps -- the small pipelined zeitlos32: RV32IM for the 25F
 * boards, in about the area picorv32 takes there.
 *
 * The same machine as zeitlos32 and zeitlos32p (RV32IM, the counter
 * reads, FENCE as a no-op, picorv32's interrupt ABI and IRQ timer,
 * zeitlos32's fault rules), sequenced differently:
 *
 *   F  fetch: pc_f, translated by the MTU, to the memory port; the
 *      answer goes into a three-entry instruction queue
 *   D  the queue's head: register read, with a bypass from X's write
 *   X  everything else: decode, ALU, branches and jumps, loads and
 *      stores (X waits for the answer), multiply and divide, the
 *      custom instructions, faults and interrupts; the register
 *      write happens as X finishes
 *
 * ONE memory port for fetches and data, with rtl/cache_split.v's
 * handshake: a request is taken on req && rdy, answered by ack (with
 * the data) at the earliest the next cycle, one outstanding, rdy high
 * again in the ack cycle. A load or store in X has the port first.
 *
 * A redirect (taken branch, jump, retirq, interrupt) is decided in X;
 * fetching restarts at the target the next cycle. Nothing after X can
 * cancel anything, and X is the only stage that changes architectural
 * state, so an interrupt is taken at the boundary before the
 * instruction in X, exactly as zeitlos32 takes it before a fetch.
 */

module zeitlos32ps #(
	parameter [31:0] PROGADDR_RESET = 32'h0000_0000,
	parameter [31:0] PROGADDR_IRQ   = 32'h0000_0010,
	parameter [31:0] STACKADDR      = 32'hffff_ffff,
	parameter [31:0] LATCHED_IRQ    = 32'hffff_ffff,
	parameter [31:0] MASKED_IRQ     = 32'h0000_0000,
	parameter [0:0]  ENABLE_MUL     = 1,
	parameter [0:0]  ENABLE_DIV     = 1,
	parameter [0:0]  FAST_MUL       = 0,
	parameter [0:0]  ENABLE_COUNTERS = 1,
	parameter [0:0]  REGS_INIT_ZERO = 1,
	// 1: track each instruction's privilege for rtl/mpu_one.v (the
	// mpu_*, p_*, fv_*, m_priv, m_pc, i_priv ports); 0: no MPU, and
	// all of it is constant and synthesises away
	parameter [0:0]  ENABLE_MPU     = 0,
	// 1: Zbb's ror, rol and rori, as picorv32's ENABLE_ROT (`CPU_ROT)
	parameter [0:0]  ENABLE_ROT     = 0,
	// 1: static prediction in D -- jal and backward branches are taken
	// there (the fetch is redirected as the instruction moves to X), and
	// X corrects a backward branch that is not taken. Off: on Lakritz it
	// cost about 520 cells for 4.9% fewer cycles (tb_cache_soc 195,056 ->
	// 185,508), which does not fit the footprint. Tested (all suites
	// pass with it on).
	parameter [0:0]  BTFN           = 0
) (
	input clk,
	input rst,

	// -- the MPU (rtl/mpu_one.v), as zeitlos32p connects it --
	input         mpu_en,
	input         mpu_enforce,
	// the MPU's verdict on the address of the last fetch taken, valid
	// when its answer comes (m_ack): fetching it would make code
	// privileged / it is the gate or the IRQ vector / it is inside the
	// app's block. Kept with the instruction, used in X.
	input         p_privreg,
	input         p_gate,
	input         p_own,
	output        m_priv,			// the data request's instruction is privileged
	output [31:0] m_pc,				// ...and its address (physical)
	output        i_priv,			// the last instruction executed was privileged
	output reg        fv_ev,		// an instruction fetch violated
	output reg [31:0] fv_adr,		// ...its address (physical)
	output reg [31:0] fv_pc,		// ...the instruction executed before it (physical)
	output reg  [3:0] fv_why,		// ...wb_mpu's reason: 1 or 4

	// the memory port (rtl/cache_split.v's port handshake)
	output        m_req,
	output [31:0] m_adr,			// physical, word aligned
	output        m_we,
	output  [3:0] m_sel,
	output [31:0] m_wdat,
	output        m_instr,			// this request is a fetch
	input         m_rdy,
	input         m_ack,
	input  [31:0] m_rdat,

	input  [31:0] mtu_base,

	input      [31:0] irq,
	output reg [31:0] eoi,

	output reg        trap
);

	// ============================================================
	// state
	// ============================================================

	// fetch
	reg [31:0] pc_f;				// next address to fetch
	reg        f_out;				// a fetch is outstanding
	reg        f_kill;				// ...and its answer is to be dropped
	reg        f_halt;				// stop fetching until a redirect
	reg        out_fetch;			// the outstanding request is a fetch

	// the instruction queue: three entries in a ring, all sequential
	// from d_pc (written at iq_wp, read at iq_rp)
	reg  [1:0] iq_cnt;
	reg  [1:0] iq_rp, iq_wp;
	reg [31:0] iq_i0, iq_i1, iq_i2;
	reg        iq_m0, iq_m1, iq_m2;	// misaligned-fetch marker
	reg  [2:0] iq_p0, iq_p1, iq_p2;	// the MPU's p_* for it (ENABLE_MPU)
	// ...and the physical address it was fetched from (ENABLE_MPU): the
	// MPU records physical fault addresses and PCs, as wb_mpu does, and
	// this is cheaper than translating them there
	reg [31:2] iq_a0, iq_a1, iq_a2;
	reg [31:2] f_pq;				// the outstanding fetch's
	reg [31:2] x_ppc;				// X's instruction's
	reg [31:0] d_pc;				// address of the head

	// X
	reg        x_valid;
	reg        x_first;				// first cycle of this instruction in X
	reg        x_late;				// third cycle or later
	reg [31:0] x_instr;
	reg [31:0] x_pc;
	reg        x_mis;				// a misaligned-fetch marker, not an instruction
	reg        x_pt;				// D predicted it taken (BTFN)
	reg [31:0] x_a;					// rs1, as read in D
	reg [31:0] x_b;					// rs2, as read in D
	reg        x_fa, x_fb;			// ...but take W's value instead

	// W: the register write, one cycle after X
	reg        w_we;
	reg  [4:0] w_rd;
	reg [31:0] w_val;
	reg [31:0] w_pc;				// for the testbenches
	reg        x_issued;			// its load/store request has been taken
	reg        x_preg, x_pgate, x_pown;	// the MPU's checks on its address

	// privilege of the last instruction executed, and its address
	reg        priv_x;
	reg [31:2] lastx_ppc;

	reg [31:0] regs [0:31];

	reg [31:0] q0, q1, q2, q3;
	reg [31:0] irq_mask;
	reg [31:0] irq_pending;
	reg [31:0] irq_pending_n;		// blocking, inside the clocked block
	reg        irq_active;
	reg        irq_delay;

	reg [31:0] timer_q;

	reg [63:0] count_cycle;
	reg [63:0] count_instr;

	integer i;
	initial begin
		if (REGS_INIT_ZERO)
			for (i = 0; i < 32; i = i + 1) regs[i] = 32'd0;
	end

	// ============================================================
	// X: decode
	// ============================================================

	wire  [6:0] opcode;
	wire  [4:0] rd_idx;
	wire  [2:0] funct3;
	wire  [4:0] rs1_idx;
	wire  [4:0] rs2_idx;
	wire  [6:0] funct7;
	wire [31:0] imm_i, imm_s, imm_b, imm_u, imm_j;
	wire is_lui, is_auipc, is_jal, is_jalr, is_branch, is_load, is_store;
	wire is_opimm, is_op, is_fence, is_system, is_custom;
	wire is_muldiv;
	wire is_getq, is_setq, is_retirq, is_maskirq, is_waitirq;
	wire is_ecall_ebreak, is_csr_ok, is_legal;

	zeitlos32_decode #(
		.ENABLE_MUL(ENABLE_MUL),
		.ENABLE_DIV(ENABLE_DIV),
		.ENABLE_COUNTERS(ENABLE_COUNTERS)
	) decode_i (
		.instr(x_instr),
		.opcode(opcode), .rd_idx(rd_idx), .funct3(funct3),
		.rs1_idx(rs1_idx), .rs2_idx(rs2_idx), .funct7(funct7),
		.imm_i(imm_i), .imm_s(imm_s), .imm_b(imm_b), .imm_u(imm_u), .imm_j(imm_j),
		.is_lui(is_lui), .is_auipc(is_auipc), .is_jal(is_jal), .is_jalr(is_jalr),
		.is_branch(is_branch), .is_load(is_load), .is_store(is_store),
		.is_opimm(is_opimm), .is_op(is_op), .is_fence(is_fence),
		.is_system(is_system), .is_custom(is_custom),
		.is_muldiv(is_muldiv),
		.is_getq(is_getq), .is_setq(is_setq), .is_retirq(is_retirq),
		.is_maskirq(is_maskirq), .is_waitirq(is_waitirq),
		.is_ecall_ebreak(is_ecall_ebreak), .is_csr_ok(is_csr_ok),
		.is_legal(is_legal)
	);

	// picorv32's IRQ timer: custom-0, funct7 0000101, funct3 110
	// (zeitlos32_decode, shared with zeitlos32, which has no timer, calls
	// it illegal; this core accepts it on its own, as zeitlos32p does)
	wire is_timer = is_custom && (funct7 == 7'b0000101) && (funct3 == 3'b110);
	// Zbb's rotates (ENABLE_ROT): funct7 0110000; rol and ror in OP,
	// rori in OP-IMM. The decoder calls them illegal; with ENABLE_ROT
	// this core does not.
	wire is_rot   = ENABLE_ROT && (funct7 == 7'b0110000) &&
		((is_op && (funct3 == 3'b001 || funct3 == 3'b101)) ||
		 (is_opimm && funct3 == 3'b101));
	wire x_legal  = is_legal || is_timer || is_rot;
	wire x_cust   = is_getq || is_setq || is_retirq || is_maskirq || is_waitirq || is_timer;

	// ============================================================
	// X: execute
	// ============================================================

	// X's operands: as read in D, or the result of the instruction just
	// before, which is in W now (the selects are registers, set in D)
	wire [31:0] xa = x_fa ? w_val : x_a;
	wire [31:0] xb = x_fb ? w_val : x_b;

	wire [31:0] alu_b = (is_op || is_branch) ? xb : is_store ? imm_s : imm_i;
	wire  [4:0] shamt = is_op ? xb[4:0] : x_instr[24:20];

	wire [31:0] alu_out;
	wire [31:0] alu_add;
	wire        br_taken;

	zeitlos32ps_alu #(
		.ENABLE_ROT(ENABLE_ROT)
	) alu_i (
		.a(xa),
		.b(alu_b),
		.shamt(shamt),
		.funct3(funct3),
		.sub(is_op && funct7[5]),
		.sra(funct7[5]),
		.rot(funct7[5:4] == 2'b11),		// 0110000; sra is 0100000
		.cmp(is_branch || ((is_op || is_opimm) && funct3[2:1] == 2'b01)),
		.out(alu_out),
		.add(alu_add),
		.taken(br_taken)
	);

	wire [31:0] pc_plus4 = x_pc + 32'd4;
	wire [31:0] mem_adr  = alu_add;

	wire mem_misaligned =
		(funct3[1:0] == 2'b10) ? (mem_adr[1:0] != 2'b00) :
		(funct3[1:0] == 2'b01) ? (mem_adr[0]   != 1'b0)  : 1'b0;

	// The data address, translated: in the 0x8 window it is mtu_base
	// plus the offset, i.e. va + (mtu_base - 0x8000_0000), and
	// subtracting 0x8000_0000 flips bit 31 -- so a three-input sum,
	// formed carry-save next to the plain one (as zeitlos32p does).
	wire        mtu_en  = (mtu_base != 32'h0);
	wire [31:0] data_va = { mem_adr[31:2], 2'b00 };
	wire [31:0] mtu_off = { ~mtu_base[31], mtu_base[30:2], 2'b00 };
	wire [31:0] csa_s   = xa ^ alu_b ^ mtu_off;
	wire [31:0] csa_c   = { (xa[30:0] & alu_b[30:0]) | (xa[30:0] & mtu_off[30:0]) |
	                        (alu_b[30:0] & mtu_off[30:0]), 1'b0 };
	wire [31:0] mtu_sum = csa_s + csa_c;
	wire [31:0] x_padr  = (mtu_en && data_va[31:28] == 4'h8) ?
		{ mtu_sum[31:2], mtu_base[1:0] } : data_va;

	wire [3:0]  st_sel;
	wire [31:0] st_dat;

	zeitlos32_store store_i (
		.size(funct3[1:0]),
		.adr_lo(mem_adr[1:0]),
		.rs2(xb),
		.sel(st_sel),
		.dat(st_dat)
	);

	wire [31:0] ld_out;

	// The load's low address bits, kept from the cycle it is taken: X's
	// operand registers are reloaded from D while it waits (d_ld_ab).
	reg  [1:0]  x_lo;

	zeitlos32_load load_i (
		.funct3(funct3),
		.adr_lo(x_lo),
		.word(m_rdat),
		.out(ld_out)
	);

	wire        md_ready;
	wire [31:0] md_result;
	wire        md_start;

	zeitlos32_muldiv_lean #(
		.ENABLE_MUL(ENABLE_MUL),
		.ENABLE_DIV(ENABLE_DIV),
		.FAST_MUL(FAST_MUL)
	) muldiv_i (
		.clk(clk),
		.rst(rst),
		.start(md_start),
		.op(funct3),
		.a(xa),
		.b(xb),
		.ready(md_ready),
		.result(md_result)
	);

	// ============================================================
	// X: control
	// ============================================================

	wire [31:0] irq_delivered = irq_pending & ~irq_mask;
	// at the boundary before the instruction in X, never once it has
	// started (a load issued, a divide begun, waitirq waiting)
	wire irq_take = x_valid && x_first && !trap &&
		(|irq_delivered) && !irq_active && !irq_delay;

	wire x_go    = x_valid && !trap && !irq_take;	// X acts this cycle
	wire x_mem   = is_load || is_store;
	// -- the MPU: was fetching this instruction allowed? Decided here, in
	// program order, with the privilege of the instruction before it
	// (zeitlos32p's rule). With ENFORCE a violating instruction is an
	// illegal one, as wb_mpu's blocked fetch returns 0x0000_0000.
	wire       mpu_on    = ENABLE_MPU && mpu_en;
	wire [3:0] fv_reason = x_preg ? (x_pgate ? 4'd0 : 4'd4) :
	                                (x_pown  ? 4'd0 : 4'd1);
	wire       fv        = mpu_on && x_valid && !x_mis && !priv_x &&
	                       (fv_reason != 4'd0);
	wire       x_fblk    = fv && mpu_enforce;
	// this instruction's privilege: for its own load or store, and for
	// the next instruction
	wire       x_newpriv = !mpu_on ? 1'b1 : (x_fblk ? priv_x : x_preg);

	wire x_fault1 = !x_legal || is_ecall_ebreak || x_fblk;	// IRQ 1
	wire x_fault2 = x_mem && !x_issued && mem_misaligned;	// IRQ 2

	// X's load or store wants the port
	wire x_dreq = x_go && !x_mis && x_legal && !x_fblk && x_mem && !mem_misaligned && !x_issued;

	assign md_start = x_go && x_first && !x_mis && x_legal && is_muldiv;

	// One adder for everything PC-relative: auipc's result and the jal
	// and branch targets (one instruction at a time needs it).
	wire [31:0] pcrel = x_pc + (is_jal ? imm_j : is_branch ? imm_b : imm_u);

	// The rare results, read through small muxes of their own.
	wire [31:0] q_sel   = rs1_idx[1] ? (rs1_idx[0] ? q3 : q2) : (rs1_idx[0] ? q1 : q0);
	wire [31:0] cnt_sel = x_instr[27] ?
		(x_instr[21] ? count_instr[63:32] : count_cycle[63:32]) :
		(x_instr[21] ? count_instr[31:0]  : count_cycle[31:0]);

	// X's instruction is real and legal and acts this cycle
	wire x_bad  = x_fault1 || x_fault2;
	wire x_ok   = x_go && !x_mis && !x_bad;

	// ...and finishes this cycle: most at once, loads and stores on
	// their answer, multiply/divide when the unit is ready, the custom
	// instructions in their third cycle (waitirq once something is
	// pending)
	// (written apart, so that what does not depend on m_ack is built
	// without it: m_ack arrives late, from the cache's tag compare)
	wire x_fin_nm = is_muldiv ? md_ready :
	              x_cust   ? (x_late && (!is_waitirq || (|irq_pending))) :
	              1'b1;
	wire x_fin  = x_mem    ? (x_issued && m_ack) : x_fin_nm;

	// a faulting instruction is skipped when its interrupt can be taken
	wire x_skip = !irq_mask[x_fault2 ? 2 : 1] && !irq_active;

	wire x_retire = x_go && !x_mis && (x_bad ? x_skip : x_fin);

	// Which result: the decoder's classes are mutually exclusive, so the
	// result is an AND-OR of one-hot selects rather than a priority chain
	wire s_alu  = is_opimm || (is_op && !is_muldiv);
	wire s_lui  = is_lui;
	wire s_pcr  = is_auipc;
	wire s_link = is_jal || is_jalr;
	wire s_ld   = is_load;
	wire s_md   = is_muldiv;
	wire s_cnt  = is_system;
	wire s_q    = is_getq;
	wire s_mask = is_maskirq;
	wire s_tmr  = is_timer;
	wire s_pend = is_waitirq;

	wire x_we = x_ok && x_fin &&
		(s_alu || s_lui || s_pcr || s_link || s_ld || s_md || s_cnt ||
		 s_q || s_mask || s_tmr || s_pend);

	wire [31:0] x_wdat =
		({32{s_alu}}  & alu_out)     | ({32{s_lui}}  & imm_u)     |
		({32{s_pcr}}  & pcrel)       | ({32{s_link}} & pc_plus4)  |
		({32{s_ld}}   & ld_out)      | ({32{s_md}}   & md_result) |
		({32{s_cnt}}  & cnt_sel)     | ({32{s_q}}    & q_sel)     |
		({32{s_mask}} & irq_mask)    | ({32{s_tmr}}  & timer_q)   |
		({32{s_pend}} & irq_pending);

	// Redirects: an interrupt, a jump, a taken branch, retirq
	wire r_pcr  = x_ok && !x_pt && (is_jal || (is_branch && br_taken));
	wire r_nt   = x_ok && x_pt && is_branch && !br_taken;	// mispredicted
	wire r_jalr = x_ok && is_jalr;
	wire r_ret  = x_ok && is_retirq && x_fin_nm;

	wire x_redir = irq_take || r_pcr || r_jalr || r_ret || r_nt;

	wire [31:0] x_target =
		({32{irq_take}} & PROGADDR_IRQ)          |
		({32{r_pcr}}    & pcrel)                 |
		({32{r_jalr}}   & { alu_add[31:1], 1'b0 }) |
		({32{r_ret}}    & q0)                    |
		({32{r_nt}}     & pc_plus4);

	// X is free for the next instruction at the next edge
	wire x_free = !x_valid || x_retire || irq_take;

	// The same, but with "a load or store has been taken" counting as
	// free: from then on X no longer reads its operand registers (the
	// cache has the address and data, the load formatter has x_lo), so
	// D may write them in every such cycle while it waits. The last
	// write is in the cycle D really moves (d_go), so they end up the
	// same -- but their enable no longer waits for m_ack, which is the
	// critical path's end (tag RAM -> hit -> ack -> d_go -> x_a).
	wire x_free_ab = !x_valid || irq_take || (x_mem && x_issued) ||
	                 (x_go && !x_mis && !x_mem && (x_bad ? x_skip : x_fin_nm)) ||
	                 (x_go && !x_mis && x_mem && x_bad && x_skip);

	// ============================================================
	// D: the queue's head, register read with a bypass from X
	// ============================================================

	wire        d_valid = (iq_cnt != 2'd0);
	wire [31:0] d_instr = iq_rp[1] ? iq_i2 : iq_rp[0] ? iq_i1 : iq_i0;
	wire        d_mis   = iq_rp[1] ? iq_m2 : iq_rp[0] ? iq_m1 : iq_m0;
	wire  [2:0] d_p     = !ENABLE_MPU ? 3'b000 :
	                      iq_rp[1] ? iq_p2 : iq_rp[0] ? iq_p1 : iq_p0;
	wire [31:2] d_ppc   = !ENABLE_MPU ? 30'd0 :
	                      iq_rp[1] ? iq_a2 : iq_rp[0] ? iq_a1 : iq_a0;
	wire  [4:0] d_rs1   = d_instr[19:15];
	wire  [4:0] d_rs2   = d_instr[24:20];
	wire        x_wr    = x_we && (rd_idx != 5'd0);

	// the register file as of the next edge: W writes at this one, so its
	// value is taken directly (a register, not X's result logic)
	wire [31:0] d_a = (d_rs1 == 5'd0) ? 32'd0 :
	                  (w_we && w_rd == d_rs1) ? w_val : regs[d_rs1];
	wire [31:0] d_b = (d_rs2 == 5'd0) ? 32'd0 :
	                  (w_we && w_rd == d_rs2) ? w_val : regs[d_rs2];

	// ...and the instruction leaving X now writes W next: forward it then
	wire d_fa = x_wr && (rd_idx == d_rs1) && (d_rs1 != 5'd0);
	wire d_fb = x_wr && (rd_idx == d_rs2) && (d_rs2 != 5'd0);

	// d_go is the enable of most of X's registers, and m_ack -- late, from
	// the cache's tag compare -- is in it only while X waits for its load
	// or store's answer (x_wack). Then nothing else can hold X (no
	// interrupt is taken once an access is out, no redirect comes from a
	// load or store), so d_go is D's readiness and m_ack. Built as one
	// final select on m_ack, kept apart, so that m_ack passes one LUT
	// before the fan-out rather than the whole retire logic.
	wire d_go_full = d_valid && x_free && !x_redir && !trap;	// reference
	(* keep *) wire x_wack  = x_valid && x_mem && x_issued;
	(* keep *) wire d_go_w  = d_valid && !trap;
	(* keep *) wire d_go_n  = d_valid && !trap && !x_redir &&
		(!x_valid || irq_take ||
		 (x_go && !x_mis && (x_bad ? x_skip : (!x_mem && x_fin_nm))));
	wire d_go = x_wack ? (d_go_w && m_ack) : d_go_n;
	// copies of it for the wide register groups, so that no one net
	// fans out to all of them (the tools do not duplicate it themselves)
	(* keep *) wire d_go_i = x_wack ? (d_go_w && m_ack) : d_go_n;	// x_instr

	// BTFN: D's instruction is a jal or a backward branch, so fetch its
	// target now. Only when X is not waiting for an answer (then d_go is
	// d_go_n, which has no m_ack in it, and the fetch PC's enable stays
	// off the critical path); otherwise X redirects as without BTFN.
	wire        d_isbr  = (d_instr[6:0] == 7'b1100011);
	wire        d_isjal = (d_instr[6:0] == 7'b1101111);
	wire        d_pred  = BTFN && !d_mis && (d_isjal || (d_isbr && d_instr[31]));
	wire [31:0] d_imm   = d_isjal ?
		{ {12{d_instr[31]}}, d_instr[19:12], d_instr[20], d_instr[30:21], 1'b0 } :
		{ {20{d_instr[31]}}, d_instr[7], d_instr[30:25], d_instr[11:8], 1'b0 };
	wire [31:0] d_tgt   = d_pc + d_imm;
	wire        d_red   = BTFN && d_pred && !x_wack && d_go_n;
	(* keep *) wire d_go_p = x_wack ? (d_go_w && m_ack) : d_go_n;	// x_pc, x_ppc
`ifndef SYNTHESIS
	always @(posedge clk)
		if (!rst && (d_go !== d_go_full)) begin
			$display("zeitlos32ps: d_go %b != reference %b at pc %08x", d_go, d_go_full, x_pc);
			$finish;
		end
`endif
	// Nor do they need D to be valid or the instruction to survive: when
	// X is free, whatever is written is either the instruction that moves
	// to X (the last write then is its d_go cycle) or nothing that X will
	// read. So the enable has neither the branch compare (x_redir) nor
	// trap in it.
	wire d_ld_ab = x_free_ab;

	// ============================================================
	// F: fetch
	// ============================================================

	wire        f_mis   = (pc_f[1:0] != 2'b00);
	// room for the answer: the queue plus what is in flight stays at 3,
	// a test on registers alone
	wire        f_room  = ({1'b0, iq_cnt} + {2'b0, f_out}) < 3'd3;
	wire        f_want  = !f_halt && !trap && f_room && !rst;
	wire        f_req   = f_want && !f_mis && !x_dreq;
	// a misaligned target becomes a marker in the queue instead of a
	// fetch, once nothing is outstanding (so it stays in order)
	wire        f_mark  = f_want && f_mis && !f_out;

	wire [31:0] f_va    = { pc_f[31:2], 2'b00 };
	wire [31:0] f_padr  = (mtu_en && f_va[31:28] == 4'h8) ?
		(mtu_base + { 4'h0, f_va[27:0] }) : f_va;

	assign m_priv  = ENABLE_MPU ? x_newpriv : 1'b1;
	assign m_pc    = ENABLE_MPU ? { x_ppc, 2'b00 } : 32'h0;
	assign i_priv  = ENABLE_MPU ? priv_x : 1'b1;

	assign m_req   = x_dreq || f_req;
	assign m_adr   = x_dreq ? x_padr : f_padr;
	assign m_we    = x_dreq && is_store;
	assign m_sel   = (x_dreq && is_store) ? st_sel : 4'b1111;
	assign m_wdat  = st_dat;
	assign m_instr = !x_dreq;

	wire taken   = m_req && m_rdy;
	wire f_taken = taken && !x_dreq;
	wire f_ack   = m_ack && out_fetch;			// a fetch answered
	wire f_push  = (f_ack && !f_kill) || f_mark;
	wire [2:0] f_pin = ENABLE_MPU ? { p_privreg, p_gate, p_own } : 3'b000;

	// ============================================================
	// the clocked part
	// ============================================================

	always @(posedge clk) begin
		irq_pending_n = irq_pending & LATCHED_IRQ;

		if (ENABLE_COUNTERS) count_cycle <= count_cycle + 64'd1;

		if (rst) begin
			pc_f <= PROGADDR_RESET;
			d_pc <= PROGADDR_RESET;
			f_out <= 1'b0;
			f_kill <= 1'b0;
			f_halt <= 1'b0;
			out_fetch <= 1'b0;
			iq_cnt <= 2'd0;
			iq_rp <= 2'd0;
			iq_wp <= 2'd0;
			x_valid <= 1'b0;
			x_first <= 1'b0;
			x_late <= 1'b0;
			x_issued <= 1'b0;
			x_mis <= 1'b0;
			x_pt <= 1'b0;
			trap <= 1'b0;
			eoi <= 32'd0;
			q0 <= 32'd0; q1 <= 32'd0; q2 <= 32'd0; q3 <= 32'd0;
			irq_mask <= ~32'd0;
			irq_pending_n = 32'd0;
			irq_active <= 1'b0;
			irq_delay <= 1'b0;
			timer_q <= 32'd0;
			count_cycle <= 64'd0;
			count_instr <= 64'd0;
			w_we <= 1'b0;
			x_fa <= 1'b0;
			x_fb <= 1'b0;
			priv_x <= 1'b1;            // as wb_mpu: privileged from reset
			lastx_ppc <= 30'h0;
			fv_ev <= 1'b0;
			fv_adr <= 32'h0;
			fv_pc <= 32'h0;
			fv_why <= 4'h0;
		end else begin

			// -- the port --------------------------------------------
			if (taken) out_fetch <= !x_dreq;

			// -- X -----------------------------------------------------
			if (irq_take) begin
				q0 <= x_pc;
				q1 <= irq_delivered;
				eoi <= irq_delivered;
				irq_pending_n = irq_pending_n & irq_mask;
				irq_active <= 1'b1;
			end else if (x_go) begin
				if (x_first) irq_delay <= irq_active;

				if (x_mis) begin
					if (!irq_mask[2] && !irq_active) irq_pending_n[2] = 1'b1;
					else trap <= 1'b1;
				end else if (x_fault1 || x_fault2) begin
					if (!irq_mask[x_fault2 ? 2 : 1] && !irq_active)
						irq_pending_n[x_fault2 ? 2 : 1] = 1'b1;
					else
						trap <= 1'b1;
				end else if (x_cust && x_late) begin
					if (is_setq) begin
						case (rd_idx[1:0])
							2'd0: q0 <= xa;
							2'd1: q1 <= xa;
							2'd2: q2 <= xa;
							default: q3 <= xa;
						endcase
					end
					if (is_retirq) begin
						irq_active <= 1'b0;
						eoi <= 32'd0;
					end
					if (is_maskirq) irq_mask <= xa;
				end

				if (x_dreq && taken) x_issued <= 1'b1;
				// a marker keeps x_first: the interrupt it raised is taken
				// at this same boundary next cycle
				if (!x_retire && !x_mis) begin
					x_first <= 1'b0;
					x_late  <= !x_first;
				end
			end

			// -- the timer ---------------------------------------------
			if (x_go && !x_mis && x_cust && x_late && is_timer) begin
				timer_q <= xa;
			end else if (timer_q != 32'd0) begin
				timer_q <= timer_q - 32'd1;
				if (timer_q == 32'd1) irq_pending_n[0] = 1'b1;
			end

			// -- W: the result, written to the register file next cycle
			// (w_val holds until the next write: an instruction that
			// forwards from it may wait in X for several cycles)
			w_we  <= x_wr;
			if (x_wr) begin
				w_rd  <= rd_idx;
				w_val <= x_wdat;
				w_pc  <= x_pc;
			end

			// -- the MPU's view: report a fetch violation once, in the
			// instruction's first cycle in X (every older load or store
			// has been answered, and so judged, by then)
			fv_ev  <= ENABLE_MPU && fv && x_go && x_first;
			fv_adr <= ENABLE_MPU ? { x_ppc, 2'b00 } : 32'h0;
			fv_pc  <= ENABLE_MPU ? { lastx_ppc, 2'b00 } : 32'h0;
			fv_why <= ENABLE_MPU ? fv_reason : 4'h0;
			if (ENABLE_MPU && x_retire) begin
				priv_x   <= x_newpriv;
				lastx_ppc <= x_ppc;
			end

			// -- retire ------------------------------------------------
			if (x_retire && ENABLE_COUNTERS) count_instr <= count_instr + 64'd1;

			// -- X's operands (see x_free_ab) --------------------------
			if (d_ld_ab) begin
				x_a <= d_a;
				x_b <= d_b;
			end
			if (x_dreq && m_rdy) x_lo <= mem_adr[1:0];

			// -- X takes the next instruction --------------------------
			if (d_go_i) x_instr <= d_instr;
			if (d_go_p) begin
				x_pc     <= d_pc;
				x_ppc    <= d_ppc;
			end
			if (d_go) begin
				x_valid  <= 1'b1;
				x_first  <= 1'b1;
				x_late   <= 1'b0;
				x_issued <= 1'b0;
				x_mis    <= d_mis;
				x_pt     <= d_red;
				x_fa     <= d_fa;
				x_fb     <= d_fb;
				x_preg   <= d_p[2];
				x_pgate  <= d_p[1];
				x_pown   <= d_p[0];
			end else if (x_free) begin
				x_valid  <= 1'b0;
			end

			// -- the queue ---------------------------------------------
			if (x_redir) begin
				iq_cnt <= 2'd0;
				iq_rp  <= 2'd0;
				iq_wp  <= 2'd0;
				d_pc   <= x_target;
			end else if (d_red) begin
				// the head goes to X; the rest is the fall-through path
				iq_cnt <= 2'd0;
				iq_rp  <= 2'd0;
				iq_wp  <= 2'd0;
				d_pc   <= d_tgt;
			end else begin
				if (f_push) begin
					case (iq_wp)
						2'd0: begin iq_i0 <= m_rdat; iq_m0 <= f_mark; iq_p0 <= f_pin; iq_a0 <= f_pq; end
						2'd1: begin iq_i1 <= m_rdat; iq_m1 <= f_mark; iq_p1 <= f_pin; iq_a1 <= f_pq; end
						default: begin iq_i2 <= m_rdat; iq_m2 <= f_mark; iq_p2 <= f_pin; iq_a2 <= f_pq; end
					endcase
					iq_wp <= (iq_wp == 2'd2) ? 2'd0 : iq_wp + 2'd1;
				end
				if (d_go) begin
					iq_rp <= (iq_rp == 2'd2) ? 2'd0 : iq_rp + 2'd1;
					d_pc  <= d_pc + 32'd4;
				end
				iq_cnt <= iq_cnt + {1'b0, f_push} - {1'b0, d_go};
			end

			// -- fetch -------------------------------------------------
			if (x_redir) begin
				pc_f   <= x_target;
				f_halt <= 1'b0;
				// whatever is still to come back is from the old path
				f_kill <= (f_out && !f_ack) || f_taken;
			end else if (d_red) begin
				pc_f   <= d_tgt;
				f_halt <= 1'b0;
				f_kill <= (f_out && !f_ack) || f_taken;
			end else begin
				if (f_taken) pc_f <= pc_f + 32'd4;
				if (f_mark) f_halt <= 1'b1;
				if (f_ack) f_kill <= 1'b0;
			end
			if (f_taken && ENABLE_MPU) f_pq <= f_padr[31:2];
			if (f_taken)    f_out <= 1'b1;
			else if (f_ack) f_out <= 1'b0;
		end

		irq_pending_n = irq_pending_n | irq;
		irq_pending <= irq_pending_n & ~MASKED_IRQ;
	end

	// the register file's one write port: W's write, or x2 at reset
	always @(posedge clk) begin
		if (rst)
			regs[2] <= STACKADDR;
		else if (w_we)
			regs[w_rd] <= w_val;
	end

	// for the testbenches: the write W makes this cycle
	wire w_wr = !rst && w_we;

endmodule
