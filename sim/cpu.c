#include <string.h>
#include "cpu.h"
#include "machine.h"

static inline int32_t sext(uint32_t v, int bits) {
	uint32_t m = 1u << (bits - 1);
	return (int32_t)((v ^ m) - m);
}

void cpu_reset(cpu_t *cpu, uint32_t pc, uint32_t sp) {

	/* Registers are POISONED, not zeroed.
	 *
	 * They used to be zeroed, which is tidy and hides a whole class
	 * of bug: real hardware leaves whatever the kernel last had in
	 * them, so code that reads an argument register it was never
	 * given sees 0 here and garbage there. That is not hypothetical
	 * -- sw/apps/zcc's main() read an argc it had never been passed,
	 * ran clean under this simulator for days, and took a real
	 * machine down the first time it was typed with no arguments.
	 *
	 * 0xDEADBEEF is recognisable in a register dump and is far
	 * outside any process's window when used as a pointer.
	 *
	 * It is NOT a substitute for not reading the register. Poison
	 * catches a use, it does not predict the consequence: as a
	 * SIGNED int 0xDEADBEEF is negative, so the very loop that
	 * crashed the machine -- `for (i = 1; i < argc; i++)` -- still
	 * did not run under it. What a real kernel leaves in a0 is more
	 * likely to be a small positive leftover from the last syscall,
	 * which is the case that walks off into memory. So this makes
	 * the class of bug more likely to show, and the fix still has to
	 * be in the code that was reading a register nobody set.
	 *
	 * x0 is hardwired to zero and sp/pc are set by the caller
	 * because the kernel really does set those (k_proc_create,
	 * docs/app_runtime.md). Everything else is fair game. */
	for (int i = 0; i < 32; i++) cpu->regs[i] = 0xdeadbeefu;

	cpu->regs[0] = 0;
	cpu->regs[2] = sp;
	cpu->pc = pc;
	cpu->insn_count = 0;
	cpu->trapped = 0;
	cpu->trap_pc = 0;
}

static inline uint32_t rget(cpu_t *c, unsigned r) { return r ? c->regs[r] : 0; }
static inline void rset(cpu_t *c, unsigned r, uint32_t v) { if (r) c->regs[r] = v; }

