/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB mass storage -- bulk-only transport and the SCSI commands FatFs
 * needs. See docs/usb_host.md phase 5.
 *
 * -- why this one blocks, when nothing else here does --
 *
 * Every other path in this driver is a state machine that does one
 * thing and returns, because enumeration spans hundreds of
 * milliseconds and must not stall the scheduler.
 *
 * disk_read() cannot work that way. FatFs is synchronous: ff.c calls
 * disk_read() and expects the sector to be in the buffer when it
 * returns. There is no continuation to hand it.
 *
 * That is survivable here only because of where the filesystem sits.
 * k_fs_enter() (sw/os/fs/fs.c) already sets k_no_preempt around every
 * FatFs operation, so the scheduler is off for the duration whether
 * this spins or not. Spinning inside that window costs nothing extra;
 * spinning OUTSIDE it would be a bug, which is why these functions are
 * reachable only through diskio_mux.
 *
 * A 512-byte sector at full speed is eight 64-byte packets, about
 * 350 us of bus time. The SD card path (sw/os/fs/fatfs/sdmm.c) blocks
 * for considerably longer on bit-banged SPI, so this is not a new
 * class of stall for the system.
 *
 * If FS and USB ever move out of the kernel (see "Considered and
 * declined" in docs/usb_host.md), this is the file that has to change
 * shape first.
 */

#include <stdio.h>
#include <string.h>

#include "usbh.h"
#include "usbh_hw.h"
#include "usbh_msc.h"

#ifndef Z_USBH_COSIM
#include "../kernel.h"
#endif

// -- bulk-only transport, USB MSC BBB 3.1/3.2 --

#define CBW_SIG             0x43425355u     // "USBC"
#define CSW_SIG             0x53425355u     // "USBS"
#define CBW_LEN             31
#define CSW_LEN             13

#define CBW_IN              0x80

// -- SCSI, the six commands a block device actually needs --

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2a

// Interface class/subclass/protocol that identifies BOT+SCSI.
#define MSC_CLASS            0x08
#define MSC_SUBCLASS_SCSI    0x06
#define MSC_PROTO_BOT        0x50

// NAK retries per bulk transaction, the field maximum. A bulk endpoint
// NAKs freely while the device is busy -- a flash write can NAK for a
// long time -- and each NAK costs only the transaction that was going
// to happen anyway. Same value and same reasoning as the control path
// in usbh.c, kept separate so neither has to include the other.
// A few quick hardware retries for a momentarily busy device; longer
// waits are paced by bulk_xfer(). Was the field maximum, chosen before
// anything real was on the other end.
#define MSC_NAK_BUDGET       3

// Paced retries on top of that. A flash write can NAK for a long time,
// and this is reached only from FatFs, which already runs with the
// scheduler off, so waiting here costs nothing extra.
#define MSC_SOFT_NAKS        2000

// Packet buffer regions. The per-port control scratch lives at 0x400
// and 0x600 (see usbh_hw.h), and the auto-poll slots at 0x380, so the
// low half is free for block traffic.
#define MSC_OFF_DATA        0x000u          // 512, one sector
#define MSC_OFF_CBW         0x200u          // 31
#define MSC_OFF_CSW         0x240u          // 13

static z_usbh_msc_t msc;

// Chatty while the first sector read is being brought up. Every step
// of a failing command says which one it was, because "mount failed"
// on its own does not distinguish a CBW the device rejected from a
// data stage that stalled from a CSW that never arrived.
static int msc_verbose = 1;

// ---------------------------------------------------------------
// one bulk transaction, run to completion
// ---------------------------------------------------------------

/*
 * One bulk transfer, run to completion, resuming across NAKs.
 *
 * A NAK is not a failure here any more than it is on a control
 * endpoint: a mass storage device NAKs an IN while it fetches the
 * sector, and an OUT while it programs flash, and it may do so for
 * milliseconds. The hardware retries back to back with only the
 * inter-packet gap between attempts, which exhausts its small budget
 * long before a real device is ready -- the first 512-byte READ(10)
 * came back "data in 0 of 512, status NAK".
 *
 * So the hardware gets a few quick retries for a momentarily busy
 * device and this paces the rest, the same split the control path
 * uses.
 *
 * RESUMES rather than restarts. Auto-continue may already have moved
 * several packets before the budget ran out, and the device carries on
 * from where it left off; re-requesting from the beginning would slide
 * the two streams past each other. act_len says how far it got and the
 * toggle it left behind comes back in the same register.
 */
static int bulk_xfer(int in, uint32_t off, uint16_t len)
{
    uint32_t s;
    uint32_t guard;
    uint16_t done = 0;
    int naks = 0;
    uint8_t st;

    msc.last_len = 0;

    for (;;) {

        z_usbh_wr(Z_USBH_XACT_A,
                  Z_USBH_XA_ADDR(msc.addr) |
                  Z_USBH_XA_ENDP(in ? msc.ep_in : msc.ep_out) |
                  Z_USBH_XA_PID(in ? Z_USBH_PID_IN : Z_USBH_PID_OUT) |
                  Z_USBH_XA_PORT(msc.port) |
                  Z_USBH_XA_MPS(msc.mps) |
                  ((uint32_t)msc.xa_flags << 13) |
                  ((in ? msc.tgl_in : msc.tgl_out) ?
                      Z_USBH_XA_TOGGLE : 0) |
                  Z_USBH_XA_AUTOCONT);

        z_usbh_wr(Z_USBH_XACT_B,
                  Z_USBH_XB_OFF(off + done) |
                  Z_USBH_XB_LEN(len - done) |
                  Z_USBH_XB_NAK(MSC_NAK_BUDGET) |
                  Z_USBH_XB_START);

        // Bounded spin. A transaction that never completes must not
        // hang the filesystem; the hardware has its own timeouts, so
        // this only has to outlast them.
        for (guard = 0; guard < 2000000u; guard++) {
            s = z_usbh_rd(Z_USBH_XACT_S);
            if (!(s & Z_USBH_XS_PENDING)) break;
        }
        if (s & Z_USBH_XS_PENDING) return Z_USBH_MSC_ERR;

        st = (uint8_t)Z_USBH_XS_STATUS(s);
        msc.last_status = st;

        // The toggle persists for the life of a bulk endpoint -- only
        // a reset or CLEAR_FEATURE(HALT) clears it -- so it is carried
        // across transfers, not reset per transfer.
        if (in) msc.tgl_in = (uint8_t)Z_USBH_XS_TOGGLE(s);
        else msc.tgl_out = (uint8_t)Z_USBH_XS_TOGGLE(s);

        if (st == Z_USBH_ST_NAK) {
            // Nothing moved, nothing advanced: re-issue unchanged.
            if (++naks > MSC_SOFT_NAKS) {
                msc.last_len = done;
                return Z_USBH_MSC_ERR;
            }
            for (guard = 0; guard < 2000u; guard++) { }
            continue;
        }

        if (st != Z_USBH_ST_OK && st != Z_USBH_ST_SHORT) {
            msc.last_len = done;
            return st == Z_USBH_ST_STALL ? Z_USBH_MSC_STALL
                                         : Z_USBH_MSC_ERR;
        }

        done += (uint16_t)Z_USBH_XS_LEN(s);
        msc.last_len = done;

        // Short means the device had less to give, which ends the
        // transfer and is not an error.
        if (st == Z_USBH_ST_SHORT || done >= len) return Z_USBH_MSC_OK;
    }
}

// ---------------------------------------------------------------
// command block / status wrappers
// ---------------------------------------------------------------

static void put32le(uint32_t off, uint32_t v)
{
    z_usbh_wb(Z_USBH_BUF + off + 0, (uint8_t)v);
    z_usbh_wb(Z_USBH_BUF + off + 1, (uint8_t)(v >> 8));
    z_usbh_wb(Z_USBH_BUF + off + 2, (uint8_t)(v >> 16));
    z_usbh_wb(Z_USBH_BUF + off + 3, (uint8_t)(v >> 24));
}

static uint32_t get32le(uint32_t off)
{
    return (uint32_t)z_usbh_rb(Z_USBH_BUF + off) |
           ((uint32_t)z_usbh_rb(Z_USBH_BUF + off + 1) << 8) |
           ((uint32_t)z_usbh_rb(Z_USBH_BUF + off + 2) << 16) |
           ((uint32_t)z_usbh_rb(Z_USBH_BUF + off + 3) << 24);
}

static uint32_t get32be(uint32_t off)
{
    return ((uint32_t)z_usbh_rb(Z_USBH_BUF + off) << 24) |
           ((uint32_t)z_usbh_rb(Z_USBH_BUF + off + 1) << 16) |
           ((uint32_t)z_usbh_rb(Z_USBH_BUF + off + 2) << 8) |
           (uint32_t)z_usbh_rb(Z_USBH_BUF + off + 3);
}

/*
 * Run one SCSI command: CBW out, optional data, CSW in.
 *
 * cmd[] is the SCSI command block, cmd_len its length (6, 10 or 12).
 * dir is CBW_IN for a read, 0 for a write or no data.
 */
static int scsi_cmd(const uint8_t *cmd, int cmd_len, int dir,
                    uint32_t data_off, uint32_t data_len)
{
    uint32_t base = Z_USBH_BUF + MSC_OFF_CBW;
    int i, r;

    // -- CBW --
    put32le(MSC_OFF_CBW + 0, CBW_SIG);
    put32le(MSC_OFF_CBW + 4, ++msc.tag);
    put32le(MSC_OFF_CBW + 8, data_len);
    z_usbh_wb(base + 12, (uint8_t)dir);
    z_usbh_wb(base + 13, 0);              // LUN 0
    z_usbh_wb(base + 14, (uint8_t)cmd_len);
    for (i = 0; i < 16; i++)
        z_usbh_wb(base + 15 + i, i < cmd_len ? cmd[i] : 0);

    r = bulk_xfer(0, MSC_OFF_CBW, CBW_LEN);
    if (r != Z_USBH_MSC_OK) {
        if (msc_verbose)
            printf("usb msc: cmd %02x: CBW failed (%d, status %d)\n",
                   cmd[0], r, msc.last_status);
        return r;
    }

    // -- data --
    if (data_len) {
        r = bulk_xfer(dir == CBW_IN, data_off, (uint16_t)data_len);
        if (msc_verbose && (r != Z_USBH_MSC_OK || msc.last_len != data_len))
            printf("usb msc: cmd %02x: data %s %lu of %lu (r=%d, "
                   "status %d)\n", cmd[0], dir == CBW_IN ? "in" : "out",
                   (unsigned long)msc.last_len,
                   (unsigned long)data_len, r, msc.last_status);
        // A STALL on the data stage is recoverable: the device still
        // owes a CSW, and the spec says to clear the endpoint and read
        // it. Endpoint recovery is not implemented yet, so this is
        // reported rather than repaired.
        if (r == Z_USBH_MSC_ERR) return r;
    }

    // -- CSW --
    r = bulk_xfer(1, MSC_OFF_CSW, CSW_LEN);
    if (r != Z_USBH_MSC_OK) {
        if (msc_verbose)
            printf("usb msc: cmd %02x: CSW failed (%d, status %d)\n",
                   cmd[0], r, msc.last_status);
        return r;
    }

    if (get32le(MSC_OFF_CSW + 0) != CSW_SIG) {
        if (msc_verbose)
            printf("usb msc: cmd %02x: bad CSW signature %08lx\n",
                   cmd[0], (unsigned long)get32le(MSC_OFF_CSW + 0));
        return Z_USBH_MSC_ERR;
    }
    if (get32le(MSC_OFF_CSW + 4) != msc.tag) {
        if (msc_verbose)
            printf("usb msc: cmd %02x: tag %lu, expected %lu\n",
                   cmd[0], (unsigned long)get32le(MSC_OFF_CSW + 4),
                   (unsigned long)msc.tag);
        return Z_USBH_MSC_ERR;
    }
    if (msc_verbose && z_usbh_rb(Z_USBH_BUF + MSC_OFF_CSW + 12))
        printf("usb msc: cmd %02x: device reports status %d, "
               "residue %lu\n", cmd[0],
               z_usbh_rb(Z_USBH_BUF + MSC_OFF_CSW + 12),
               (unsigned long)get32le(MSC_OFF_CSW + 8));

    // bCSWStatus: 0 passed, 1 failed, 2 phase error.
    return z_usbh_rb(Z_USBH_BUF + MSC_OFF_CSW + 12) == 0 ?
           Z_USBH_MSC_OK : Z_USBH_MSC_FAIL;
}

// ---------------------------------------------------------------
// public
// ---------------------------------------------------------------

int z_usbh_msc_present(void)
{
    return msc.ready;
}

int z_usbh_msc_ready(void)
{
    return msc.started;
}

uint32_t z_usbh_msc_sectors(void)
{
    return msc.sectors;
}

/*
 * Claim a device whose configuration descriptor advertises
 * bulk-only SCSI mass storage, and find its two bulk endpoints.
 *
 * Returns non-zero if claimed. Called from the enumeration state
 * machine at bind time, with the SAVED descriptor copy -- never the
 * live packet buffer, which control transfers reuse.
 */
int z_usbh_msc_bind(uint8_t addr, uint8_t xa_flags, uint8_t port,
                    uint8_t mps0, const uint8_t *cfg, int cfg_len)
{
    int i = 0;
    int match = 0;

    (void)mps0;

    if (msc.ready) return 0;          // one drive for now

    memset(&msc, 0, sizeof(msc));

    while (i + 1 < cfg_len) {
        int len = cfg[i];
        int type = cfg[i + 1];

        if (len < 2) break;

        if (type == 0x04 && (i + 8) < cfg_len) {
            match = (cfg[i + 5] == MSC_CLASS &&
                     cfg[i + 6] == MSC_SUBCLASS_SCSI &&
                     cfg[i + 7] == MSC_PROTO_BOT);
        }

        if (type == 0x05 && match && (i + 6) < cfg_len) {
            // Bulk endpoints only; an MSC interface has exactly two.
            if ((cfg[i + 3] & 0x03) == 0x02) {
                if (cfg[i + 2] & 0x80) {
                    msc.ep_in = cfg[i + 2] & 0x0f;
                    msc.mps = cfg[i + 4] ? cfg[i + 4] : 64;
                } else {
                    msc.ep_out = cfg[i + 2] & 0x0f;
                }
            }
        }

        i += len;
    }

    if (!msc.ep_in || !msc.ep_out) return 0;

    msc.addr = addr;
    msc.xa_flags = xa_flags;
    msc.port = port;
    msc.ready = 1;

    printf("usb msc: addr %d ep in %d out %d, mps %d\n",
           msc.addr, msc.ep_in, msc.ep_out, msc.mps);

    return 1;
}

/*
 * Bring the drive up: wait for it to report ready, then learn its
 * geometry. Called once after bind, outside any filesystem lock.
 */
int z_usbh_msc_start(void)
{
    uint8_t cmd[16];
    int tries;

    if (!msc.ready) return Z_USBH_MSC_ERR;

    // TEST UNIT READY, repeatedly. A card reader with no card, or a
    // drive still spinning up, answers "not ready" for a while and
    // that is not an error -- it is the normal startup handshake.
    for (tries = 0; tries < 20; tries++) {
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = SCSI_TEST_UNIT_READY;
        if (scsi_cmd(cmd, 6, 0, 0, 0) == Z_USBH_MSC_OK) break;

        // REQUEST SENSE clears the check condition the failure just
        // raised; without it many devices keep failing forever.
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = SCSI_REQUEST_SENSE;
        cmd[4] = 18;
        scsi_cmd(cmd, 6, CBW_IN, MSC_OFF_DATA, 18);
    }
    if (tries >= 20) {
        printf("usb msc: unit never became ready\n");
        return Z_USBH_MSC_ERR;
    }

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = SCSI_READ_CAPACITY10;
    if (scsi_cmd(cmd, 10, CBW_IN, MSC_OFF_DATA, 8) != Z_USBH_MSC_OK) {
        printf("usb msc: READ CAPACITY failed\n");
        return Z_USBH_MSC_ERR;
    }

    // Both fields are BIG endian, unlike everything in the CBW.
    // The first is the LAST valid LBA, not the count.
    msc.sectors = get32be(MSC_OFF_DATA + 0) + 1;
    msc.sector_size = get32be(MSC_OFF_DATA + 4);

    printf("usb msc: %lu sectors of %lu bytes (%lu MB)\n",
           (unsigned long)msc.sectors, (unsigned long)msc.sector_size,
           (unsigned long)((msc.sectors / 2048) *
                           (msc.sector_size / 512)));

    // FatFs here is built for 512-byte sectors.
    if (msc.sector_size != 512) {
        printf("usb msc: unsupported sector size, drive ignored\n");
        msc.ready = 0;
        return Z_USBH_MSC_ERR;
    }

    msc.started = 1;
    return Z_USBH_MSC_OK;
}

int z_usbh_msc_read(uint32_t lba, uint8_t *dst, uint32_t count)
{
    uint8_t cmd[16];
    uint32_t n;
    int i;

    if (!msc.started) return Z_USBH_MSC_ERR;

    // One sector per command. The packet buffer holds one, and
    // multi-sector reads would need somewhere bigger to land --
    // see docs/usb_host.md on raising this.
    for (n = 0; n < count; n++) {
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = SCSI_READ10;
        cmd[2] = (uint8_t)((lba + n) >> 24);
        cmd[3] = (uint8_t)((lba + n) >> 16);
        cmd[4] = (uint8_t)((lba + n) >> 8);
        cmd[5] = (uint8_t)(lba + n);
        cmd[8] = 1;

        if (scsi_cmd(cmd, 10, CBW_IN, MSC_OFF_DATA, 512) !=
            Z_USBH_MSC_OK)
            return Z_USBH_MSC_ERR;

        for (i = 0; i < 512; i++)
            dst[n * 512 + i] = z_usbh_rb(Z_USBH_BUF + MSC_OFF_DATA + i);
    }

    return Z_USBH_MSC_OK;
}

int z_usbh_msc_write(uint32_t lba, const uint8_t *src, uint32_t count)
{
    uint8_t cmd[16];
    uint32_t n;
    int i;

    if (!msc.started) return Z_USBH_MSC_ERR;

    for (n = 0; n < count; n++) {
        for (i = 0; i < 512; i++)
            z_usbh_wb(Z_USBH_BUF + MSC_OFF_DATA + i, src[n * 512 + i]);

        memset(cmd, 0, sizeof(cmd));
        cmd[0] = SCSI_WRITE10;
        cmd[2] = (uint8_t)((lba + n) >> 24);
        cmd[3] = (uint8_t)((lba + n) >> 16);
        cmd[4] = (uint8_t)((lba + n) >> 8);
        cmd[5] = (uint8_t)(lba + n);
        cmd[8] = 1;

        if (scsi_cmd(cmd, 10, 0, MSC_OFF_DATA, 512) != Z_USBH_MSC_OK)
            return Z_USBH_MSC_ERR;
    }

    return Z_USBH_MSC_OK;
}
