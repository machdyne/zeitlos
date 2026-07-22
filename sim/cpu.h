/*
 * zeitlos-sim: a minimal RV32I interpreter core.
 *
 * This matches the picorv32 configuration used by the real Zeitlos SOC
 * (see rtl/sysctl.v): BARREL_SHIFTER=1, COMPRESSED_ISA=0, ENABLE_MUL=0,
 * ENABLE_DIV=0. So only the base RV32I integer instruction set needs to
 * be supported -- no M/C/F extensions, no MMU, no privilege levels.
 *
 * The core is bus-agnostic: it calls back into the machine (via the
 * function pointers in machine_t, see machine.h) for all memory access,
 * so it has no notion of the Zeitlos memory map itself.
 */

#ifndef ZSIM_CPU_H
#define ZSIM_CPU_H

#include <stdint.h>

struct machine;

typedef struct {
	uint32_t regs[32];   /* x0..x31, x0 is always read as 0 */
	uint32_t pc;

	/* stats / debug */
	uint64_t insn_count;
	int trapped;         /* set to 1 on illegal instruction */
	uint32_t trap_pc;
} cpu_t;

void cpu_reset(cpu_t *cpu, uint32_t pc, uint32_t sp);

/* Executes exactly one instruction. Returns 0 on success, -1 if the
 * instruction was illegal/unsupported (cpu->trapped will be set). */
int cpu_step(cpu_t *cpu, struct machine *m);

#endif
