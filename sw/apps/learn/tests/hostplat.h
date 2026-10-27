#ifndef LEARN_HOSTPLAT_H
#define LEARN_HOSTPLAT_H
#include "../lplat.h"
extern const lplat_t host_plat;
extern char hp_root[512];
extern uint32_t hp_now, hp_ms_now;
extern char hp_said[2048], hp_prompt_answer[128], hp_ran[128];
extern bool hp_prompt_ok;
extern int hp_confirm_answer;
void hp_reset_card(void);
#endif
