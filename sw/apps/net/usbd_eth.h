#ifndef USBD_ETH_H
#define USBD_ETH_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB ethernet as the DEVICE: the USB-C socket's CDC-ECM function
 * (rtl/usbd_ep.v's frame endpoint, sw/apps/usbd). The PC sees a USB
 * network adapter, and is the other end of the link -- its gateway if
 * it shares its connection. docs/usb_dev.md.
 *
 * Not usb_ecm.c, which is the other way round: a USB ethernet adapter
 * plugged INTO this board's USB host.
 */
#include <stdint.h>
#include <stdbool.h>

#ifdef Z_SMALL
/* The small build (Obst: no USB device) leaves usbd_eth.c out. */
static inline bool usbd_eth_supported(void) { return false; }
#else
bool usbd_eth_supported(void);
#endif
bool usbd_eth_init(const uint8_t mac[6]);
uint16_t usbd_eth_recv(uint8_t *buf, uint16_t maxlen);
bool usbd_eth_send(const uint8_t *buf, uint16_t len);
void usbd_eth_debug_dump(void);
int usbd_eth_tx_room(void);

#endif
