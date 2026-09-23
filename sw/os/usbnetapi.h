#ifndef Z_USBNETAPI_H
#define Z_USBNETAPI_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The syscall onto the USB ethernet (CDC-ECM) driver, usb/usbh_ecm.c.
 * The app side and the argument shapes are sw/common/zusbnet.h.
 */

#include "kernel.h"

z_obj_t *k_usbnet(z_obj_t *args);

#endif
