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
// Consecutive transmission errors (bad CRC, no response) tolerated on
// a bulk IN before the transfer fails. USB 2.0 8.5.2 has the host
// retry a transaction up to three times; progress resets the count.
#define MSC_STRIKES          3

// Packet buffer regions. The per-port control scratch lives at 0x400
// and 0x600 (see usbh_hw.h), and the auto-poll slots at 0x380, so the
// low half is free for block traffic.
#define MSC_OFF_DATA        0x000u          // 512, one sector
#define MSC_OFF_CBW         0x200u          // 31
#define MSC_OFF_CSW         0x240u          // 13
#define MSC_OFF_SETUP       0x260u          // 8, the driver's own requests

static z_usbh_msc_t msc;

// Bumped by every bind and every unbind, and deliberately OUTSIDE msc
// so bind's memset does not reset it. A public entry point records it
// on the way in; if it has moved, the drive this operation started on
// is gone, even if a new one has since been bound in its place.
static volatile uint32_t msc_gen;
static uint32_t op_gen;

static int alive(void)
{
    return !msc.gone && op_gen == msc_gen;
}

// Bytes the most recent command's data stage actually moved. msc.last_len
// cannot serve: the CSW read that follows overwrites it.
static uint16_t msc_data_got;

// Chatty while the first sector read is being brought up. Every step
// of a failing command says which one it was, because "mount failed"
// on its own does not distinguish a CBW the device rejected from a
// data stage that stalled from a CSW that never arrived.
// Off now that reads and writes are confirmed on hardware; failures
// still return errors, and Reset Recovery always prints.
#ifdef USBH_DEBUG
static int msc_verbose = 0;
#else
// A constant, so every `if (msc_verbose) printf(...)` and its string is
// compiled out of a normal kernel.
#define msc_verbose 0
#endif

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
#ifdef USBH_DEBUG
// usbbench (fs/sdbench.c): SCSI commands issued, bulk requests made
// and the NAKs the device gave in them, since boot.
uint32_t z_usbh_msc_cmds;
uint32_t z_usbh_msc_naks, z_usbh_msc_xacts;
#endif