int cpu_step(cpu_t *cpu, struct machine *m) {

	uint32_t pc = cpu->pc;
	uint32_t insn = bus_read32(m, pc);

	unsigned opcode = insn & 0x7f;
	unsigned rd     = (insn >> 7)  & 0x1f;
	unsigned funct3 = (insn >> 12) & 0x7;
	unsigned rs1    = (insn >> 15) & 0x1f;
	unsigned rs2    = (insn >> 20) & 0x1f;
	unsigned funct7 = (insn >> 25) & 0x7f;

	uint32_t next_pc = pc + 4;

	int32_t imm_i = sext(insn >> 20, 12);
	int32_t imm_s = sext(((insn >> 25) << 5) | ((insn >> 7) & 0x1f), 12);
	int32_t imm_b = sext(
		(((insn >> 31) & 1) << 12) |
		(((insn >> 7)  & 1) << 11) |
		(((insn >> 25) & 0x3f) << 5) |
		(((insn >> 8)  & 0xf) << 1), 13);
	uint32_t imm_u = insn & 0xfffff000u;
	int32_t imm_j = sext(
		(((insn >> 31) & 1) << 20) |
		(((insn >> 12) & 0xff) << 12) |
		(((insn >> 20) & 1) << 11) |
		(((insn >> 21) & 0x3ff) << 1), 21);

	uint32_t a = rget(cpu, rs1);
	uint32_t b = rget(cpu, rs2);

	switch (opcode) {

	case 0x37: /* LUI */
		rset(cpu, rd, imm_u);
		break;

	case 0x17: /* AUIPC */
		rset(cpu, rd, pc + imm_u);
		break;

	case 0x6f: /* JAL */
		rset(cpu, rd, pc + 4);
		next_pc = pc + (uint32_t)imm_j;
		break;

	case 0x67: /* JALR */
		if (funct3 != 0) { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
		{
			uint32_t target = (a + (uint32_t)imm_i) & ~1u;
			rset(cpu, rd, pc + 4);
			next_pc = target;
		}
		break;

	case 0x63: /* branches */
		{
			int take = 0;
			switch (funct3) {
			case 0: take = (a == b); break;                          /* BEQ  */
			case 1: take = (a != b); break;                          /* BNE  */
			case 4: take = ((int32_t)a <  (int32_t)b); break;        /* BLT  */
			case 5: take = ((int32_t)a >= (int32_t)b); break;        /* BGE  */
			case 6: take = (a < b); break;                           /* BLTU */
			case 7: take = (a >= b); break;                          /* BGEU */
			default: cpu->trapped = 1; cpu->trap_pc = pc; return -1;
			}
			if (take) next_pc = pc + (uint32_t)imm_b;
		}
		break;

	case 0x03: /* loads */
		{
			uint32_t addr = a + (uint32_t)imm_i;
			switch (funct3) {
			case 0: rset(cpu, rd, (uint32_t)sext(bus_read8(m, addr), 8)); break;   /* LB  */
			case 1: rset(cpu, rd, (uint32_t)sext(bus_read16(m, addr), 16)); break; /* LH  */
			case 2: rset(cpu, rd, bus_read32(m, addr)); break;                     /* LW  */
			case 4: rset(cpu, rd, bus_read8(m, addr)); break;                      /* LBU */
			case 5: rset(cpu, rd, bus_read16(m, addr)); break;                     /* LHU */
			default: cpu->trapped = 1; cpu->trap_pc = pc; return -1;
			}
		}
		break;

	case 0x23: /* stores */
		{
			uint32_t addr = a + (uint32_t)imm_s;
			switch (funct3) {
			case 0: bus_write8(m, addr, (uint8_t)b); break;   /* SB */
			case 1: bus_write16(m, addr, (uint16_t)b); break; /* SH */
			case 2: bus_write32(m, addr, b); break;           /* SW */
			default: cpu->trapped = 1; cpu->trap_pc = pc; return -1;
			}
		}
		break;

	case 0x13: /* ALU immediate */
		switch (funct3) {
		case 0: rset(cpu, rd, a + (uint32_t)imm_i); break;                      /* ADDI  */
		case 2: rset(cpu, rd, (uint32_t)((int32_t)a < imm_i)); break;           /* SLTI  */
		case 3: rset(cpu, rd, (uint32_t)(a < (uint32_t)imm_i)); break;          /* SLTIU */
		case 4: rset(cpu, rd, a ^ (uint32_t)imm_i); break;                      /* XORI  */
		case 6: rset(cpu, rd, a | (uint32_t)imm_i); break;                      /* ORI   */
		case 7: rset(cpu, rd, a & (uint32_t)imm_i); break;                      /* ANDI  */
		case 1: rset(cpu, rd, a << (rs2 & 0x1f)); break;                        /* SLLI  */
		case 5:
			if (funct7 == 0x20)
				rset(cpu, rd, (uint32_t)((int32_t)a >> (rs2 & 0x1f)));          /* SRAI */
			else
				rset(cpu, rd, a >> (rs2 & 0x1f));                               /* SRLI */
			break;
		}
		break;

	case 0x33: /* ALU register-register, RV32I + RV32M */

		/* RV32M.
		 *
		 * This used to be absent, on the grounds that rtl/sysctl.v
		 * builds picorv32 with ENABLE_MUL=0/ENABLE_DIV=0 so no mul or
		 * div instruction could ever reach here. That stopped being
		 * true: sw/common/arch.mk now defaults ARCH to rv32im, every
		 * binary in the tree is built with M, and the boards that
		 * enable `CPU_MUL/`CPU_DIV (rtl/boards.vh) run them natively.
		 * A simulator that traps on `mul` cannot run anything the
		 * current toolchain produces.
		 *
		 * There is no flag to turn this off. An rv32i binary simply
		 * contains no M-extension encodings, so supporting them costs
		 * such a binary nothing -- whereas a switch would have to be
		 * set correctly to get a correct answer, which is the failure
		 * mode arch.mk's own header comment is about.
		 *
		 * Division follows the RISC-V spec's defined results for the
		 * two cases C leaves undefined, rather than dividing in host
		 * C and inheriting whatever it does: divide by zero gives all
		 * ones (or the dividend, for remainder), and the signed
		 * overflow case INT_MIN / -1 gives INT_MIN. Getting these
		 * wrong produces a simulator that disagrees with hardware
		 * only on inputs a test suite is unlikely to generate. */
		if (funct7 == 0x01) {
			switch (funct3) {
			case 0: /* MUL */
				rset(cpu, rd, (uint32_t)((int32_t)a * (int32_t)b));
				break;
			case 1: /* MULH */
				rset(cpu, rd, (uint32_t)(((int64_t)(int32_t)a *
					(int64_t)(int32_t)b) >> 32));
				break;
			case 2: /* MULHSU */
				rset(cpu, rd, (uint32_t)(((int64_t)(int32_t)a *
					(int64_t)(uint64_t)b) >> 32));
				break;
			case 3: /* MULHU */
				rset(cpu, rd, (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32));
				break;
			case 4: /* DIV */
				if (b == 0) rset(cpu, rd, 0xffffffffu);
				else if (a == 0x80000000u && b == 0xffffffffu)
					rset(cpu, rd, 0x80000000u);
				else rset(cpu, rd, (uint32_t)((int32_t)a / (int32_t)b));
				break;
			case 5: /* DIVU */
				if (b == 0) rset(cpu, rd, 0xffffffffu);
				else rset(cpu, rd, a / b);
				break;
			case 6: /* REM */
				if (b == 0) rset(cpu, rd, a);
				else if (a == 0x80000000u && b == 0xffffffffu)
					rset(cpu, rd, 0);
				else rset(cpu, rd, (uint32_t)((int32_t)a % (int32_t)b));
				break;
			case 7: /* REMU */
				if (b == 0) rset(cpu, rd, a);
				else rset(cpu, rd, a % b);
				break;
			}
			break;
		}

		switch (funct3) {
		case 0:
			if (funct7 == 0x20) rset(cpu, rd, a - b);       /* SUB */
			else if (funct7 == 0x00) rset(cpu, rd, a + b);  /* ADD */
			else { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			break;
		case 1:
			if (funct7 != 0x00) { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			rset(cpu, rd, a << (b & 0x1f));                 /* SLL */
			break;
		case 2:
			if (funct7 != 0x00) { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			rset(cpu, rd, (uint32_t)((int32_t)a < (int32_t)b)); /* SLT */
			break;
		case 3:
			if (funct7 != 0x00) { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			rset(cpu, rd, (uint32_t)(a < b));               /* SLTU */
			break;
		case 4:
			if (funct7 != 0x00) { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			rset(cpu, rd, a ^ b);                            /* XOR */
			break;
		case 5:
			if (funct7 == 0x20) rset(cpu, rd, (uint32_t)((int32_t)a >> (b & 0x1f))); /* SRA */
			else if (funct7 == 0x00) rset(cpu, rd, a >> (b & 0x1f));                 /* SRL */
			else { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			break;
		case 6:
			if (funct7 != 0x00) { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			rset(cpu, rd, a | b);                             /* OR */
			break;
		case 7:
			if (funct7 != 0x00) { cpu->trapped = 1; cpu->trap_pc = pc; return -1; }
			rset(cpu, rd, a & b);                             /* AND */
			break;
		}
		break;

	case 0x0f: /* FENCE / FENCE.I -- no-op, single-hart, no caches to sync */
		break;

	case 0x0b: /* PicoRV32 custom instructions.
	            *
	            * Only maskirq, which is the one the software actually
	            * uses: sw/common/zeitlos.h wraps it, and now so does
	            * libz (sw/apps/zcc/libz/syscall.c) -- so the FIRST
	            * zcc-compiled program to call maskirq trapped here
	            * with an illegal instruction, which is how this got
	            * noticed.
	            *
	            * It writes the previous mask to rd and takes the new
	            * one from rs1. There are no interrupts in this
	            * simulator, so the mask is a register and nothing
	            * else -- which is faithful for every use in the tree,
	            * since maskirq's callers only ever save, mask and
	            * restore. See docs/app_runtime.md, "maskirq".
	            *
	            * The other picorv32 custom ops (getq/setq/retirq/
	            * waitirq/timer) belong to the interrupt path in
	            * sw/bios/boot_picorv32.S, which no app executes and
	            * this simulator does not run. */
		if (funct3 == 0x6 && funct7 == 0x03) {
			rset(cpu, rd, m->irq_mask);
			m->irq_mask = a;
			break;
		}
		cpu->trapped = 1;
		cpu->trap_pc = pc;
		return -1;

	case 0x73: /* ECALL / EBREAK / CSR -- not used by the syscall-gate ABI,
	            * but handled gracefully rather than crashing the interpreter. */
		if (insn == 0x00000073 || insn == 0x00100073) {
			/* ECALL / EBREAK: treat as a halt request the frontend can see */
			cpu->trapped = 2;
			cpu->trap_pc = pc;
			return -1;
		}
		/* CSR reads.
		 *
		 * These used to all read as 0, with a comment saying apps
		 * never touch machine-mode CSRs directly. Two things in the
		 * tree do, and one of them HANGS on a zero:
		 * sw/os/fs/fatfs/sdmm.c's dly_us() spins until
		 * (rdcycle() - start) reaches a target, so a counter that is
		 * always 0 never gets there. sh.c's `bench` and
		 * sw/common/zcycles.h read the same counters and would simply
		 * report nothing.
		 *
		 * cycle and instret both return the retired-instruction count
		 * rather than a modelled cycle count. That is honest about
		 * what this simulator knows -- it does not model bus latency,
		 * cache misses or the multicycle FSM, so any "cycle" figure it
		 * invented would be a lie with a plausible shape. Code that
		 * uses rdcycle as a monotonic clock (dly_us, timeouts) works
		 * correctly; code that uses it to measure performance gets a
		 * number that is obviously an instruction count, which is the
		 * right way to find out that this is not the tool for that.
		 *
		 * Writes are still ignored. cycle/instret are read-only in
		 * unprivileged mode anyway, and nothing here has the machine
		 * mode that would make the writable ones meaningful. */
		{
			unsigned csr = insn >> 20;
			uint32_t v = 0;

			switch (csr) {
			case 0xc00: /* cycle    */
			case 0xc02: /* instret  */
				v = (uint32_t)cpu->insn_count;
				break;
			case 0xc80: /* cycleh   */
			case 0xc82: /* instreth */
				v = (uint32_t)(cpu->insn_count >> 32);
				break;
			default:
				v = 0;
				break;
			}

			rset(cpu, rd, v);
		}
		break;

	default:
		cpu->trapped = 1;
		cpu->trap_pc = pc;
		return -1;
	}

	cpu->pc = next_pc;
	cpu->insn_count++;
	return 0;
}
