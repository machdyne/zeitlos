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
 * zeitlos32p -- the pipelined zeitlos32.
 *
 * The same machine as rtl/cpu/zeitlos32 -- RV32IM, the four counter
 * reads, FENCE as a nop, picorv32's custom interrupt instructions and
 * their exact semantics -- so the BIOS, the kernel and every binary
 * run on it unchanged. What differs is how instructions are sequenced:
 * five stages, one instruction a clock when nothing stands in the way,
 * against zeitlos32's five clocks per instruction at best. See
 * docs/zeitlos32p.md.
 *
 * It is built from zeitlos32's own combinational units
 * (zeitlos32_decode, zeitlos32_alu, zeitlos32_store, zeitlos32_load,
 * zeitlos32_muldiv) and talks to rtl/cache_split.v's two ports, not to
 * a Wishbone bus: a pipeline needs a fetch and a load or store in
 * flight at once.
 *
 * -- Stages --
 *
 *   F  fetch: pc_f, translated by the MTU, to the I port. A request
 *      goes out only while the fetch queue has room for its answer, so
 *      i_req depends on registers alone and never on what the stages
 *      behind are doing this cycle.
 *   D  the instruction at the fetch queue's head (three entries);
 *      register file read, with a bypass from W.
 *   X  decode, forwarding, the ALU, branches and jumps resolved, the
 *      address of a load or store computed and sent to the D port,
 *      interrupts taken, the custom instructions, multiply/divide.
 *      Everything an instruction does to the machine besides its
 *      register write and its load or store is done here, in order,
 *      and nothing behind X can cancel it -- so an instruction that
 *      leaves X has executed, or (a load or store) is certain to.
 *   M  two entries, M and S behind it, in order: M waits for the D
 *      port's answer to a load or store; a load's byte or halfword
 *      picked out. See "The memory stage" below.
 *   W  the register write: the register file's one write port.
 *
 * -- The memory stage --
 *
 * The D port answers a hit from its tag RAM, late in the cycle. If X
 * could move on only when M was sure to empty, that answer would gate
 * every enable in X, D and the fetch queue -- far too long a path for
 * 48 MHz. So X never looks at the D port's answer at all: it moves on
 * whenever S is empty, a registered fact. What it hands over goes to M
 * if M empties this cycle (or is empty), else to S.
 *
 * A load or store is sent to the D port straight from X when nothing
 * older is still waiting to be sent; if the port does not take it
 * (busy with a miss), it is sent again from its M or S entry, which
 * keeps the physical address, the store data and the byte enables.
 * Requests go out in program order and the port has one outstanding
 * at a time, so every answer is M's: S is only ever filled behind a
 * load or store in M that is waiting, S's own request can be taken at
 * the earliest in the cycle M's is answered, and in that cycle S moves
 * up. A run of hits still goes at one a clock: one taken, answered,
 * retired every cycle.
 *
 * -- What costs cycles --
 *
 *   - a taken branch or a jal the BTB predicted: 0; a mispredicted
 *     branch or jal, a jalr, retirq: 3 (resolved in X; F, the queue
 *     and D refilled). See "F: the BTB".
 *   - a load followed by an instruction that uses its result: 1 (the
 *     value is forwarded from W, never from M: a load's data arrives
 *     from the cache's block RAM late in M, too late to go through
 *     the ALU in the same cycle)
 *   - a cache miss, an uncached access, the write buffer full: the
 *     time it takes; F and M wait independently, so a fetch miss does
 *     not hold up a load and the reverse. Behind a load or store that
 *     waits, one more instruction moves on into S; the next waits,
 *     and so does one that needs S's result
 *   - multiply 3 with FAST_MUL, 34 without; divide 35 (zeitlos32_muldiv)
 *
 * -- Interrupts and faults: zeitlos32's semantics, in X --
 *
 * An interrupt is taken from X, at the boundary before the instruction
 * there: q0 gets its address (the instruction that did NOT run), the
 * instruction and everything behind it is discarded, fetching starts
 * again at PROGADDR_IRQ. Instructions ahead of it, in M and W, are
 * older and finish. Exactly as zeitlos32:
 *
 *   - not while a handler runs (irq_active), not for the one
 *     instruction after retirq (irq_delay: set when retirq leaves X,
 *     cleared when the next instruction does);
 *   - not once a multiply/divide or a waitirq has started (zeitlos32
 *     decides only at its fetch, before either begins);
 *   - an illegal instruction, ecall/ebreak or a misaligned load or
 *     store sets IRQ 1 or 2 pending and is skipped (counted as
 *     retired, no effect); a misaligned fetch sets IRQ 2 and is taken
 *     with q0 = that address; any of them with the IRQ masked or
 *     inside a handler halts the core (trap).
 *
 * The MPU: rtl/mpu_split.v, with ENABLE_MPU. rtl/mpu.v takes the
 * privilege of a load or store from the most recent fetch, which here
 * is several instructions ahead (and may be on a path that is thrown
 * away), so this core tracks it per instruction instead. See
 * docs/zeitlos32p.md, "The MPU".
 */

