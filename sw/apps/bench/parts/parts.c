/*
 * bench parts -- every part type. Adding one: its file in parts/, its
 * line here, its object in the Makefile, its tests.
 */

#include <string.h>
#include "../core.h"

extern const part_type_t pt_tca9535, pt_tca9555, pt_led, pt_load, pt_button, pt_switch,
    pt_ls10, pt_ls11, pt_ls99, pt_gpio;

const part_type_t *const bn_types[] = {
    &pt_tca9535, &pt_tca9555, &pt_led, &pt_load, &pt_button, &pt_switch,
    &pt_ls10, &pt_ls11, &pt_ls99, &pt_gpio, 0,
};

const part_type_t *bn_type_find(const char *name) {
    for (int i = 0; bn_types[i]; i++)
        if (!strcmp(bn_types[i]->name, name)) return bn_types[i];
    return 0;
}
