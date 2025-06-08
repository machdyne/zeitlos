/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * This is the Zeitlos microkernel.
 *
 * The kernel is loaded at the beginning of main memory (0x4000_0000).
 *
 */

#include <stdint.h>
#include "../common/zeitlos.h"

#define Z_API_FUNC(name, ret, ...) ret name##_impl(__VA_ARGS__);
#include "../common/api.def"
#undef Z_API_FUNC

#define Z_API_FUNC(name, ret, ...) .name = name##_impl,
z_api_map_t api_map = {
    #include "../common/api.def"
};
#undef Z_API_FUNC

z_api_map_t *z_api_map(void) {
    return &api_map;
}
