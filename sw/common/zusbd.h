#ifndef ZUSBD_H
#define ZUSBD_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * rtl/usbd_ep.v's registers: the USB-C socket as a device in software
 * (`USB_DEV, FEATURES3 bit 4: z_usb_dev() in zsoc.h). docs/usb_dev.md.
 *
 * Two drivers share it and never touch each other's registers:
 *
 *   sw/apps/usbd       the CPU endpoints (0-3, 5-7): control, the
 *                      console (CDC-ACM), the notifications; and the
 *                      frame endpoint's enable (EPTYPE bit 24). While
 *                      the card is shared, sw/apps/share runs the same
 *                      code with the card reader (MSC) and usbd waits.
 *   sw/apps/net        the frame endpoint (CDC-ECM data): the ZU_N*
 *                      registers and the two frame windows
 */

#include <stdint.h>
#include "zsubjects.h"

#define ZU_BASE			0x62000000u

#define ZU_MAGIC		(ZU_BASE + 0x000)
#define ZU_MAGIC_VAL	0x56454455u		/* "UDEV" */
#define ZU_CTRL			(ZU_BASE + 0x004)	/* bit 0 attach, 14:8 address */
#define ZU_EVENTS		(ZU_BASE + 0x008)	/* bit 0 bus reset, 1 link changed; w1c */
#define ZU_FRAME		(ZU_BASE + 0x00C)
#define ZU_EPTYPE		(ZU_BASE + 0x010)	/* 7:0 IN bulk, 15:8 OUT bulk, 23:16 IN int, 24 frame ep */
#define ZU_TOGGLE		(ZU_BASE + 0x014)	/* 7:0 IN, 15:8 OUT (bit 4: the frame ep): DATA0 */
#define ZU_IRQEN		(ZU_BASE + 0x018)
#define ZU_PEND			(ZU_BASE + 0x01C)
#define ZU_CAPS			(ZU_BASE + 0x020)

#define ZU_EV_RESET		(1u << 0)
#define ZU_EV_LINK		(1u << 1)
#define ZU_EPTYPE_FRAME	(1u << 24)
#define ZU_EPT_IN_BULK(n)	(1u << (n))
#define ZU_EPT_OUT_BULK(n)	(1u << (8 + (n)))
#define ZU_EPT_IN_INT(n)	(1u << (16 + (n)))
#define ZU_TOG_IN(n)	(1u << (n))
#define ZU_TOG_OUT(n)	(1u << (8 + (n)))

/* PEND and IRQEN bits */
#define ZU_P_OUT(n)		(1u << (n))		/* OUT buffer n full */
#define ZU_P_IN(n)		(1u << (8 + (n)))	/* IN buffer n free */
#define ZU_P_RESET		(1u << 16)
#define ZU_P_LINK		(1u << 17)

/* CPU endpoint n, 0-3 and 5-7 */
#define ZU_OUT(n)		(ZU_BASE + 0x100 + 16u * (n))	/* r: 0 full, 1 SETUP, 14:8 len; w: empty */
#define ZU_IN(n)		(ZU_BASE + 0x104 + 16u * (n))	/* r: 8 armed; w: 6:0 len, 8 arm */
#define ZU_STALL(n)		(ZU_BASE + 0x108 + 16u * (n))
#define ZU_OBUF(n)		(ZU_BASE + 0x400 + 0x80u * (n))
#define ZU_IBUF(n)		(ZU_BASE + 0x440 + 0x80u * (n))

#define ZU_OUT_FULL		(1u << 0)
#define ZU_OUT_SETUP	(1u << 1)
#define ZU_IN_ARMED		(1u << 8)

/* the frame endpoint (endpoint 4), net's */
#define ZU_NSTAT		(ZU_BASE + 0x080)
#define ZU_NRXCTL		(ZU_BASE + 0x084)	/* w: the ready frame is done with */
#define ZU_NTXCTL		(ZU_BASE + 0x088)	/* w: 11:0 length, 16 send, 17 abandon */
#define ZU_NCTL			(ZU_BASE + 0x08C)	/* bit 0: net is up */
#define ZU_NRXBUF		(ZU_BASE + 0x800)	/* the ready frame, 1.5 KB */
#define ZU_NTXBUF		(ZU_BASE + 0x1000)	/* the frame to send, 1.5 KB */

#define ZU_N_RX_READY	(1u << 0)
#define ZU_N_TX_BUSY	(1u << 1)
#define ZU_N_ENABLED	(1u << 2)		/* the host selected the data interface's alt 1 */
#define ZU_N_LINK		(1u << 3)
#define ZU_N_DROPS(s)	(((s) >> 4) & 0xffu)
#define ZU_N_RXLEN(s)	(((s) >> 16) & 0xfffu)
#define ZU_NTX_SEND		(1u << 16)
#define ZU_NTX_ABANDON	(1u << 17)
#define ZU_FRAME_MAX	1536u		/* a slot: past ECM's 1514 */

/*
 * Who runs the device: usbd, or share while the card is shared
 * (sw/apps/usbd/usbd.c, "Who runs the device"; docs/usb_dev.md).
 *
 *   subject  Z_USBD_PAUSE, obj none                share0 -> usbd
 *     usbd leaves the bus, gives up IRQ 11 and answers Z_USBD_PAUSED.
 *     Only from the process registered as Z_USBD_SHARE_NAME.
 *
 *   subject  Z_USBD_PAUSED, obj Z_UINT32 or Z_NONE  usbd -> share0
 *     The console log position the next device's terminal starts at;
 *     Z_NONE: refused (not share0, or already paused).
 *
 *   subject  Z_USBD_RESUME, obj Z_UINT32           share0 -> usbd
 *     share0 has left the bus and IRQ 11; usbd takes them back, its
 *     terminal starting at that position. No reply. usbd also resumes
 *     by itself when share0 is gone.
 *
 *   subject  Z_USBD_EJECT, obj none                anyone -> share0
 *     Give the card back, as the Eject button does (`share` run while
 *     share0 is running sends it). No reply.
 *
 * On Sergei and Mozart ML2 there is no USB device: share serves the card
 * over the XA-XD link to the carrier's RP2040 instead (`XLINK,
 * docs/xlink.md, "The card on the PC"), and it is netx that steps aside:
 *
 *   subject  Z_USBD_LINK_PAUSE, obj none           share0 -> netx0
 *     netx leaves the link (the PC's network on the carrier's USB pauses)
 *     and answers Z_USBD_LINK_PAUSED. Only from share0.
 *
 *   subject  Z_USBD_LINK_PAUSED, obj Z_UINT32      netx0 -> share0
 *     1: the link is share0's; 0: refused.
 *
 *   subject  Z_USBD_LINK_RESUME, obj none          share0 -> netx0
 *     share0 is done with the link; netx takes it back. No reply. netx
 *     also takes it back by itself when share0 is gone.
 */
#define Z_USBD_PAUSE		(Z_SUBJ_USBD + 0)
#define Z_USBD_PAUSED		(Z_SUBJ_USBD + 1)
#define Z_USBD_RESUME		(Z_SUBJ_USBD + 2)
#define Z_USBD_EJECT		(Z_SUBJ_USBD + 3)
#define Z_USBD_LINK_PAUSE	(Z_SUBJ_USBD + 4)
#define Z_USBD_LINK_PAUSED	(Z_SUBJ_USBD + 5)
#define Z_USBD_LINK_RESUME	(Z_SUBJ_USBD + 6)
Z_SUBJECTS_IN(Z_SUBJ_USBD, Z_USBD_LINK_RESUME);
#define Z_USBD_SHARE_NAME	"share0"

#ifdef ZU_COSIM
/* rtl/tb/cosim/usbd_vpi.c: each access a real bus cycle on the gateware */
uint32_t zu_rd(uint32_t a);
void zu_wr(uint32_t a, uint32_t v);
#else
static inline uint32_t zu_rd(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void zu_wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
#endif

#endif