module zeitlos32p #(
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
	// 1: track each instruction's privilege for rtl/mpu_split.v (the
	// mpu_*, chk_*, fv_*, d_priv, d_pc, i_priv ports); 0: no MPU, and
	// all of it is constant and synthesises away
	parameter [0:0]  ENABLE_MPU     = 0,
	// branch prediction: a branch target buffer of 2**BTB_BITS entries
	// in F (0: none, and a taken branch or a jump always costs 3)
	parameter        BTB_BITS       = 6
) (
	input clk,
	input rst,

	// -- the I port (rtl/cache_split.v): fetches, physical address --
	output        i_req,
	output [31:0] i_adr,
	input         i_rdy,
	input         i_ack,
	input  [31:0] i_dat,

	// -- the D port: loads and stores, physical address --
	output        d_req,
	output [31:0] d_adr,
	output        d_we,
	output  [3:0] d_sel,
	output [31:0] d_wdat,
	input         d_rdy,
	input         d_ack,
	input  [31:0] d_rdat,

	// MTU: physical base for the 0x8000_0000 window, as zeitlos32
	input  [31:0] mtu_base,

	// -- the MPU (rtl/mpu_split.v), with ENABLE_MPU --
	input         mpu_en,         // its CTRL enable and enforce bits
	input         mpu_enforce,
	output [31:0] chk_pc,         // D's instruction address (virtual)...
	input         chk_privreg,    // ...fetching it would make code privileged
	input         chk_gate,       // ...it is the gate or the IRQ vector
	input         chk_own,        // ...it is inside the app's block
	output        d_priv,         // the D port request's instruction is privileged
	output [31:0] d_pc,           // ...and its address (virtual)
	output        i_priv,         // the last instruction executed was privileged
	output reg        fv_ev,      // an instruction fetch violated (it left X)
	output reg [31:0] fv_adr,     // ...its address (virtual)
	output reg [31:0] fv_pc,      // ...the instruction executed before it
	output reg  [3:0] fv_why,     // ...wb_mpu's reason: 1 or 4

	input      [31:0] irq,
	output reg [31:0] eoi,
	output reg        trap
);

	integer i;

	// ============================================================
	// state
	// ============================================================

	reg run;                       // cleared for good by a trap

	// -- F --
	reg [31:0] pc_f;               // the next address to fetch (virtual)
	reg        f_out;              // a fetch is outstanding
	reg        f_kill;             // ...and its answer is to be dropped
	reg [31:0] f_pc;               // its address
	reg        f_faulted;          // a misaligned fetch is in the queue

	// the fetch queue: entry 0 is the head
	reg  [1:0] fq_cnt;
	reg [31:0] fq_i0, fq_i1, fq_i2;
	reg [31:0] fq_p0, fq_p1, fq_p2;
	reg        fq_f0, fq_f1, fq_f2;   // a misaligned-fetch entry

	// what the BTB said about each fetch (see "F: the BTB")
	reg        f_bh, f_bt;         // the fetch in flight: hit; predicted taken
	reg  [1:0] f_bc;               // ...its counter
	reg [29:0] f_bg;               // ...the target it went on to
	reg        fq_h0, fq_h1, fq_h2;
	reg        fq_t0, fq_t1, fq_t2;
	reg  [1:0] fq_c0, fq_c1, fq_c2;
	reg [29:0] fq_g0, fq_g1, fq_g2;

	// -- X --
	reg        ex_valid;
	reg [31:0] ex_instr;
	reg [31:0] ex_pc;
	reg        ex_ff;              // misaligned fetch: not an instruction
	reg [31:0] ex_r1;              // operands as captured
	reg [31:0] ex_r2;
	reg        ex_started;         // a multiply/divide or waitirq is under way,
	                               // or a jump has already redirected
	reg        ex_md_have;         // ...the multiply/divide has answered
	reg [31:0] ex_md_res;
	reg        ex_bh;              // the BTB hit on its fetch...
	reg  [1:0] ex_bc;              // ...with this counter
	reg        ex_jdone;           // X has redirected for it already
	reg        jr_v;               // a jump's redirect, from X last cycle
	reg [31:0] jr_pc;              // ...and where to
	reg        ex_pt;              // fetching went on at a predicted target
	reg        ex_ptok;            // ...and D found that target right for a jal/branch
	reg        ex_preg;            // the MPU's checks on its address (chk_*)
	reg        ex_pgate;
	reg        ex_pown;

	// privilege, in program order: of the last instruction to leave X
	reg        priv_x;
	reg [31:0] lastx_pc;           // ...and its address

	// -- M, and S behind it: two slots, a circular buffer --
	//
	// q_rp names the slot that is M (the oldest), q_wp the slot X
	// writes next. A slot's registers are written only by X, into a
	// slot empty at the start of the cycle, so their enables never see
	// the D port's (late) answer; M retiring only clears a valid bit
	// and moves q_rp.
	reg  [1:0] q_v;
	reg        q_rp, q_wp;
	reg        q0_dop, q1_dop;     // a load or store: waits for the D port
	reg        q0_load, q1_load;
	reg        q0_we, q1_we;       // writes rd
	reg  [4:0] q0_rd, q1_rd;
	reg [31:0] q0_val, q1_val;     // the result; a store's data
	reg  [2:0] q0_f3, q1_f3;
	reg  [1:0] q0_lo, q1_lo;
	reg [31:0] q0_pc, q1_pc;
	reg        q0_iss, q1_iss;     // the D port has taken it
	reg [31:0] q0_adr, q1_adr;     // physical, word-aligned
	reg  [3:0] q0_sel, q1_sel;
	reg        q0_wr, q1_wr;       // a store
	reg        q0_priv, q1_priv;   // its instruction's privilege

	// M, the oldest slot
	wire        m_valid = q_rp ? q_v[1]  : q_v[0];
	wire        m_dop   = q_rp ? q1_dop  : q0_dop;
	wire        m_load  = q_rp ? q1_load : q0_load;
	wire        m_we    = q_rp ? q1_we   : q0_we;
	wire  [4:0] m_rd    = q_rp ? q1_rd   : q0_rd;
	wire [31:0] m_val   = q_rp ? q1_val  : q0_val;
	wire  [2:0] m_f3    = q_rp ? q1_f3   : q0_f3;
	wire  [1:0] m_lo    = q_rp ? q1_lo   : q0_lo;
	wire [31:0] m_pc    = q_rp ? q1_pc   : q0_pc;
	wire        m_iss   = q_rp ? q1_iss  : q0_iss;

	// S, the other one (occupied only behind an occupied M)
	wire        s_valid = q_rp ? q_v[0]  : q_v[1];

	// -- W --
	reg        w_valid;
	reg        w_we;
	reg  [4:0] w_rd;
	reg [31:0] w_val;
	reg [31:0] w_pc;

	reg [31:0] regs [0:31];

	reg [31:0] q0, q1, q2, q3;
	reg [31:0] irq_mask;
	reg [31:0] irq_pending;
	reg [31:0] irq_pending_n;      // blocking, as zeitlos32
	reg        irq_active;
	reg        irq_delay;

	reg [63:0] count_cycle;
	reg [63:0] count_instr;

	// picorv32's IRQ timer (ENABLE_IRQ_TIMER, which rtl/sysctl.v sets
	// for picorv32 and the kernel relies on: timer(1) is how a process
	// that blocks yields at once, sw/os/kernel.c). `timer rd, rs`
	// returns the counter in rd and loads it from rs; it counts down
	// one a clock, raising IRQ 0 as it goes from 1 to 0; 0 is off.
	reg [31:0] timer_q;

	initial begin
		if (REGS_INIT_ZERO) begin
			for (i = 0; i < 32; i = i + 1) regs[i] = 32'd0;
		end
	end

	// ============================================================
	// MTU translation: physical = base + offset in the 0x8 window
	// ============================================================

	wire        mtu_en = (mtu_base != 32'h0);

	wire [31:0] fetch_va = { pc_f[31:2], 2'b00 };
	assign i_adr = (mtu_en && fetch_va[31:28] == 4'h8) ?
		(mtu_base + { 4'h0, fetch_va[27:0] }) : fetch_va;

	// ============================================================
	// F
	// ============================================================

	// Room for one more answer: the queue plus what is in flight must
	// stay within its three entries. (Three, not two: every answer goes
	// into the queue, so a stream of hits has one in the queue, one in
	// flight and one being asked for, all at once.)
	wire f_room = (fq_cnt == 2'd0) || (fq_cnt == 2'd1) ||
	              ((fq_cnt == 2'd2) && !f_out);
	wire f_mis  = (pc_f[1:0] != 2'b00);

	assign i_req = run && f_room && !f_mis;
	wire   i_take = i_req && i_rdy;

	// A misaligned pc_f is not fetched: a marker goes into the queue in
	// its place, once, and X turns it into IRQ 2 or a trap when it gets
	// there -- in order, behind everything fetched before it.
	wire f_fpush = run && f_room && f_mis && !f_out && !f_faulted;

	wire        arrive   = (i_ack && f_out && !f_kill) || f_fpush;
	wire [31:0] arr_i    = f_fpush ? 32'h0000_0013 : i_dat;
	wire [31:0] arr_p    = f_fpush ? pc_f : f_pc;
	wire        arr_f    = f_fpush;
	wire        arr_t    = !f_fpush && f_bt;

	// ============================================================
	// F: the BTB
	//
	// Direct-mapped, indexed by the fetch address, each entry a short
	// tag, a target and a two-bit counter, for conditional branches
	// and jal only. When a fetch goes out and its entry matches with
	// the counter's upper bit set, the next fetch is from the target
	// instead of pc+4 -- the register pc_f, the table (distributed
	// RAM, read without a clock) and a mux into pc_f's input: nothing
	// on the I port's address path.
	//
	// Nothing the table says is trusted. D checks it against the
	// instruction that really arrived (a jal or branch whose pc+imm is
	// that target); X goes back to pc+4, or to the real target, when
	// the prediction does not hold -- the redirect it makes today for
	// every taken branch. So a stale entry (another program loaded at
	// the same address, another MTU base, an alias) costs only time.
	// X writes the outcome back a cycle after the instruction leaves.
	// ============================================================

	localparam BTB_N = (BTB_BITS > 0) ? (1 << BTB_BITS) : 1;
	localparam BTB_IB = (BTB_BITS > 0) ? BTB_BITS : 1;

	// distributed RAM, read without a clock: as block RAM its address
	// would be pc_f's next value, and the I port's answer with it
	(* ram_style = "distributed" *) reg  [7:0] btb_tag [0:BTB_N-1];
	(* ram_style = "distributed" *) reg [29:0] btb_tgt [0:BTB_N-1];
	(* ram_style = "distributed" *) reg  [1:0] btb_ctr [0:BTB_N-1];

	initial begin
		for (i = 0; i < BTB_N; i = i + 1) begin
			btb_tag[i] = 8'h00;
			btb_tgt[i] = 30'h0;
			btb_ctr[i] = 2'b00;
		end
	end

	wire [BTB_IB-1:0] bf_idx = pc_f[BTB_IB+1:2];
	wire  [7:0] bf_tag = pc_f[BTB_IB+9:BTB_IB+2];
	wire  [7:0] bf_etag = btb_tag[bf_idx];
	wire [29:0] bf_etgt = btb_tgt[bf_idx];
	wire  [1:0] bf_ectr = btb_ctr[bf_idx];
	wire        bf_hit = (BTB_BITS > 0) && (bf_etag == bf_tag);
	wire        bf_take = bf_hit && bf_ectr[1];

	// the write, registered (X's outcome, a cycle late)
	reg              bw_v;
	reg [BTB_IB-1:0] bw_idx;
	reg  [7:0]       bw_tag;
	reg [29:0]       bw_tgt;
	reg  [1:0]       bw_ctr;

	always @(posedge clk) begin
		if (bw_v && BTB_BITS > 0) begin
			btb_tag[bw_idx] <= bw_tag;
			btb_tgt[bw_idx] <= bw_tgt;
			btb_ctr[bw_idx] <= bw_ctr;
		end
	end

	// ============================================================
	// D
	// ============================================================

	// Always from the queue's head, never straight from the I port: the
	// answer comes out of the cache's block RAM late in the cycle, too
	// late to index the register file with and still capture what it
	// reads. It costs a cycle each time fetching starts again (a taken
	// branch, a jump, a miss) and nothing in a straight run.
	wire        d_have  = (fq_cnt != 2'd0);
	wire [31:0] d_ins   = fq_i0;
	wire [31:0] d_ipc   = fq_p0;
	wire        d_ff    = fq_f0;
	wire  [4:0] d_rs1   = d_ins[19:15];
	wire  [4:0] d_rs2   = d_ins[24:20];

	// the BTB's prediction for D's instruction, checked: a jal or a
	// branch, and pc + its offset is where fetching went on
	wire        d_isjal = (d_ins[6:0] == 7'b1101111);
	wire        d_isbr  = (d_ins[6:0] == 7'b1100011);
	wire [31:0] d_immj  = { {12{d_ins[31]}}, d_ins[19:12], d_ins[20], d_ins[30:21], 1'b0 };
	wire [31:0] d_immb  = { {20{d_ins[31]}}, d_ins[7], d_ins[30:25], d_ins[11:8], 1'b0 };
	wire [31:0] d_btgt  = d_ipc + (d_isjal ? d_immj : d_immb);
	wire        d_ptok  = fq_t0 && !d_ff && (d_isjal || d_isbr) &&
	                      (d_btgt[1:0] == 2'b00) && (d_btgt[31:2] == fq_g0);

	// register file read, with the value W writes this very cycle
	wire w_wr = w_valid && w_we && (w_rd != 5'd0);
	wire [31:0] d_r1 = (d_rs1 == 5'd0) ? 32'd0 :
		(w_wr && w_rd == d_rs1) ? w_val : regs[d_rs1];
	wire [31:0] d_r2 = (d_rs2 == 5'd0) ? 32'd0 :
		(w_wr && w_rd == d_rs2) ? w_val : regs[d_rs2];

	// ============================================================
	// X: decode
	// ============================================================

	wire [6:0] opcode;
	wire [4:0] rd_idx;
	wire [2:0] funct3;
	wire [4:0] rs1_idx;
	wire [4:0] rs2_idx;
	wire [6:0] funct7;
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
		.instr(ex_instr),
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

	// which registers the instruction reads (so a load ahead of it
	// holds it up only when it has to)
	wire uses_rs1 = !(is_lui || is_auipc || is_jal);
	wire uses_rs2 = is_branch || is_store || is_op;

	// ============================================================
	// X: forwarding
	//
	// Newest first: lx, the result X produced last cycle (never a
	// load's: see the header), then W, then the value captured in D
	// (refreshed below every cycle X waits, so a value forwarded while
	// it waits is not lost when its producer retires).
	//
	// lx is a copy of what went into M or S, kept in registers of its
	// own written every cycle, so the forwarding selects are plain
	// compares of registers -- not of whichever slot q_rp names. Only
	// the newest result needs it: in a straight run that is the one in
	// M, which retires this cycle and is in W next. Anything older
	// still in a slot (behind a load or store waiting for the D port)
	// or a load anywhere in the slots makes X wait, unless lx -- newer
	// than all of them -- has the register.
	// ============================================================

	reg        lx_valid;
	reg  [4:0] lx_rd;
	reg [31:0] lx_val;

	wire lx1 = lx_valid && (lx_rd == rs1_idx);
	wire lx2 = lx_valid && (lx_rd == rs2_idx);

	wire [31:0] xa = lx1 ? lx_val : (w_wr && w_rd == rs1_idx) ? w_val : ex_r1;
	wire [31:0] xb = lx2 ? lx_val : (w_wr && w_rd == rs2_idx) ? w_val : ex_r2;

	// a slot that will write the register, by slot
	wire q0w = q_v[0] && q0_we && (q0_rd != 5'd0);
	wire q1w = q_v[1] && q1_we && (q1_rd != 5'd0);
	wire haz1 = uses_rs1 && !lx1 &&
		((q0w && q0_rd == rs1_idx) || (q1w && q1_rd == rs1_idx));
	wire haz2 = uses_rs2 && !lx2 &&
		((q0w && q0_rd == rs2_idx) || (q1w && q1_rd == rs2_idx));
	wire x_haz = haz1 || haz2;

	// ============================================================
	// X: ALU, addresses
	// ============================================================

	wire [31:0] alu_b = (is_op || is_branch) ? xb : is_store ? imm_s : imm_i;
	wire  [4:0] shamt = is_op ? xb[4:0] : ex_instr[24:20];

	wire [31:0] alu_out;
	wire [31:0] alu_add;
	wire        br_taken;

	zeitlos32_alu alu_i (
		.a(xa),
		.b(alu_b),
		.shamt(shamt),
		.funct3(funct3),
		.sub(is_op && funct7[5]),
		.sra(funct7[5]),
		.out(alu_out),
		.add(alu_add),
		.taken(br_taken)
	);

	wire [31:0] pc_plus4 = ex_pc + 32'd4;
	wire [31:0] mem_adr  = alu_add;

	wire mem_misaligned =
		(funct3[1:0] == 2'b10) ? (mem_adr[1:0] != 2'b00) :
		(funct3[1:0] == 2'b01) ? (mem_adr[0]   != 1'b0)  : 1'b0;

	// The physical address. In the 0x8 window it is mtu_base plus the
	// offset; with va[31:28] == 8 that is va + (mtu_base - 0x8000_0000),
	// and subtracting 0x8000_0000 is flipping bit 31. So the translated
	// address is a three-input sum, rs1 + imm + that, formed carry-save
	// alongside the plain one, and the window test only picks between
	// them: one adder deep, not two. (Bits 1:0 of mtu_base are left out
	// of the sum and put back after, as the two-adder form never
	// carried out of them.)
	wire [31:0] data_va = { mem_adr[31:2], 2'b00 };
	wire [31:0] mtu_off = { ~mtu_base[31], mtu_base[30:2], 2'b00 };
	wire [31:0] csa_s   = xa ^ alu_b ^ mtu_off;
	wire [31:0] csa_c   = { (xa[30:0] & alu_b[30:0]) | (xa[30:0] & mtu_off[30:0]) |
	                        (alu_b[30:0] & mtu_off[30:0]), 1'b0 };
	wire [31:0] mtu_sum = csa_s + csa_c;
	wire [31:0] x_padr = (mtu_en && data_va[31:28] == 4'h8) ?
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

	wire [3:0] x_sel = is_store ? st_sel : 4'b1111;

	// ============================================================
	// X: multiply / divide (operands taken on the start pulse)
	// ============================================================

	wire        md_ready;
	wire [31:0] md_result;
	wire        md_start;

	zeitlos32_muldiv #(
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
	// X: what happens to the instruction there this cycle
	// ============================================================

	wire [31:0] irq_delivered = irq_pending & ~irq_mask;
	wire        irq_take = (|irq_delivered) && !irq_active && !irq_delay;

	// An interrupt is taken before X's instruction, unless it has begun
	// (a multiply/divide or a waitirq under way, or a jump that has
	// already redirected fetching).
	wire x_irq = run && ex_valid && irq_take && !ex_started;

	wire x_live = run && ex_valid && !x_irq;

	// the faults: skipped with IRQ 1 or 2 raised, or a trap
	// -- the MPU's fetch rule, judged here in program order (wb_mpu
	//    judges it at the fetch, which on a single-issue core is the
	//    same order). The instruction before this one decides: from
	//    unprivileged code, privileged code may be entered only at the
	//    gate or the IRQ vector, and anything else fetched must be in
	//    the app's block. With ENFORCE a violating instruction is not
	//    run -- it is an illegal instruction, as the 0x0000_0000 wb_mpu
	//    hands back -- and the privilege stays as it was. fv waits, as
	//    the custom ops do, until every older load or store has been
	//    taken (and so judged): its report must come after theirs.
	wire       mpu_on = ENABLE_MPU && mpu_en;
	wire [3:0] fv_reason = ex_preg ? (ex_pgate ? 4'd0 : 4'd4) :
	                                 (ex_pown  ? 4'd0 : 4'd1);
	wire fv     = mpu_on && run && ex_valid && !ex_ff && !priv_x &&
	              (fv_reason != 4'd0);
	wire x_fblk = fv && mpu_enforce;
	wire x_fwait = fv && (q_v != 2'b00);
	// what this instruction's privilege is, for its own load or store
	// and for the next instruction
	wire x_newpriv = !mpu_on ? 1'b1 : (x_fblk ? priv_x : ex_preg);

	// zeitlos32_decode (shared with zeitlos32, which has no timer)
	// calls it illegal; here it is not
	wire is_timer = is_custom && (funct7 == 7'b0000101) && (funct3 == 3'b110);
	wire x_ill  = !(is_legal || is_timer) || is_ecall_ebreak || x_fblk;
	wire x_mis  = (is_load || is_store) && mem_misaligned;
	wire x_fault_bit2 = ex_ff || (!x_ill && x_mis);
	wire x_fault_ok = x_fault_bit2 ? (!irq_mask[2] && !irq_active)
	                               : (!irq_mask[1] && !irq_active);
	wire x_ffpend = x_live && ex_ff && x_fault_ok;   // IRQ 2 raised, waits
	// (A misaligned load or store is known only once its operands are:
	// waiting on a load for its base register (x_haz), the address X
	// sees is the register's OLD value. So that case waits, here as in
	// x_skip below; the others do not depend on operands.)
	wire x_trap  = x_live && !x_fault_ok && !x_fwait &&
		(x_ill || ex_ff || (x_mis && !x_haz));

	// a normal instruction: x_okn from the decode alone, x_ok also
	// knowing the address is aligned (which needs the address adder)
	wire x_okn   = x_live && !ex_ff && !x_ill;
	wire x_ok    = x_okn && !x_mis;
	wire x_dop   = is_load || is_store;

	// M retires to W this cycle. d_ack is late in the cycle, so it
	// reaches only M's and S's own registers and W's, never X.
	wire m_go   = m_valid && (!m_dop || (m_iss && d_ack));

	assign md_start = x_okn && is_muldiv && !ex_started && !x_haz;
	wire md_done = md_ready || ex_md_have;

	wire x_wait_irq = is_waitirq && !(|irq_pending);

	// The custom instructions read and change the interrupt state, and
	// a store just before one may be what changes it (a device told to
	// raise or drop its line). On zeitlos32 that store has long finished
	// by the time the next instruction runs; here it may still be in M.
	// So they wait until M and W are empty: in order with everything
	// before them, as zeitlos32. And for one cycle more (x_quiet: M and
	// W were empty last cycle too), so that what the store did to the
	// interrupt lines has reached irq_pending -- including a one-cycle
	// pulse on a line that is not latched having come AND GONE, which
	// tests/t_irq.S checks (630). They are rare -- interrupt entry and
	// exit -- so this costs nothing measurable.
	reg x_quiet;
	wire x_serial = is_custom && (m_valid || s_valid || w_valid || !x_quiet);

	// X moves on when S is empty: registered, whatever the D port does.
	//
	// x_go is "X is finished with its instruction" without asking
	// whether a load or store is misaligned: a misaligned one is
	// skipped (or traps) under exactly the conditions an aligned one
	// moves on under. So X emptying (x_free, and from it D, the fetch
	// queue and the slot enables) never waits for the address adder;
	// only what happens to the instruction -- M slot valid, or IRQ 2
	// raised -- does.
	// A store to the 0x9 nibble -- the MTU's base, the MPU's registers
	// -- changes how every later access is translated and judged. On
	// zeitlos32 it has finished before the next instruction starts;
	// here it may still be in a slot on its way to the bus while X
	// translates the next address with the old base (the kernel
	// switches reg_mtu and may touch the app window at once). So X
	// waits while one is outstanding. They are rare (context switches).
	wire x_mtuw = (q_v[0] && q0_wr && (q0_adr[31:28] == 4'h9)) ||
	              (q_v[1] && q1_wr && (q1_adr[31:28] == 4'h9));

	wire x_go  = x_okn && !s_valid && !x_haz && !x_serial && !x_fwait && !x_mtuw &&
		(x_dop ? 1'b1 :
		 is_muldiv ? md_done :
		 !x_wait_irq);
	wire x_adv = x_go && !x_mis;
	wire x_skip = x_live && !ex_ff && x_fault_ok &&    // retired, no effect
		((x_ill && !x_fwait) || (x_mis && x_go));

	// The D port request: the oldest load or store not yet taken --
	// M's, S's, or X's own (which then also moves on this cycle).
	// (by slot: a slot that needs sending goes first if it is M, or
	// if the other does not need sending)
	wire need0 = q_v[0] && q0_dop && !q0_iss;
	wire need1 = q_v[1] && q1_dop && !q1_iss;
	wire iss_q0 = need0 && (!q_rp || !need1);
	wire iss_q1 = need1 && ( q_rp || !need0);
	wire iss_m = q_rp ? iss_q1 : iss_q0;     // (names for the benches)
	wire iss_s = q_rp ? iss_q0 : iss_q1;
	// X's own only when X moves on this cycle (x_adv: registered
	// facts and the decode, not the D port's answer) -- never a request
	// for an instruction that then stays, and would send it again
	wire iss_x = !need0 && !need1 && x_adv && x_dop;

	assign d_req  = need0 || need1 || iss_x;
	assign d_adr  = iss_q0 ? q0_adr : iss_q1 ? q1_adr : x_padr;
	assign d_we   = iss_q0 ? q0_wr  : iss_q1 ? q1_wr  : is_store;
	assign d_sel  = iss_q0 ? q0_sel : iss_q1 ? q1_sel : x_sel;
	assign d_wdat = iss_q0 ? q0_val : iss_q1 ? q1_val : st_dat;
	// (constant without ENABLE_MPU, so none of it is built)
	assign d_priv = !ENABLE_MPU ? 1'b1 :
		iss_q0 ? q0_priv : iss_q1 ? q1_priv : x_newpriv;
	assign d_pc   = !ENABLE_MPU ? 32'h0 :
		iss_q0 ? q0_pc : iss_q1 ? q1_pc : ex_pc;

	assign chk_pc = ENABLE_MPU ? fq_p0 : 32'h0;
	assign i_priv = ENABLE_MPU ? priv_x : 1'b1;

	wire x_taken = iss_x && d_rdy;     // X's own, taken now

	// X empties this cycle. (In a trap's cycle too, which does not
	// matter: run drops and nothing happens again.)
	wire x_free = !ex_valid || x_irq || (x_live && !ex_ff && x_ill && !x_fwait) || x_go;

	// D moves to X
	wire d_go = d_have && x_free && run;

	// the result X hands M
	reg [31:0] x_res;
	reg        x_we;

	always @* begin
		x_we = 1'b0;
		x_res = alu_out;
		case (opcode[6:2])
			5'b01101: begin x_we = 1'b1; x_res = imm_u; end            // lui
			5'b00101: begin x_we = 1'b1; x_res = ex_pc + imm_u; end    // auipc
			5'b11011, 5'b11001: begin x_we = 1'b1; x_res = pc_plus4; end // jal, jalr
			5'b00000: x_we = 1'b1;                                     // load (M fills it)
			5'b00100, 5'b01100: begin
				x_we = 1'b1;
				x_res = is_muldiv ? (ex_md_have ? ex_md_res : md_result) : alu_out;
			end
			5'b11100: begin                                            // counters
				x_we = 1'b1;
				case ({ ex_instr[27], ex_instr[21] })
					2'b00: x_res = count_cycle[31:0];
					2'b01: x_res = count_instr[31:0];
					2'b10: x_res = count_cycle[63:32];
					default: x_res = count_instr[63:32];
				endcase
			end
			5'b00010: begin                                            // custom-0
				case (funct7[2:0])
					3'b000: begin
						x_we = 1'b1;
						case (rs1_idx[1:0])
							2'd0: x_res = q0;
							2'd1: x_res = q1;
							2'd2: x_res = q2;
							default: x_res = q3;
						endcase
					end
					3'b011: begin x_we = 1'b1; x_res = irq_mask; end   // maskirq
					3'b100: begin x_we = 1'b1; x_res = irq_pending; end // waitirq
					3'b101: begin x_we = 1'b1; x_res = timer_q; end     // timer
					default: ;
				endcase
			end
			default: ;
		endcase
	end

	// where X sends fetching, when its instruction changes the flow.
	// Not gated by x_adv: a jump redirects as soon as its operands are
	// ready, even while M still holds a load or store waiting for its
	// ack, and remembers it has (ex_started) so as not to do it twice.
	// That keeps the D port's ack -- tag RAM, compare, ack -- out of the
	// fetch side's enables, which was the critical path. The jump
	// itself still moves on to M (its link register) with x_adv.
	//
	// With the BTB: a jal or taken branch whose predicted target D
	// found right (ex_ptok) does not redirect -- fetching is already
	// there. Anything else fetched past at a predicted target (a branch
	// not taken, a wrong target, not a branch at all) goes back: to the
	// real target if it is taken, to pc+4 if not. An instruction that is
	// skipped (illegal, misaligned) does the same with x_pfix.
	wire x_ctl  = is_jal || (is_branch && br_taken);
	wire x_jump = x_okn && !x_haz && !x_serial && !x_mtuw && !ex_jdone &&
		(is_jalr || is_retirq || (x_ctl ? !ex_ptok : ex_pt));
	wire x_pfix = x_skip && ex_pt;
	wire x_tgo  = is_jalr || is_retirq || x_ctl;   // to x_target, not pc+4
	wire [31:0] x_target =
		is_jal    ? (ex_pc + imm_j) :
		is_jalr   ? { alu_add[31:1], 1'b0 } :
		is_retirq ? q0 :
		            (ex_pc + imm_b);

	// A jump's redirect goes out a cycle after X decides it (jr_v,
	// jr_pc): the branch compare -- forwarding, the comparator's carry,
	// the prediction checks -- then ends in two registers rather than
	// in pc_f and everything a redirect touches, which was sys_clk's
	// critical path at 48 MHz (49.6 MHz on the third seed, the others
	// short). In that cycle X takes nothing from D (all of it is the
	// old path), so nothing wrong-path runs. A jump that has to wait in
	// X (M busy) has its redirect gone out already (ex_jdone), as
	// before. It costs a cycle a redirecting jump: mispredicted
	// branches, jalr, returns. Interrupts and skipped instructions
	// (x_pfix) still redirect at once: neither waits on the compare.
	wire redirect = x_irq || jr_v || x_pfix;
	wire [31:0] redirect_pc = x_irq ? PROGADDR_IRQ : jr_v ? jr_pc : pc_plus4;

	// ============================================================
	// M: a load's word, picked out
	// ============================================================

	wire [31:0] ld_out;

	zeitlos32_load load_i (
		.funct3(m_f3),
		.adr_lo(m_lo),
		.word(d_rdat),
		.out(ld_out)
	);

	// ============================================================
	// the one sequential process
	// ============================================================

	reg        rf_we;
	reg  [4:0] rf_idx;
	reg [31:0] rf_dat;

	always @(posedge clk) begin

		irq_pending_n = irq_pending & LATCHED_IRQ;

		if (ENABLE_COUNTERS) count_cycle <= count_cycle + 64'd1;

		rf_we  = w_wr;
		rf_idx = w_rd;
		rf_dat = w_val;

		if (rst) begin

			run <= 1'b1;
			trap <= 1'b0;
			eoi <= 32'd0;

			pc_f <= PROGADDR_RESET;
			f_out <= 1'b0;
			f_kill <= 1'b0;
			f_pc <= 32'd0;
			f_faulted <= 1'b0;
			fq_cnt <= 2'd0;
			f_bt <= 1'b0;
			bw_v <= 1'b0;

			ex_valid <= 1'b0;
			x_quiet <= 1'b0;
			ex_started <= 1'b0;
			ex_jdone <= 1'b0;
			jr_v <= 1'b0;
			ex_md_have <= 1'b0;
			q_v <= 2'b00;
			lx_valid <= 1'b0;
			priv_x <= 1'b1;            // as wb_mpu: privileged from reset
			lastx_pc <= 32'h0;
			fv_ev <= 1'b0;
			q_rp <= 1'b0;
			q_wp <= 1'b0;
			w_valid <= 1'b0;

			q0 <= 32'd0; q1 <= 32'd0; q2 <= 32'd0; q3 <= 32'd0;
			irq_mask <= ~32'd0;
			irq_pending_n = 32'd0;
			irq_active <= 1'b0;
			irq_delay <= 1'b0;

			count_cycle <= 64'd0;
			count_instr <= 64'd0;
			timer_q <= 32'd0;

			// picorv32 sets x2 from STACKADDR at reset
			rf_we  = 1'b1;
			rf_idx = 5'd2;
			rf_dat = STACKADDR;

		end else begin

			// -- F ------------------------------------------------

			if (i_take) begin
				f_out <= 1'b1;
				f_pc <= pc_f;
				f_bh <= bf_hit;
				f_bt <= bf_take;
				f_bc <= bf_ectr;
				f_bg <= bf_etgt;
			end else if (i_ack) begin
				f_out <= 1'b0;
			end

			if (redirect) begin
				pc_f <= redirect_pc;
				f_faulted <= 1'b0;
				// whatever is in flight now, or goes out this cycle, is
				// for the old path
				f_kill <= i_take || (f_out && !i_ack);
			end else begin
				if (i_take) begin
					pc_f <= bf_take ? { bf_etgt, 2'b00 } : (pc_f + 32'd4);
					f_kill <= 1'b0;
				end else if (i_ack) begin
					f_kill <= 1'b0;
				end
				if (f_fpush) f_faulted <= 1'b1;
			end

			// -- the fetch queue --------------------------------------

			// d_go takes the head; an arrival goes in behind the
			// last entry (one place lower when the head leaves in the
			// same cycle). The queue cannot be full when one arrives
			// (f_room).
			// (The entries are written whether or not X redirects:
			// a redirect only empties the queue, so what they hold
			// then is never read, and the redirect -- a branch
			// compare -- stays out of 200 enables.)
			if (d_go) begin
				fq_i0 <= fq_i1; fq_p0 <= fq_p1; fq_f0 <= fq_f1;
				fq_i1 <= fq_i2; fq_p1 <= fq_p2; fq_f1 <= fq_f2;
				fq_h0 <= fq_h1; fq_t0 <= fq_t1; fq_c0 <= fq_c1; fq_g0 <= fq_g1;
				fq_h1 <= fq_h2; fq_t1 <= fq_t2; fq_c1 <= fq_c2; fq_g1 <= fq_g2;
			end
			if (arrive) begin
				case (d_go ? fq_cnt - 2'd1 : fq_cnt)
					2'd0: begin fq_i0 <= arr_i; fq_p0 <= arr_p; fq_f0 <= arr_f;
						fq_h0 <= f_bh; fq_t0 <= arr_t; fq_c0 <= f_bc; fq_g0 <= f_bg; end
					2'd1: begin fq_i1 <= arr_i; fq_p1 <= arr_p; fq_f1 <= arr_f;
						fq_h1 <= f_bh; fq_t1 <= arr_t; fq_c1 <= f_bc; fq_g1 <= f_bg; end
					default: begin fq_i2 <= arr_i; fq_p2 <= arr_p; fq_f2 <= arr_f;
						fq_h2 <= f_bh; fq_t2 <= arr_t; fq_c2 <= f_bc; fq_g2 <= f_bg; end
				endcase
			end
			if (redirect)
				fq_cnt <= 2'd0;
			else
				fq_cnt <= fq_cnt + (arrive ? 2'd1 : 2'd0) - (d_go ? 2'd1 : 2'd0);

			// -- D -> X ------------------------------------------------

			if (d_go) begin
				ex_instr <= d_ins;
				ex_pc <= d_ipc;
				ex_ff <= d_ff;
				ex_bh <= fq_h0 && !d_ff;
				ex_bc <= fq_c0;
				ex_pt <= fq_t0 && !d_ff;
				ex_ptok <= d_ptok;
				ex_preg <= chk_privreg;
				ex_pgate <= chk_gate;
				ex_pown <= chk_own;
				ex_r1 <= d_r1;
				ex_r2 <= d_r2;
				ex_started <= 1'b0;
				ex_jdone <= 1'b0;
				ex_md_have <= 1'b0;
			end else if (ex_valid) begin
				// waiting: keep what forwarding provides now
				ex_r1 <= xa;
				ex_r2 <= xb;
			end
			if (x_free)
				ex_valid <= d_go && !redirect && !x_jump;
			// a jump's redirect, out next cycle (above)
			jr_v <= x_jump;
			jr_pc <= x_tgo ? x_target : pc_plus4;

			// -- X: an instruction under way -----------------------

			if (md_start) ex_started <= 1'b1;
			if (x_jump && !x_adv) begin
				ex_started <= 1'b1;
				ex_jdone <= 1'b1;
			end
			if (x_okn && is_waitirq && x_wait_irq && !x_serial)
				ex_started <= 1'b1;
			if (md_ready && !d_go) begin
				ex_md_have <= 1'b1;
				ex_md_res <= md_result;
			end

			// -- X: an interrupt taken -----------------------------

			if (x_irq) begin
				q0 <= ex_pc;
				q1 <= irq_delivered;
				eoi <= irq_delivered;
				irq_pending_n = irq_pending_n & irq_mask;
				irq_active <= 1'b1;
			end

			// -- X: the faults ---------------------------------------

			if (x_skip) begin
				if (x_fault_bit2) irq_pending_n[2] = 1'b1;
				else irq_pending_n[1] = 1'b1;
			end
			if (x_ffpend) begin
				irq_pending_n[2] = 1'b1;
				// the interrupt is taken next cycle, on this marker,
				// with q0 = the misaligned address; irq_delay must not
				// stand in its way (zeitlos32 clears it at the fetch)
				irq_delay <= 1'b0;
			end
			if (x_trap) begin
				run <= 1'b0;
				trap <= 1'b1;
			end

			// -- X: an instruction executes ------------------------

			// -- X: the MPU's view of it (ENABLE_MPU) --------------
			fv_ev <= fv && (x_adv || x_skip || x_trap);
			fv_adr <= ENABLE_MPU ? ex_pc : 32'h0;
			fv_pc <= ENABLE_MPU ? lastx_pc : 32'h0;
			fv_why <= ENABLE_MPU ? fv_reason : 4'h0;
			if (x_adv || x_skip) begin
				priv_x <= x_newpriv;
				lastx_pc <= ex_pc;
			end

			// -- X: the outcome into the BTB (a cycle late) -------
			// a taken one is entered (weakly taken) or strengthened, a
			// not-taken one weakened if it is there; nothing else written
			bw_v <= (BTB_BITS > 0) && x_adv && (is_jal || is_branch) &&
			        (x_ctl || ex_bh);
			bw_idx <= ex_pc[BTB_IB+1:2];
			bw_tag <= ex_pc[BTB_IB+9:BTB_IB+2];
			bw_tgt <= x_target[31:2];
			bw_ctr <= !ex_bh ? 2'b10 :
				x_ctl ? ((ex_bc == 2'b11) ? 2'b11 : ex_bc + 2'd1) :
				        ((ex_bc == 2'b00) ? 2'b00 : ex_bc - 2'd1);

			if (x_adv || x_skip) begin
				if (ENABLE_COUNTERS) count_instr <= count_instr + 64'd1;
				irq_delay <= x_adv && is_retirq;
			end

			// -- the timer ------------------------------------------
			if (x_adv && is_timer) begin
				timer_q <= xa;
			end else if (timer_q != 32'd0) begin
				timer_q <= timer_q - 32'd1;
				if (timer_q == 32'd1) irq_pending_n[0] = 1'b1;
			end

			if (x_adv && is_custom) begin
				case (funct7[2:0])
					3'b001: begin                     // setq
						case (rd_idx[1:0])
							2'd0: q0 <= xa;
							2'd1: q1 <= xa;
							2'd2: q2 <= xa;
							default: q3 <= xa;
						endcase
					end
					3'b010: begin                     // retirq
						irq_active <= 1'b0;
						eoi <= 32'd0;
					end
					3'b011: irq_mask <= xa;           // maskirq
					3'b101: ;                         // timer: below
					default: ;
				endcase
			end

			// -- X -> M ------------------------------------------------

			// X writes the slot q_wp names (empty: X moves on only
			// when S is) -- the registers whenever X is finished
			// (x_go), the valid bit only when the instruction really
			// moves on; M retiring empties its slot and q_rp moves on.
			if (x_go && !q_wp) begin
				q0_dop <= x_dop; q0_load <= is_load; q0_we <= x_we;
				q0_rd <= rd_idx; q0_val <= is_store ? st_dat : x_res;
				q0_f3 <= funct3; q0_lo <= mem_adr[1:0]; q0_pc <= ex_pc;
				q0_adr <= x_padr; q0_sel <= x_sel; q0_wr <= is_store;
				q0_priv <= x_newpriv;
			end
			if (x_go && q_wp) begin
				q1_dop <= x_dop; q1_load <= is_load; q1_we <= x_we;
				q1_rd <= rd_idx; q1_val <= is_store ? st_dat : x_res;
				q1_f3 <= funct3; q1_lo <= mem_adr[1:0]; q1_pc <= ex_pc;
				q1_adr <= x_padr; q1_sel <= x_sel; q1_wr <= is_store;
				q1_priv <= x_newpriv;
			end
			q0_iss <= (x_go && !q_wp) ? x_taken : (q0_iss || (iss_q0 && d_rdy));
			q1_iss <= (x_go &&  q_wp) ? x_taken : (q1_iss || (iss_q1 && d_rdy));
			q_v[0] <= (x_adv && !q_wp) || (q_v[0] && !(m_go && !q_rp));
			q_v[1] <= (x_adv &&  q_wp) || (q_v[1] && !(m_go &&  q_rp));
			if (x_adv) q_wp <= !q_wp;
			if (m_go) q_rp <= !q_rp;

			x_quiet <= !m_valid && !s_valid && !w_valid;

			lx_valid <= x_adv && x_we && !is_load && (rd_idx != 5'd0);
			lx_rd <= rd_idx;
			lx_val <= x_res;

			// -- M -> W ------------------------------------------------

			w_valid <= m_go;
			w_we <= m_we;
			w_rd <= m_rd;
			w_val <= m_load ? ld_out : m_val;
			w_pc <= m_pc;

		end

		// the register file's one write port
		if (rf_we && rf_idx != 5'd0) regs[rf_idx] <= rf_dat;

		// incoming lines folded in last, as zeitlos32
		irq_pending_n = irq_pending_n | irq;
		irq_pending <= irq_pending_n & ~MASKED_IRQ;

	end

endmodule
