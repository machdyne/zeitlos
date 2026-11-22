#ifndef XLCARD_H
#define XLCARD_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * share's card over the XA-XD link (Sergei and Mozart ML2, `XLINK):
 * machboot's card-reader requests, answered by Zeitlos (xlcard.c,
 * docs/xlink.md, "The card on the PC").
 */

#include <stdint.h>

#define XLC_MAX_SECT	3		/* per request: 3 x 512 + header < 2 KB (machboot's) */

/* the card, from share.c (the same as usbd.c's card reader uses) */
int usbd_card_read(uint32_t lba, uint8_t *buf, uint32_t n);
int usbd_card_write(uint32_t lba, const uint8_t *buf, uint32_t n);

void xlc_begin(uint32_t sectors, int net);	/* FLAGS: shared, a new generation */
void xlc_end(void);				/* FLAGS: not shared, a new generation */
int xlc_serve(void);		/* requests waiting: answered. bit 0 any, bit 1 EJECT */
int xlc_pc(void);		/* the RP2040's USB is configured by a PC */
uint32_t xlc_frames(void);	/* the link's frame count (the RP2040 polls: it moves) */
int xlc_present(void);		/* the link's registers are there */
void xlc_counts(uint32_t *rd, uint32_t *wr);	/* sectors read and written */

#endif
