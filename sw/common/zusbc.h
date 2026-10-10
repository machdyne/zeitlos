#ifndef ZUSBC_H
#define ZUSBC_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The client of the USB driver (sw/apps/usb), for `fs`, `net`,
 * `serial` and the kernel shell: requests to `usb` and their replies,
 * as zusbproto.h describes. The ethernet and serial calls are declared
 * in zusbnet.h and zusbcdc.h; storage and the shell's commands here.
 *
 * Each call is one round trip, about a millisecond, and waits only for
 * its own reply (z_msg_recv_match()): other mail stays queued. With no
 * `usb` -- no controller on this board, or not started yet -- a call
 * fails at once with -Z_ENODEV.
 *
 * The kernel builds this too (ZUSBC_KERNEL, sw/os/kusb.c), for its
 * shell's `lsusb` and the debug commands: only zusb_cmd().
 */

#include <stdint.h>
#include <stdbool.h>
#include "zusbproto.h"

// The drive: present, ready, sectors, generation. Also makes the
// caller the drive's client, which Z_USBEV_MSC events go to.
bool zusb_msc_info(z_usb_msc_info_t *mi);

// Bring a newly bound drive up (TEST UNIT READY until ready, then its
// geometry). Seconds at worst.
int32_t zusb_msc_start(void);

// Sectors, any number: split into requests of Z_USB_MSC_MAX. 0 or -Z_E*.
int32_t zusb_msc_read(uint32_t lba, uint32_t count, void *buf);
int32_t zusb_msc_write(uint32_t lba, uint32_t count, const void *buf);

// A command line for `usb` to run: lsusb, usbrescan, and in its
// USBH_DEBUG build the bring-up tools. It prints on the console. 0, or
// -Z_ENOENT for a command it does not know.
int32_t zusb_cmd(const char *line);

#endif
