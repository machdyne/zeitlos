#ifndef Z_USBCDCAPI_H
#define Z_USBCDCAPI_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Syscalls onto the USB CDC-ACM driver (usb/usbh_cdc.c), for apps.
 * The app side and the argument shape are sw/common/zusbcdc.h.
 */

#include "kernel.h"

z_obj_t *k_usbcdc_present(z_obj_t *args);
z_obj_t *k_usbcdc_read(z_obj_t *args);
z_obj_t *k_usbcdc_write(z_obj_t *args);

#endif
