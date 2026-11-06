/*
 * mauskart3d -- the karts' lists (karts.c).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */
#ifndef MAUSKART_KARTS_H
#define MAUSKART_KARTS_H

#include <stdint.h>

#define RACERS 4

typedef struct {
	const char *name;
	uint8_t ramp, ramp2;       /* the driver's colours */
	uint8_t box;               /* the matchbox's */
} racer_t;
extern const racer_t kt_racer[RACERS];

#define KT_NEAR(r)  (r)
#define KT_FAR(r)   (RACERS + (r))
#define KT_SHADOW   (2 * RACERS)
#define KT_LISTS    (2 * RACERS + 1)

extern uint32_t kt_list;
extern uint16_t kt_tris[KT_LISTS];
void kt_make(void);

#endif
