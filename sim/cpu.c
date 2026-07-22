#include <string.h>
#include "cpu.h"
#include "machine.h"

static inline int32_t sext(uint32_t v, int bits) {
	uint32_t m = 1u << (bits - 1);
	return (int32_t)((v ^ m) - m);
}

void cpu_reset(cpu_t *cpu, uint32_t pc, uint32_t sp) {
	memset(cpu->regs, 0, sizeof(cpu->regs));
	cpu->pc = pc;
	cpu->regs[2] = sp;   /* x2 = sp */
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

	case 0x33: /* ALU register-register (RV32I subset only: no mul/div) */
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

	case 0x73: /* ECALL / EBREAK / CSR -- not used by the syscall-gate ABI,
	            * but handled gracefully rather than crashing the interpreter. */
		if (insn == 0x00000073 || insn == 0x00100073) {
			/* ECALL / EBREAK: treat as a halt request the frontend can see */
			cpu->trapped = 2;
			cpu->trap_pc = pc;
			return -1;
		}
		/* CSR* instructions: read as 0, ignore writes. Good enough since
		 * this SOC's apps never touch machine-mode CSRs directly. */
		rset(cpu, rd, 0);
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
