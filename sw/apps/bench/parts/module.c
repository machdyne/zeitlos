/*
 * bench parts -- Zwölf modules, run by ls99 (sw/apps/ls99, docs/ls99.md):
 *
 *   module m1 LS10 program=/basic/GROW.BAS [addr=0x0c]
 *
 * Pins A-D (LS10) or A-G (LS11, LS99), then LED, high while it is lit.
 * The module's own process drives them; the part only holds them. Its
 * Sechs side answers on a bus at addr (0x0c, as a new module); its C/D
 * may be the master of a bus (bus NAME master=m1 ...).
 */

#include <string.h>
#include "../core.h"

typedef struct {
    char program[48];
} module_t;

static const char *const pins_ls10[] = { "A", "B", "C", "D", "LED" };
static const char *const pins_ls11[] = { "A", "B", "C", "D", "E", "F", "G", "LED" };

static const char *init(part_t *p, const param_t *pp) {
    module_t *m = BN_STATE_OF(p, module_t);
    const char *prog = bn_param(pp, "program");
    memset(m, 0, sizeof(*m));
    if (prog) {
        if (strlen(prog) >= sizeof(m->program)) return ": program= is too long a path";
        strcpy(m->program, prog);
    }
    for (int i = 0; i < p->type->npins; i++) p->drive[i] = BN_NONE;
    return 0;
}

static bool lit(part_t *p) {
    return bn_pin_get(p, p->type->npins - 1) == BN_L1;
}

const char *bn_module_program(part_t *p) {
    return BN_STATE_OF(p, module_t)->program;
}

const part_type_t pt_ls10 = {
    "ls10", "Zwölf LS10: BASIC 1, 1KB programs, pins A-D (virtual: ls99)",
    pins_ls10, 5, 0x08, 0x77, init, 0, 0, 0, 0, 0, lit, 0x0C, 1,
};

const part_type_t pt_ls11 = {
    "ls11", "Zwölf LS11: BASIC 1, 4KB programs, pins A-G (virtual: ls99)",
    pins_ls11, 8, 0x08, 0x77, init, 0, 0, 0, 0, 0, lit, 0x0C, 2,
};

const part_type_t pt_ls99 = {
    "ls99", "a virtual Zwölf module: BASIC 1, 32KB programs, pins A-G",
    pins_ls11, 8, 0x08, 0x77, init, 0, 0, 0, 0, 0, lit, 0x0C, 3,
};
