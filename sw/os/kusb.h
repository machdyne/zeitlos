#ifndef Z_KUSB_H
#define Z_KUSB_H

/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What is left of USB in the kernel since Phase 3 (docs/usb_driver.md):
 * the host stack is the `usb` process (sw/apps/usb), and the kernel
 * only keeps its interrupt quiet when nobody holds it, answers the
 * retired syscalls, and passes the shell's USB commands on.
 */

#include "kernel.h"

// Z_SYS_USBCDC_* and Z_SYS_USBNET, which went with the stack: fail.
z_obj_t *k_usb_retired(z_obj_t *args);

// IRQ 9 fired with no driver bound: turn the controller's interrupt
// sources off, or the level holds the CPU in the handler.
void k_usb_irq_off(void);

// The shell's `lsusb` and the other USB commands: run by `usb`, which
// prints. False if there is no `usb` to ask.
bool k_usb_cmd(const char *line);

#endif
