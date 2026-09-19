/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB mass storage -- interface. See usbh_msc.c and docs/usb_host.md.
 *
 * Everything here BLOCKS, unlike the rest of this driver. That is only
 * safe because these are reachable solely through FatFs's diskio_mux,
 * which runs inside k_fs_enter()'s k_no_preempt window. Read the
 * header comment in usbh_msc.c before calling any of it from anywhere
 * else.
 */

#ifndef Z_USBH_MSC_H
#define Z_USBH_MSC_H

#include <stdint.h>

#define Z_USBH_MSC_OK     0
#define Z_USBH_MSC_FAIL   1     // device said the command failed
#define Z_USBH_MSC_STALL  2     // endpoint stalled, needs recovery
#define Z_USBH_MSC_ERR    3     // transport error or timeout

typedef struct {
    uint8_t addr;
    uint8_t port;
    uint8_t xa_flags;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t mps;
    // Bulk data toggles persist for the life of the endpoint -- only a
    // reset or CLEAR_FEATURE(HALT) clears them -- so they live here
    // rather than being reset per transfer.
    uint8_t tgl_in;
    uint8_t tgl_out;
    uint8_t ready;      // claimed, endpoints known
    uint8_t started;    // geometry known, safe to read and write
    uint16_t last_len;
    uint8_t last_status;
    uint32_t tag;
    uint32_t sectors;
    uint32_t sector_size;
} z_usbh_msc_t;

// Claim a device from its configuration descriptor. Non-zero if this
// driver took it.
int z_usbh_msc_bind(uint8_t addr, uint8_t xa_flags, uint8_t port,
                    uint8_t mps0, const uint8_t *cfg, int cfg_len);

// Wait for the unit to report ready and read its geometry. Call once
// after bind, NOT inside a filesystem lock.
int z_usbh_msc_start(void);

// Claimed: endpoints known, but the geometry may not be read yet.
int z_usbh_msc_present(void);

// Started: TEST UNIT READY passed and READ CAPACITY succeeded, so
// reads and writes are safe. disk_status() reports this.
int z_usbh_msc_ready(void);
uint32_t z_usbh_msc_sectors(void);

int z_usbh_msc_read(uint32_t lba, uint8_t *dst, uint32_t count);
int z_usbh_msc_write(uint32_t lba, const uint8_t *src, uint32_t count);

#endif