static int bulk_xfer(int in, uint32_t off, uint16_t len)
{
    uint32_t s;
    uint32_t guard;
    uint16_t done = 0;
    int naks = 0;
    int strikes = 0;
    uint8_t st;

    msc.last_len = 0;

    for (;;) {

        if (!alive()) return Z_USBH_MSC_ERR;

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

        // Every NAK the device gave this request, the hardware's own
        // retries included -- a device slow to supply data shows up here.
#ifdef USBH_DEBUG
        z_usbh_msc_naks += Z_USBH_XS_NAKS(s);
        z_usbh_msc_xacts++;
#endif
        st = (uint8_t)Z_USBH_XS_STATUS(s);
        msc.last_status = st;

        // The toggle persists for the life of a bulk endpoint -- only
        // a reset or CLEAR_FEATURE(HALT) clears it -- so it is carried
        // across transfers, not reset per transfer.
        if (in) msc.tgl_in = (uint8_t)Z_USBH_XS_TOGGLE(s);
        else msc.tgl_out = (uint8_t)Z_USBH_XS_TOGGLE(s);

        if (st == Z_USBH_ST_NAK) {
            // The NAK ended the sequence, but not necessarily before
            // anything moved: auto-continue may have landed several
            // packets first, and act_len and the toggle say how far it
            // got. Resume from there. This used to re-issue unchanged
            // -- "nothing moved" -- which re-requested the whole
            // length from offset 0 while the device carried on from
            // where it was; it ran out of data early and answered the
            // extra IN with its CSW, so a sector that paused mid-read
            // came back as (the rest) + 13 bytes and the real CSW read
            // then failed. tb_usb_msc_cosim's "OVER hardware budget"
            // case.
            if (Z_USBH_XS_LEN(s)) {
                done += (uint16_t)Z_USBH_XS_LEN(s);
                msc.last_len = done;
                naks = 0;           // progress, so not a stuck device
            }
            if (++naks > MSC_SOFT_NAKS) {
                msc.last_len = done;
                return Z_USBH_MSC_ERR;
            }
            for (guard = 0; guard < 2000u; guard++) { }
            continue;
        }

        // A transmission error on an IN is retried, not failed. The
        // engine stops on the bad packet WITHOUT advancing: act_len
        // counts only the packets before it, and the toggle it
        // reports is still the one the device will resend with,
        // because the host sent no ACK. So this resumes exactly like
        // a NAK.
        //
        // Failing instead was far worse than losing one sector: the
        // device was still mid-data-phase, so the CSW read that
        // follows took sector bytes as a status wrapper and the
        // stream stayed out of step for every later command.
        // tb_usb_msc_cosim's bit-error cases.
        //
        // OUT is not retried here. A lost handshake there is resolved
        // by the device ignoring a repeated toggle, and nothing yet
        // exercises that path.
        if (in && (st == Z_USBH_ST_CRCERR || st == Z_USBH_ST_TIMEOUT)) {
            if (Z_USBH_XS_LEN(s)) {
                done += (uint16_t)Z_USBH_XS_LEN(s);
                msc.last_len = done;
                strikes = 0;
            }
            if (++strikes <= MSC_STRIKES) continue;
        }

        if (st != Z_USBH_ST_OK && st != Z_USBH_ST_SHORT) {
            msc.last_len = done;
            return st == Z_USBH_ST_STALL ? Z_USBH_MSC_STALL
                                         : Z_USBH_MSC_ERR;
        }

        done += (uint16_t)Z_USBH_XS_LEN(s);
        msc.last_len = done;
        strikes = 0;

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

// ---------------------------------------------------------------
// the driver's own control requests, and Reset Recovery
// ---------------------------------------------------------------

/*
 * One transaction on the engine, run to completion. Returns 0 and
 * leaves *sp untouched if the engine never finishes, which only a
 * hardware fault would cause. The caller holds the bus.
 */
static int xact_run(uint8_t pid, uint8_t ep, int tgl, uint32_t off,
                    uint16_t len, uint32_t *sp)
{
    uint32_t s = 0;
    uint32_t guard;

    z_usbh_wr(Z_USBH_XACT_A,
              Z_USBH_XA_ADDR(msc.addr) | Z_USBH_XA_ENDP(ep) |
              Z_USBH_XA_PID(pid) | Z_USBH_XA_PORT(msc.port) |
              Z_USBH_XA_MPS(msc.mps0) |
              ((uint32_t)msc.xa_flags << 13) |
              (tgl ? Z_USBH_XA_TOGGLE : 0));
    z_usbh_wr(Z_USBH_XACT_B,
              Z_USBH_XB_OFF(off) | Z_USBH_XB_LEN(len) |
              Z_USBH_XB_NAK(MSC_NAK_BUDGET) | Z_USBH_XB_START);
    for (guard = 0; guard < 2000000u; guard++) {
        s = z_usbh_rd(Z_USBH_XACT_S);
        if (!(s & Z_USBH_XS_PENDING)) break;
    }
    if (s & Z_USBH_XS_PENDING) return 0;
    *sp = s;
    return 1;
}

/*
 * A control request with no data stage, on endpoint 0: SETUP, then a
 * zero-length IN for status. Only two are ever sent -- CLEAR_FEATURE
 * and the class reset -- and neither carries data.
 */
static int ctrl_nodata(uint8_t bm, uint8_t req, uint16_t val,
                       uint16_t idx)
{
    uint32_t base = Z_USBH_BUF + MSC_OFF_SETUP;
    uint32_t s, guard;
    uint8_t st;
    int naks = 0;

    z_usbh_wb(base + 0, bm);
    z_usbh_wb(base + 1, req);
    z_usbh_wb(base + 2, (uint8_t)val);
    z_usbh_wb(base + 3, (uint8_t)(val >> 8));
    z_usbh_wb(base + 4, (uint8_t)idx);
    z_usbh_wb(base + 5, (uint8_t)(idx >> 8));
    z_usbh_wb(base + 6, 0);
    z_usbh_wb(base + 7, 0);

    if (!alive()) return Z_USBH_MSC_ERR;
    if (!xact_run(Z_USBH_PID_SETUP, 0, 0, MSC_OFF_SETUP, 8, &s))
        return Z_USBH_MSC_ERR;
    if (Z_USBH_XS_STATUS(s) != Z_USBH_ST_OK) return Z_USBH_MSC_ERR;

    // Status stage, always DATA1. A device may NAK it while it carries
    // the request out -- a reset can take a while.
    for (;;) {
        if (!alive()) return Z_USBH_MSC_ERR;
        if (!xact_run(Z_USBH_PID_IN, 0, 1, MSC_OFF_SETUP, 0, &s))
            return Z_USBH_MSC_ERR;
        st = (uint8_t)Z_USBH_XS_STATUS(s);
        if (st != Z_USBH_ST_NAK || ++naks > MSC_SOFT_NAKS) break;
        for (guard = 0; guard < 2000u; guard++) { }
    }
    if (st == Z_USBH_ST_OK || st == Z_USBH_ST_SHORT) return Z_USBH_MSC_OK;
    return st == Z_USBH_ST_STALL ? Z_USBH_MSC_STALL : Z_USBH_MSC_ERR;
}

/*
 * CLEAR_FEATURE(ENDPOINT_HALT). ep carries the direction bit, 0x80 for
 * IN. Clearing a halt resets that endpoint's data toggle to DATA0 on
 * the device, so ours follows -- whether or not the endpoint was
 * actually halted, which is why this is also safe as a precaution.
 */
static int clear_halt(uint8_t ep)
{
    int r = ctrl_nodata(0x02, 0x01, 0x0000, ep);
    if (r == Z_USBH_MSC_OK) {
        if (ep & 0x80) msc.tgl_in = 0;
        else msc.tgl_out = 0;
    }
    return r;
}

/*
 * Bulk-Only Mass Storage Reset Recovery, BOT 5.3.4: the class reset,
 * then clear the halt on both bulk endpoints.
 *
 * This is the only way back into step once the three-phase sequence
 * has broken -- a CSW that is missing, malformed or reports a phase
 * error, a transfer that failed part-way through a data phase. Before
 * it existed, any of those left every later command reading the
 * previous one's leftovers.
 */
static int reset_recovery(void)
{
    int r1, r2, r3;

    msc.resets++;
    r1 = ctrl_nodata(0x21, 0xff, 0x0000, msc.iface);
    r2 = clear_halt((uint8_t)(0x80 | msc.ep_in));
    r3 = clear_halt(msc.ep_out);

    // Printed whatever msc_verbose says: this is rare, and when it
    // happens it is the first thing anyone chasing a fault wants.
    printf("usb msc: reset recovery %s (%d %d %d)\n",
           (r1 | r2 | r3) == Z_USBH_MSC_OK ? "ok" : "FAILED", r1, r2, r3);

    return (r1 | r2 | r3) == Z_USBH_MSC_OK ? Z_USBH_MSC_OK
                                            : Z_USBH_MSC_ERR;
}

// Repair the transport if the device is still here to repair. A pulled
// drive usually fails its transfer before the ISR's detach path has run
// and marked it gone, so the port is asked directly: recovering a
// device that is not there fails anyway, and says "FAILED" in the log
// for what was only an unplug.
static void recover(void)
{
    uint32_t ps = Z_USBH_PS(z_usbh_rd(Z_USBH_PORTSTAT), msc.port);

    if (!(ps & Z_USBH_PS_CONNECTED)) return;
    if (alive()) reset_recovery();
}

/*
 * Run one SCSI command: CBW out, optional data, CSW in.
 *
 * cmd[] is the SCSI command block, cmd_len its length (6, 10 or 12).
 * dir is CBW_IN for a read, 0 for a write or no data.
 *
 * Holds the transaction engine for the whole command; see usbh.h.
 *
 * Returns OK, FAIL (the device reported the command failed; the
 * transport is in step) or ERR (it was not, or the device is gone --
 * recovery has already been attempted).
 */
static int scsi_cmd_body(const uint8_t *cmd, int cmd_len, int dir,
                         uint32_t data_off, uint32_t data_len)
{
    uint32_t base = Z_USBH_BUF + MSC_OFF_CBW;
    int i, r;
    int data_r = Z_USBH_MSC_OK;
    uint8_t status;

    msc_data_got = 0;

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
        // A device that stalls or drops a CBW has not accepted the
        // command; BOT 6.6.1 has the host reset rather than guess.
        if (msc_verbose)
            printf("usb msc: cmd %02x: CBW failed (%d, status %d)\n",
                   cmd[0], r, msc.last_status);
        recover();
        return Z_USBH_MSC_ERR;
    }

    // -- data --
    if (data_len) {
        r = bulk_xfer(dir == CBW_IN, data_off, (uint16_t)data_len);
        msc_data_got = msc.last_len;
        data_r = r;
        if (msc_verbose && (r != Z_USBH_MSC_OK || msc.last_len != data_len))
            printf("usb msc: cmd %02x: data %s %lu of %lu (r=%d, "
                   "status %d)\n", cmd[0], dir == CBW_IN ? "in" : "out",
                   (unsigned long)msc.last_len,
                   (unsigned long)data_len, r, msc.last_status);

        if (r == Z_USBH_MSC_STALL) {
            // BOT 6.7.2: the device ended the data phase early by
            // halting the pipe -- a read past the end of the medium, a
            // card reader with no card. That is the command failing,
            // not the transport: clear the halt, and the CSW that
            // follows says why.
            if (clear_halt(dir == CBW_IN ? (uint8_t)(0x80 | msc.ep_in)
                                         : msc.ep_out) != Z_USBH_MSC_OK) {
                recover();
                return Z_USBH_MSC_ERR;
            }
        } else if (r != Z_USBH_MSC_OK) {
            // Failed part-way through the data phase. The device is
            // still in it, so a CSW read now would take data as status
            // -- which is how one bad packet used to put every later
            // command out of step. Reset instead.
            recover();
            return Z_USBH_MSC_ERR;
        }
    }

    // -- CSW --
    r = bulk_xfer(1, MSC_OFF_CSW, CSW_LEN);
    if (r == Z_USBH_MSC_STALL) {
        // BOT 6.7.2 again: a halted IN pipe at the CSW is cleared and
        // the CSW read once more.
        if (clear_halt((uint8_t)(0x80 | msc.ep_in)) == Z_USBH_MSC_OK)
            r = bulk_xfer(1, MSC_OFF_CSW, CSW_LEN);
    }
    if (r != Z_USBH_MSC_OK || msc.last_len != CSW_LEN) {
        if (msc_verbose)
            printf("usb msc: cmd %02x: CSW failed (%d, status %d, "
                   "%u bytes)\n", cmd[0], r, msc.last_status,
                   msc.last_len);
        recover();
        return Z_USBH_MSC_ERR;
    }

    // BOT 6.3: a CSW is valid only if it is 13 bytes, carries the
    // signature, and echoes this command's tag. Anything else means the
    // two ends disagree about where they are.
    if (get32le(MSC_OFF_CSW + 0) != CSW_SIG ||
        get32le(MSC_OFF_CSW + 4) != msc.tag) {
        if (msc_verbose)
            printf("usb msc: cmd %02x: invalid CSW (sig %08lx, tag %lu, "
                   "expected %lu)\n", cmd[0],
                   (unsigned long)get32le(MSC_OFF_CSW + 0),
                   (unsigned long)get32le(MSC_OFF_CSW + 4),
                   (unsigned long)msc.tag);
        recover();
        return Z_USBH_MSC_ERR;
    }

    status = z_usbh_rb(Z_USBH_BUF + MSC_OFF_CSW + 12);
    if (msc_verbose && status)
        printf("usb msc: cmd %02x: device reports status %d, "
               "residue %lu\n", cmd[0], status,
               (unsigned long)get32le(MSC_OFF_CSW + 8));

    // bCSWStatus: 0 passed, 1 failed, 2 phase error. A phase error is
    // the device saying it lost track; BOT 6.7 requires a reset.
    if (status == 2) {
        recover();
        return Z_USBH_MSC_ERR;
    }
    if (status != 0) return Z_USBH_MSC_FAIL;

    // Passed, but a stalled data phase still did not deliver.
    if (data_r != Z_USBH_MSC_OK) return Z_USBH_MSC_FAIL;
    return Z_USBH_MSC_OK;
}

static int scsi_cmd(const uint8_t *cmd, int cmd_len, int dir,
                    uint32_t data_off, uint32_t data_len)
{
#ifdef USBH_DEBUG
    z_usbh_msc_cmds++;
#endif
    int r;

    if (!alive()) return Z_USBH_MSC_ERR;
    if (!z_usbh_bus_reserve()) {
        printf("usb msc: bus busy, cmd %02x not sent\n", cmd[0]);
        return Z_USBH_MSC_ERR;
    }
    // Checked again now the bus is ours: enumeration cannot bind a new
    // device while it is held, so this answer stays true for the whole
    // command.
    if (!alive()) {
        z_usbh_bus_release();
        return Z_USBH_MSC_ERR;
    }
    r = scsi_cmd_body(cmd, cmd_len, dir, data_off, data_len);
    z_usbh_bus_release();
    return r;
}

// ---------------------------------------------------------------
// public
// ---------------------------------------------------------------

void z_usbh_msc_unbind(void)
{
    // ISR context, from usbh.c's detach path. Mark, do not clear: a
    // command may be running in process context right now, and it is
    // the one that notices and stops -- see alive().
    if (!msc.ready && !msc.started) return;
    msc.gone = 1;
    msc.ready = 0;
    msc.started = 0;
    msc_gen++;
    printf("usb msc: device removed\n");
}

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

    uint8_t iface = 0;

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
            if (match) iface = cfg[i + 2];
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
    msc.iface = iface;
    msc.mps0 = mps0 ? mps0 : 8;
    msc_gen++;
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
    op_gen = msc_gen;

    // TEST UNIT READY, repeatedly. A card reader with no card, or a
    // drive still spinning up, answers "not ready" for a while and
    // that is not an error -- it is the normal startup handshake.
    for (tries = 0; tries < 20; tries++) {
        if (!alive()) return Z_USBH_MSC_ERR;
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
    op_gen = msc_gen;

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

        // A sector is 512 bytes or it is a failure. Fewer leaves the
        // tail of the landing area holding the previous command's
        // bytes, and handing that to FatFs as file content is worse
        // than an error it can report.
        if (msc_data_got != 512) {
            if (msc_verbose)
                printf("usb msc: lba %lu: short sector, %u bytes\n",
                       (unsigned long)(lba + n), msc_data_got);
            return Z_USBH_MSC_ERR;
        }

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
    op_gen = msc_gen;

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
