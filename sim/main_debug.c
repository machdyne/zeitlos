#include <stdio.h>
#include <stdlib.h>
#include "machine.h"

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <bin> [n]\n", argv[0]); return 1; }
	uint64_t n = argc > 2 ? strtoull(argv[2], NULL, 0) : 200;

	machine_t m;
	machine_init(&m, 0);
	machine_load_bin(&m, argv[1]);

	for (uint64_t i = 0; i < n; i++) {
		if (m.cpu.pc == ZS_SYSCALL_TRAP_PC) { printf("[%llu] SYSCALL TRAP pc=0x%08x a0=%u a1=0x%08x\n",
			(unsigned long long)i, m.cpu.pc, m.cpu.regs[10], m.cpu.regs[11]); break; }
		if (m.cpu.pc == 0) { printf("[%llu] PC==0 (return-to-null)\n", (unsigned long long)i); break; }
		uint32_t insn = bus_read32(&m, m.cpu.pc);
		printf("[%llu] pc=0x%08x insn=0x%08x sp=0x%08x ra=0x%08x a0=0x%08x a5=0x%08x\n",
			(unsigned long long)i, m.cpu.pc, insn, m.cpu.regs[2], m.cpu.regs[1],
			m.cpu.regs[10], m.cpu.regs[15]);
		if (cpu_step(&m.cpu, &m) != 0) {
			printf("TRAPPED (%d) at pc=0x%08x\n", m.cpu.trapped, m.cpu.trap_pc);
			break;
		}
	}
	machine_destroy(&m);
	return 0;
}
