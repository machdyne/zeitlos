#ifndef XLINK_ETH_H
#define XLINK_ETH_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's Ethernet over the XA-XD link (xlink_eth.c, docs/xlink.md).
 * Built with -DNETX only.
 */

#include <stdint.h>
#include <stdbool.h>

/* the messages (machboot's framing, docs/xlink.md): a request is
 * [op][tag][pc][0] then its data, a reply [op|0x80][tag][status][n]
 * then its data */
#define XL_HELLO	1
#define XL_NET_PUT	12		/* the PC's frame, to Zeitlos */
#define XL_NET_GET	13		/* Zeitlos's frame, to the PC */
#define XL_SYSTEM_ZEITLOS 0x5A		/* HELLO's reply, byte 1 */
#define XL_CAP_NET	0x01
#define XL_ST_OK	0
#define XL_ST_BAD	3
#define XL_ST_BUSY	5
#define XL_ST_NONE	6
#define XL_PC_USB	0x01		/* the pc byte: a PC configured the RP2040's USB */
#define XL_PC_NET	0x02		/* ... and has its network interface up */
#define XL_F_FRAME	0x02		/* FLAGS: a frame waits for NET_GET */
#define XL_F_UP		0x04		/* FLAGS: netx is up */

#define XLINK_TXQ	4		/* frames waiting for the PC */

bool xlink_eth_supported(void);
bool xlink_eth_init(const uint8_t mac[6]);
uint16_t xlink_eth_recv(uint8_t *buf, uint16_t maxlen);
bool xlink_eth_send(const uint8_t *buf, uint16_t len);
void xlink_eth_debug_dump(void);
int xlink_eth_tx_room(void);
/* share0 serves the card over the link (zusbd.h, Z_USBD_LINK_PAUSE):
   netx leaves it alone until resumed, or until share0 is gone */
void xlink_eth_pause(bool on);
bool xlink_eth_paused(void);

#endif
