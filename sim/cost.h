/* the simulator's cycle model (cost.c) */
#include <stdint.h>
extern int cost_on;
int cost_enabled(void);
void cost_insn(int taken);
void cost_fetch(uint32_t pc);
void cost_load(uint32_t a);
void cost_store(uint32_t a);
uint64_t cost_cycles(void);
void cost_report(void);
