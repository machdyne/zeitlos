/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host stack -- core.
 *
 * Port state, control transfers, enumeration, address allocation and
 * driver binding. See docs/usb_host.md.
 *
 * Everything here is a state machine that advances one step per call
 * and returns. Read usbh.h's header for why that is not negotiable.
 */

#include <stdio.h>
#include <string.h>

#include "usbh.h"
#include "usbh_hw.h"
#include "usbh_msc.h"
#ifdef Z_USBH_COSIM
// rtl/tb/cosim builds this file for the host, where the kernel's
// headers neither exist nor mean anything. The two things it borrows
// from the kernel are declared directly instead.
extern volatile uint32_t z_kernel_ticks;
int z_soc_has_feature2(uint32_t bit);
#define Z_FEATURE2_USB_HOST (1u << 4)
#else
#include "../kernel.h"          // z_kernel_ticks
#include "../../common/zsoc.h"
#endif

// -- standard requests, USB 2.0 chapter 9 --

#define REQ_GET_DESCRIPTOR  0x06
#define REQ_SET_ADDRESS     0x05
#define REQ_SET_CONFIG      0x09

#define DESC_DEVICE         0x01
#define DESC_CONFIG         0x02
#define DESC_INTERFACE      0x04
#define DESC_ENDPOINT       0x05

#define DIR_IN              0x80

// Hardware NAK retries per transaction: a few, not the maximum.
//
// The engine retries a NAKed transaction back to back with only the
// inter-packet gap between attempts -- about 19 us at full speed. A
// probe capture of a hub showed us doing exactly that, IN / NAK / IN /
// NAK, and then the hub stopping mid-sequence: the failure was
// reported as "TIMEOUT (no response) (9 naks)", meaning it answered
// nine times and then went quiet.
//
// Real hosts do not hammer an endpoint like that; they retry a NAKed
// control transfer in a LATER FRAME. So the hardware gets a small
// budget for a device that is momentarily busy, and anything longer is
// paced by the driver at one kernel tick per attempt -- see
// CTRL_SOFT_NAKS.
//
// This was 15, the field maximum, chosen when the only thing on the
// other end was a device model that never NAKed at all.
#define Z_USBH_NAK_BUDGET   3

// HID class requests, addressed to an interface.
#define HID_OUT_IFACE       0x21
#define REQ_SET_IDLE        0x0a
#define REQ_SET_PROTOCOL    0x0b

// -- control transfer engine --
//
// A control transfer is three transactions: SETUP, an optional data
// stage, and a status stage whose direction is the opposite of the
// data. Each is one call's worth of work.

#define CS_IDLE     0
#define CS_SETUP    1
#define CS_DATA     2
#define CS_STATUS   3
#define CS_DONE     4
#define CS_ERROR    5
#define CS_FINAL    6
#define CS_DATA_RUN 7

// Which stage's RESULT was being read when the transfer failed.
// Recorded at the point of failure, because ctrl_state has already
// moved to CS_ERROR by the time the caller sees it -- capturing it
// there just reports "errored", which is what the previous attempt
// did and it was useless.
#define CE_NONE     0
#define CE_SETUP    1
#define CE_DATA     2
#define CE_STATUS   3

// -- enumeration states --

#define E_EMPTY         0
#define E_DEBOUNCE      1
#define E_RESET         2
#define E_RESET_WAIT    3
#define E_DESC8         4
#define E_SET_ADDR      5
#define E_ADDR_SETTLE   6
#define E_DESC18        7
#define E_CONFIG9       8
#define E_CONFIG_ALL    9
#define E_SET_CONFIG    10
#define E_SET_PROTO     11
#define E_SET_IDLE      12
#define E_BIND          13
#define E_RUNNING       14
#define E_FAILED        15

typedef struct {
    uint8_t state;
    uint8_t port;
    uint8_t addr;
    uint8_t mps0;
    uint8_t xa_flags;       // lowspeed / inverted / use_pre, prebuilt
    uint8_t cls;
    uint8_t retries;
    uint8_t cfg_len;
    int8_t blk;             // compat block claimed, -1 for none
    int8_t iface;           // HID interface number, -1 for none
    // Where enumeration died and what the bus said, kept across the
    // retry so it survives to be reported. "FAILED" on its own is not
    // a diagnosis -- a device that never answers, one that NAKs past
    // the retry budget and one that STALLs are three different
    // problems and they all reach E_FAILED.
    uint8_t fail_state;
    uint8_t fail_status;
    // Which STAGE of the control transfer died. "get-desc8" is three
    // transactions -- SETUP, an IN data stage and an OUT status stage
    // -- and they fail for different reasons. Without this the three
    // are indistinguishable and the useful half of the report is
    // missing.
    uint8_t fail_ctrl;
    uint8_t fail_naks;
    // Every attempt, not just the last one. We have been reasoning
    // from the residue of attempt four while the cumulative wire
    // counters described all four, and the two disagreed -- which is
    // itself the finding: the attempts are not alike. A device that
    // answers once and then goes quiet has changed state, and that
    // points somewhere completely different from random failure.
    uint8_t att_state[4];
    uint8_t att_ctrl[4];
    uint8_t att_status[4];
    uint8_t att_naks[4];
    // What the failing transaction actually reported, beyond its
    // status. A capture has now proved the wire is correct through a
    // NAK/NAK/NAK/DATA1/ACK sequence, so what is left is the
    // SOFTWARE retry path -- and these are the two values it depends
    // on: how much the transaction moved before it stopped, and which
    // toggle it left behind.
    uint8_t att_alen[4];
    uint8_t att_tgl[4];
    uint8_t att_want[4];
    uint8_t att_n;
    // What we learned about the device before it went wrong. A STALL
    // on SET_CONFIGURATION is what a device says when it dislikes the
    // request, so the value we sent is the first thing to check --
    // and it comes from a descriptor byte we have never looked at.
    uint8_t d_vid_lo, d_vid_hi, d_pid_lo, d_pid_hi;
    uint8_t d_class, d_nconf;
    uint8_t cfg_val, cfg_nif, cfg_attr;
    uint8_t got_desc, got_cfg;
    // The first 32 bytes of the configuration descriptor, kept so
    // lsusb can show why the HID walk did or did not claim a device.
    // A keyboard that enumerates cleanly and then binds to nothing is
    // a descriptor-walk problem, and guessing at it from the outside
    // has already cost more than storing 32 bytes.
    uint8_t cfg_raw[64];
    uint8_t cfg_rawn;
    // The address allocated in E_DESC8, held until SET_ADDRESS's
    // status stage lands. This used to borrow cfg_len, which is the
    // configuration descriptor's length -- harmless when enumeration
    // succeeds first time, and wrong on every retry, because the
    // stashed address survived into the descriptor walk as a bogus
    // length. lsusb showed it as "cfg desc (64 of 3 bytes)".
    uint8_t pend_addr;

    // -- this device's control transfer engine --
    uint8_t ctrl_state;
    uint8_t ctrl_err_stage;
    uint8_t ctrl_soft_naks;
    uint8_t ctrl_tgl;
    uint8_t ctrl_pend;
    uint8_t ctrl_dir_in;
    uint16_t ctrl_xferred;
    uint16_t ctrl_len;
    uint32_t ctrl_delay_until;
    uint32_t deadline;      // kernel ticks, 0 for none
} z_usbh_dev_t;

static z_usbh_dev_t devs[Z_USBH_MAX_PORTS];

// The control transfer engine's state now lives in z_usbh_dev_t, one
// instance per device, reached through `ce` inside ctrl_step().
//
// It used to be a single shared set of globals, and both root ports
// drove it at once: each judged results belonging to the other's
// transfer, with the wrong direction and the wrong length. On hardware
// that produced a STALL on the data stage and babble on the status
// stage, and every device worked perfectly when it was the only one
// plugged in.
//
// A stopgap serialised ALL enumeration so only one
// device is ever mid-transfer. That is fine for two root ports, where
// the cost is a few hundred milliseconds once per plug event, and
// wrong for hubs, where several devices arrive behind one port and
// enumerating them strictly one at a time gets slow.
//
// With the state per-device, the only thing that still has to be
// serialised is ADDRESS ZERO: a freshly reset device answers on 0, so
// two of them mid-enumeration would both reply to the same token.
// Software-paced NAK retry. The hardware retries a NAKed transaction
// back-to-back with only the inter-packet gap between attempts, so
// its whole budget burns in under a millisecond -- and a slow device
// is entitled to NAK for far longer than that while it prepares an
// answer. A real host retries in LATER FRAMES. Port 1's mouse made
// the point on real hardware: it ACKed the SETUP and then NAKed the
// descriptor IN sixteen times in a row, which is not refusal, it is
// "not yet" said faster than it could possibly become "yes".
//
// So on NAK the stage is re-issued from here, two kernel ticks
// (~2.7 ms) apart, up to CTRL_SOFT_NAKS times -- a ~160 ms window,
// against the sub-millisecond one the hardware budget alone gives.
// How much of the data stage the device has already handed over across
// all the re-issues so far. A NAK retry must RESUME, not restart.
// ~1.4 s of patience, not ~85 ms.
//
// A NAK is not a failure. It means "not yet", and USB lets a device
// say it for as long as it is busy: a hub bringing up its downstream
// ports, a card reader looking for a card, a stick finishing an
// internal write. A probe capture of a hub that would not enumerate
// showed a clean IN / NAK / IN / NAK sequence with perfect CRCs all
// the way to the point where we gave up -- the device was talking the
// whole time, and we stopped listening.
//
// Costs nothing when devices are prompt: a NAK only consumes the
// transaction that was going to happen anyway, and the retry is paced
// at one kernel tick, so this is ~1000 transactions spread over a
// second rather than a burst.
#define CTRL_SOFT_NAKS 1000

// One kernel tick (~1.4 ms), not two.
//
// A low-speed device suspends after 3 ms of bus inactivity, and with
// the frame timer off there are no keepalives to hold it awake -- so a
// 2.7 ms retry gap sits right on that threshold. This is a suspect I
// introduced myself with the original pacing, so it is worth removing
// rather than reasoning about.
#define CTRL_NAK_TICKS 1
static uint8_t addr_bitmap;     // addresses 1..7
static uint8_t usbh_present;
static uint8_t usbh_any_ready;

// Forward declaration; the HID driver lives in usbh_hid.c and is the
// only consumer of a completed enumeration so far.
int z_usbh_hid_bind(int slot, uint8_t addr, uint8_t xa_flags,
                    uint8_t port, const uint8_t *cfg, int cfg_len);
int z_usbh_hid_probe(const uint8_t *cfg, int cfg_len);
void z_usbh_hid_release(int blk);

// ---------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------

static uint32_t usbh_ticks(void)
{
    // The kernel's ~732 Hz tick, used only for coarse timeouts.
    // Nothing here needs better resolution than a millisecond or two,
    // and nothing here may wait for one.
    return z_kernel_ticks;
}

static int xact_pending(void)
{
    return (z_usbh_rd(Z_USBH_XACT_S) & Z_USBH_XS_PENDING) != 0;
}

static void xact_start(uint8_t pid, uint8_t addr, uint8_t endp,
                       uint8_t xa_flags, uint8_t port, int toggle,
                       int autocont, uint8_t mps, uint16_t off,
                       uint16_t len, uint8_t nak)
{
    uint32_t a = Z_USBH_XA_ADDR(addr) | Z_USBH_XA_ENDP(endp) |
                 Z_USBH_XA_PID(pid) | Z_USBH_XA_PORT(port) |
                 Z_USBH_XA_MPS(mps);

    // xa_flags carries LOWSPEED / INVERTED / USE_PRE already combined,
    // because the three of them are decided once when the device is
    // found and must never be recomputed from "is it low speed" alone.
    // A low-speed device behind a hub is LOWSPEED|USE_PRE and NOT
    // INVERTED, which is the case that silently fails if anything
    // re-derives it. See docs/usb_host.md.
    a |= (uint32_t)xa_flags << 13;

    if (toggle) a |= Z_USBH_XA_TOGGLE;
    if (autocont) a |= Z_USBH_XA_AUTOCONT;

    z_usbh_wr(Z_USBH_XACT_A, a);
    z_usbh_wr(Z_USBH_XACT_B, Z_USBH_XB_OFF(off) | Z_USBH_XB_LEN(len) |
                             Z_USBH_XB_NAK(nak) | Z_USBH_XB_START);
}

static void put_setup(int d, uint8_t bmType, uint8_t bReq, uint16_t wVal,
                      uint16_t wIdx, uint16_t wLen)
{
    uint32_t p = Z_USBH_BUF + Z_USBH_OFF_PSETUP(d);
    z_usbh_wb(p + 0, bmType);
    z_usbh_wb(p + 1, bReq);
    z_usbh_wb(p + 2, (uint8_t)wVal);
    z_usbh_wb(p + 3, (uint8_t)(wVal >> 8));
    z_usbh_wb(p + 4, (uint8_t)wIdx);
    z_usbh_wb(p + 5, (uint8_t)(wIdx >> 8));
    z_usbh_wb(p + 6, (uint8_t)wLen);
    z_usbh_wb(p + 7, (uint8_t)(wLen >> 8));
}

// ---------------------------------------------------------------
// control transfer
// ---------------------------------------------------------------

static void ctrl_begin(int d, uint8_t bmType, uint8_t bReq,
                       uint16_t wVal, uint16_t wIdx, uint16_t wLen)
{
    z_usbh_dev_t *ce = &devs[d];

    put_setup(d, bmType, bReq, wVal, wIdx, wLen);
    ce->ctrl_err_stage = CE_NONE;
    ce->ctrl_soft_naks = 0;
    ce->ctrl_xferred = 0;
    ce->ctrl_pend = 0;
    ce->ctrl_delay_until = usbh_ticks();
    ce->ctrl_dir_in = (bmType & DIR_IN) ? 1 : 0;
    ce->ctrl_len = wLen;
    ce->ctrl_state = CS_SETUP;
}

// One step. Returns the new state so callers can test CS_DONE /
// CS_ERROR without reaching into the variable.
static int ctrl_step(int d)
{
    z_usbh_dev_t *dv = &devs[d];
    z_usbh_dev_t *ce = dv;
    uint32_t s;
    uint8_t st;

    if (ce->ctrl_state == CS_DONE || ce->ctrl_state == CS_ERROR)
        return ce->ctrl_state;

    if (xact_pending()) return ce->ctrl_state;
    if ((int32_t)(usbh_ticks() - ce->ctrl_delay_until) < 0)
        return ce->ctrl_state;

    s = z_usbh_rd(Z_USBH_XACT_S);
    st = (uint8_t)Z_USBH_XS_STATUS(s);

    switch (ce->ctrl_state) {

    case CS_SETUP:
        // The SETUP token and its 8 data bytes always go out as DATA0.
        xact_start(Z_USBH_PID_SETUP, dv->addr, 0, dv->xa_flags,
                   dv->port, 0, 0, dv->mps0,
                   Z_USBH_OFF_PSETUP(d), 8, Z_USBH_NAK_BUDGET);
        ce->ctrl_state = ce->ctrl_len ? CS_DATA : CS_STATUS;
        break;

    case CS_DATA:
        if (st != Z_USBH_ST_OK) {
            ce->ctrl_err_stage = CE_SETUP;
            ce->ctrl_state = CS_ERROR;
            break;
        }
        // -- one packet per transaction, paced here --
        //
        // Auto-continue was doing this in hardware, and on a device
        // that NAKs partway the accounting between the two came apart:
        // a 59-byte descriptor came back with exactly one 8-byte
        // packet missing at offset 16 and everything after it shifted
        // 8 bytes early. That is one packet's worth of buffer pointer
        // failing to advance, so the following packet overwrote it.
        //
        // Software knows exactly how much it has, where the next
        // packet goes and which toggle it expects, and a NAK simply
        // re-issues the same request unchanged. A control descriptor
        // read is a handful of packets, so the cost of a round trip
        // through the driver per packet is irrelevant -- and the mouse
        // only worked before because it never NAKed mid-descriptor.
        ce->ctrl_xferred = 0;
        ce->ctrl_tgl = 1;
        ce->ctrl_pend = 0;
        ce->ctrl_state = CS_DATA_RUN;
        /* fall through */

    case CS_DATA_RUN:
        // ctrl_pend distinguishes "a packet is outstanding, judge it"
        // from "nothing issued yet". Deriving that from ctrl_xferred
        // does not work: a first packet returning zero bytes looks
        // identical to not having started.
        if (ce->ctrl_pend) {
            uint16_t got = (uint16_t)Z_USBH_XS_LEN(s);
            ce->ctrl_pend = 0;
            if (st == Z_USBH_ST_NAK) {
                if (ce->ctrl_soft_naks >= CTRL_SOFT_NAKS) {
                    // Exhausted, not silent. dev_fail() reads the
                    // transaction status, which is ST_NAK here, so the
                    // report says so -- it used to come out as
                    // "TIMEOUT (no response)", which reads as silence
                    // and is the opposite of what happened.
                    ce->ctrl_err_stage = CE_DATA;
                    ce->ctrl_state = CS_ERROR;
                    break;
                }
                // A NAK moves nothing: same offset, same toggle.
                ce->ctrl_soft_naks++;
                ce->ctrl_delay_until = usbh_ticks() + CTRL_NAK_TICKS;
            } else if (st != Z_USBH_ST_OK && st != Z_USBH_ST_SHORT) {
                ce->ctrl_err_stage = CE_DATA;
                ce->ctrl_state = CS_ERROR;
                break;
            } else {
                ce->ctrl_xferred += got;
                ce->ctrl_tgl ^= 1;
                // A short packet ends the stage: the device has no
                // more to give, which is success, not truncation.
                if (got < dv->mps0 || ce->ctrl_xferred >= ce->ctrl_len) {
                    ce->ctrl_state = CS_STATUS;
                    break;
                }
            }
        }
        {
            uint16_t want = ce->ctrl_len - ce->ctrl_xferred;
            if (want > dv->mps0) want = dv->mps0;
            xact_start(ce->ctrl_dir_in ? Z_USBH_PID_IN : Z_USBH_PID_OUT,
                       dv->addr, 0, dv->xa_flags, dv->port, ce->ctrl_tgl, 0,
                       dv->mps0, Z_USBH_OFF_PCTRL(d) + ce->ctrl_xferred, want,
                       Z_USBH_NAK_BUDGET);
            ce->ctrl_pend = 1;
        }
        ce->ctrl_state = CS_DATA_RUN;
        break;

    case CS_STATUS:
        // ST_SHORT is a success: the device had less to give than we
        // asked for, which is normal and not an error.
        if (st == Z_USBH_ST_NAK && ce->ctrl_len &&
            ce->ctrl_soft_naks < CTRL_SOFT_NAKS) {
            // RESUME the data stage, do not restart it.
            //
            // Auto-continue may already have moved several packets
            // before the hardware's NAK budget ran out, and the device
            // carries on from where IT left off. Re-issuing the whole
            // stage from offset zero asks for bytes it has already
            // sent, and the two streams slide past each other -- which
            // is how an 18-byte descriptor came back as plausible
            // nonsense rather than as an error.
            //
            // act_len is what this transaction moved; the toggle it
            // left behind is in the same register, and a NAK does not
            // advance it.
            ce->ctrl_xferred += (uint16_t)Z_USBH_XS_LEN(s);
            if (ce->ctrl_xferred >= ce->ctrl_len) {
                ce->ctrl_state = CS_STATUS;
                break;
            }
            ce->ctrl_soft_naks++;
            ce->ctrl_delay_until = usbh_ticks() + CTRL_NAK_TICKS;
            xact_start(ce->ctrl_dir_in ? Z_USBH_PID_IN : Z_USBH_PID_OUT,
                       dv->addr, 0, dv->xa_flags, dv->port,
                       Z_USBH_XS_TOGGLE(s), 1, dv->mps0,
                       Z_USBH_OFF_PCTRL(d) + ce->ctrl_xferred,
                       ce->ctrl_len - ce->ctrl_xferred, Z_USBH_NAK_BUDGET);
            break;
        }
        if (st != Z_USBH_ST_OK && st != Z_USBH_ST_SHORT) {
            // With no data stage this is still the SETUP's result.
            ce->ctrl_err_stage = ce->ctrl_len ? CE_DATA : CE_SETUP;
            ce->ctrl_state = CS_ERROR;
            break;
        }
        // The status stage is a zero-length packet in the OPPOSITE
        // direction to the data, always DATA1.
        xact_start(ce->ctrl_dir_in ? Z_USBH_PID_OUT : Z_USBH_PID_IN,
                   dv->addr, 0, dv->xa_flags, dv->port, 1, 0,
                   dv->mps0, Z_USBH_OFF_PCTRL(d), 0,
                   Z_USBH_NAK_BUDGET);
        ce->ctrl_state = CS_FINAL;
        break;

    case CS_FINAL:
        // The status stage's own result was never checked before this
        // -- the engine declared the transfer done the moment it
        // LAUNCHED the final packet. A device that NAKed or never
        // answered the status stage was therefore recorded as a
        // success, and enumeration carried on to the next request
        // against a device that had not finished the previous one.
        // ST_SHORT is a SUCCESS here and not an edge case: the
        // status stage of a control WRITE is a zero-length IN, the
        // device returns no bytes, and "fewer bytes than the maximum
        // packet size" is exactly what short means. Treating it as an
        // error made every SET_ADDRESS look like a failure, so
        // enumeration restarted and handed out a fresh address each
        // time -- which co-simulation caught as a device sitting at
        // address 4.
        if (st == Z_USBH_ST_NAK && ce->ctrl_soft_naks < CTRL_SOFT_NAKS) {
            // Same for the status stage: a device that has not
            // finished acting on the request NAKs here, and
            // SET_ADDRESS is allowed 50 ms to complete.
            ce->ctrl_soft_naks++;
            ce->ctrl_delay_until = usbh_ticks() + CTRL_NAK_TICKS;
            xact_start(ce->ctrl_dir_in ? Z_USBH_PID_OUT : Z_USBH_PID_IN,
                       dv->addr, 0, dv->xa_flags, dv->port, 1, 0,
                       dv->mps0, Z_USBH_OFF_PCTRL(d), 0,
                       Z_USBH_NAK_BUDGET);
            break;
        }
        if (st != Z_USBH_ST_OK && st != Z_USBH_ST_SHORT) {
            ce->ctrl_err_stage = CE_STATUS;
            ce->ctrl_state = CS_ERROR;
            break;
        }
        ce->ctrl_state = CS_DONE;
        break;

    case CS_DONE:
    default:
        break;
    }

    return ce->ctrl_state;
}

// True once the final transaction the engine launched has landed. The
// engine sets CS_DONE when it STARTS the status stage, so a caller
// must also see the bus go idle before believing the transfer is over.
static int ctrl_finished(int d)
{
    // CS_DONE is now only reached after the status stage's result has
    // been read, so it already implies the bus is idle.
    return devs[d].ctrl_state == CS_DONE;
}

// ---------------------------------------------------------------
// addresses
// ---------------------------------------------------------------
//
// 1..7. Address 0 is the enumeration address and only one device may
// hold it at a time -- which is why a port sitting in E_DEBOUNCE waits
// rather than racing ahead. With hubs (phase 4) two devices can arrive
// in the same second and both want it.

static int addr_alloc(void)
{
    int i;
    for (i = 1; i <= 7; i++) {
        if (!(addr_bitmap & (1u << i))) {
            addr_bitmap |= (1u << i);
            return i;
        }
    }
    return 0;
}

static void addr_free(int a)
{
    if (a >= 1 && a <= 7) addr_bitmap &= ~(1u << a);
}

// Does any port currently hold address zero?
//
// This started as "does someone hold address 0", which only had to
// cover up to E_DESC18. It now covers the WHOLE sequence, because the
// control transfer engine above -- ctrl_state, ctrl_dev, ce->ctrl_dir_in,
// ctrl_len, ce->ctrl_xferred, ce->ctrl_tgl -- is a single shared instance.
//
// Two ports enumerating at once drive that one engine from two places:
// each judges results belonging to the other's transfer, with the
// wrong direction and the wrong length. On hardware that produced a
// STALL on the data stage and babble on the status stage, and each
// device worked perfectly when it was the only one plugged in.
//
// Serialising is the small fix. The right fix is to move the control
// state into z_usbh_dev_t so transfers can genuinely overlap, which
// matters for hubs -- with several devices behind one, enumerating
// them strictly one at a time gets slow. Noted in docs/usb_host.md.
//
// The cost here is bounded: enumeration is a few hundred milliseconds
// and only happens on a plug event.
static int addr0_busy(void)
{
    int i;
    for (i = 0; i < Z_USBH_MAX_PORTS; i++) {
        // From the reset that puts a device on address 0 until
        // SET_ADDRESS's status stage has moved it off it. E_DESC18 is
        // the first state that talks to the new address.
        if (devs[i].state > E_DEBOUNCE && devs[i].state < E_DESC18)
            return 1;
    }
    return 0;
}

// ---------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------

// Record why, then fail. Called instead of assigning E_FAILED.
static void dev_fail(z_usbh_dev_t *dv)
{
    uint32_t st = z_usbh_rd(Z_USBH_XACT_S);
    dv->fail_state = dv->state;
    dv->fail_status = (uint8_t)Z_USBH_XS_STATUS(st);
    dv->fail_naks = (uint8_t)Z_USBH_XS_NAKS(st);
    // ctrl_state has ALREADY advanced past the stage that failed:
    // ctrl_step() sets the next stage when it LAUNCHES a transaction
    // and only checks the result on the following call. So the value
    // here names the stage whose result was just read.
    dv->fail_ctrl = dv->ctrl_err_stage;
    if (dv->att_n < 4) {
        dv->att_alen[dv->att_n] = (uint8_t)Z_USBH_XS_LEN(st);
        dv->att_tgl[dv->att_n] = (uint8_t)Z_USBH_XS_TOGGLE(st);
        dv->att_want[dv->att_n] = (uint8_t)dv->ctrl_tgl;
        dv->att_state[dv->att_n] = dv->state;
        dv->att_ctrl[dv->att_n] = dv->ctrl_err_stage;
        dv->att_status[dv->att_n] = dv->fail_status;
        dv->att_naks[dv->att_n] = dv->fail_naks;
        dv->att_n++;
    }
    dv->state = E_FAILED;
}

static void dev_reset_state(int i)
{
    // Give back everything this device held. The compat block is the
    // one that bites if it is forgotten: blk_claim() has exactly two,
    // so a leak does not show up on the first unplug/replug, it shows
    // up on the third, as a device that enumerates perfectly and then
    // silently does not appear.
    if (devs[i].blk >= 0) z_usbh_hid_release(devs[i].blk);
    addr_free(devs[i].addr);
    memset(&devs[i], 0, sizeof(z_usbh_dev_t));
    devs[i].port = (uint8_t)i;
    devs[i].state = E_EMPTY;
    devs[i].mps0 = 8;
    devs[i].blk = -1;
    devs[i].iface = -1;
}

static void port_step(int i)
{
    z_usbh_dev_t *dv = &devs[i];
    uint32_t ps = Z_USBH_PS(z_usbh_rd(Z_USBH_PORTSTAT), i);
    uint32_t d = Z_USBH_BUF + Z_USBH_OFF_PCTRL(i);
    int a;

    if (!(ps & Z_USBH_PS_CONNECTED)) {
        if (dv->state != E_EMPTY) {
            // Gone. Clearing the compat block is deliberate and not
            // just tidiness: sw/os/hid.c watches typ to know when to
            // synthesise key releases for a keyboard yanked
            // mid-keypress. See docs/user_input.md.
            // Stop the poll slot BEFORE anything else. It is running
            // in hardware against a device that is no longer there,
            // and every poll it makes now is a transaction that will
            // time out and take bus time from the other port.
            z_usbh_wr(Z_USBH_POLL_A(i), 0);
            dev_reset_state(i);
        }
        return;
    }

    switch (dv->state) {

    case E_EMPTY:
        dv->mps0 = 8;
        dv->addr = 0;
        // The port reports the speed of a DIRECTLY attached device.
        // Direct low speed is the one case that inverts polarity.
        dv->xa_flags = (ps & Z_USBH_PS_LOWSPEED) ? 0x03 : 0x00;
        dv->state = E_DEBOUNCE;
        break;

    case E_DEBOUNCE:
        // The hardware has already debounced the attach. This state
        // exists to serialise address 0 between ports.
        if (!addr0_busy()) dv->state = E_RESET;
        break;

    case E_RESET:
        z_usbh_wr(Z_USBH_CTRL,
                  z_usbh_rd(Z_USBH_CTRL) | Z_USBH_CTRL_PORT_RST(i));
        dv->deadline = usbh_ticks() + 64;
        dv->state = E_RESET_WAIT;
        break;

    case E_RESET_WAIT:
        if (ps & Z_USBH_PS_ENABLED) {
            z_usbh_wr(Z_USBH_CTRL,
                      z_usbh_rd(Z_USBH_CTRL) & ~Z_USBH_CTRL_PORT_RST(i));
            // Re-read the speed AFTER the reset. A bouncy attach can
            // be read wrong, and a reset is also how the speed is
            // settled for good.
            dv->xa_flags = (ps & Z_USBH_PS_LOWSPEED) ? 0x03 : 0x00;
            // Ask for 8 bytes, which every device can answer with a
            // single packet whatever its real bMaxPacketSize0 is --
            // that byte is what we are asking FOR, so it cannot be
            // assumed before this.
            ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                       (DESC_DEVICE << 8), 0, 8);
            dv->state = E_DESC8;
        } else if ((int32_t)(usbh_ticks() - dv->deadline) > 0) {
            dev_fail(dv);
        }
        break;

    case E_DESC8:
        if (ctrl_step(i) == CS_ERROR) { dev_fail(dv); break; }
        if (!ctrl_finished(i)) break;
        dv->mps0 = z_usbh_rb(d + 7);
        dv->d_class = z_usbh_rb(d + 4);
        if (dv->mps0 < 8 || dv->mps0 > 64) dv->mps0 = 8;
        a = addr_alloc();
        if (!a) { dev_fail(dv); break; }
        dv->addr = 0;
        ctrl_begin(i, 0, REQ_SET_ADDRESS, (uint16_t)a, 0, 0);
        dv->pend_addr = (uint8_t)a;
        dv->state = E_SET_ADDR;
        break;

    case E_SET_ADDR:
        if (ctrl_step(i) == CS_ERROR) { dev_fail(dv); break; }
        if (!ctrl_finished(i)) break;
        // A device only adopts its new address once the status stage
        // has completed, so this assignment cannot happen earlier.
        dv->addr = dv->pend_addr;
        dv->deadline = usbh_ticks() + 4;
        dv->state = E_ADDR_SETTLE;
        break;

    case E_ADDR_SETTLE:
        // USB gives a device 2 ms to switch addresses. Talking to it
        // before that is a transaction that mysteriously times out.
        if ((int32_t)(usbh_ticks() - dv->deadline) < 0) break;
        ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                   (DESC_DEVICE << 8), 0, 18);
        dv->state = E_DESC18;
        break;

    case E_DESC18:
        if (ctrl_step(i) == CS_ERROR) { dev_fail(dv); break; }
        if (!ctrl_finished(i)) break;
        // Full device descriptor is in the buffer now.
        dv->d_vid_lo = z_usbh_rb(d + 8);
        dv->d_vid_hi = z_usbh_rb(d + 9);
        dv->d_pid_lo = z_usbh_rb(d + 10);
        dv->d_pid_hi = z_usbh_rb(d + 11);
        dv->d_nconf = z_usbh_rb(d + 17);
        dv->got_desc = 1;
        ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                   (DESC_CONFIG << 8), 0, 9);
        dv->state = E_CONFIG9;
        break;

    case E_CONFIG9:
        if (ctrl_step(i) == CS_ERROR) { dev_fail(dv); break; }
        if (!ctrl_finished(i)) break;
        // wTotalLength. Clamped to the scratch region rather than
        // trusted: a descriptor longer than the buffer is a device
        // bug, and reading it would walk into the sector area.
        dv->cfg_len = z_usbh_rb(d + 2);
        if (z_usbh_rb(d + 3) || dv->cfg_len > 200) dv->cfg_len = 200;
        ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                   (DESC_CONFIG << 8), 0, dv->cfg_len);
        dv->state = E_CONFIG_ALL;
        break;

    case E_CONFIG_ALL:
        if (ctrl_step(i) == CS_ERROR) { dev_fail(dv); break; }
        if (!ctrl_finished(i)) break;
        dv->cfg_val = z_usbh_rb(d + 5);
        dv->cfg_nif = z_usbh_rb(d + 4);
        dv->cfg_attr = z_usbh_rb(d + 7);
        dv->got_cfg = 1;
        {
            int k;
            dv->cfg_rawn = dv->cfg_len > 64 ? 64 : dv->cfg_len;
            for (k = 0; k < dv->cfg_rawn; k++)
                dv->cfg_raw[k] = z_usbh_rb(d + k);
        }
        ctrl_begin(i, 0, REQ_SET_CONFIG, dv->cfg_val, 0, 0);
        dv->state = E_SET_CONFIG;
        break;

    case E_SET_CONFIG:
        if (ctrl_step(i) == CS_ERROR) { dev_fail(dv); break; }
        if (!ctrl_finished(i)) break;
        dv->iface = (int8_t)z_usbh_hid_probe(dv->cfg_raw, dv->cfg_rawn);
        if (dv->iface < 0) { dv->state = E_BIND; break; }
        // Boot protocol, explicitly. A device whose interface declares
        // subclass 1 usually defaults to it, and "usually" is the
        // problem: a keyboard that comes up in report protocol sends a
        // layout the hardware's byte mux is not expecting, and the
        // symptom is keycodes in the wrong bytes rather than nothing
        // working.
        ctrl_begin(i, HID_OUT_IFACE, REQ_SET_PROTOCOL, 0,
                   (uint16_t)dv->iface, 0);
        dv->state = E_SET_PROTO;
        break;

    case E_SET_PROTO:
        // A STALL here is not fatal -- plenty of mice refuse
        // SET_PROTOCOL and are in boot protocol anyway. Carry on and
        // let the report layout speak for itself.
        if (ctrl_step(i) == CS_ERROR || ctrl_finished(i)) {
            ctrl_begin(i, HID_OUT_IFACE, REQ_SET_IDLE, 0,
                       (uint16_t)dv->iface, 0);
            dv->state = E_SET_IDLE;
        }
        break;

    case E_SET_IDLE:
        // SET_IDLE(0) means "only report when something changes".
        // Without it a keyboard re-sends its state on a timer and the
        // auto-poll slot delivers duplicate reports to the compat
        // block, which sw/os/hid.c would see as repeated keypresses.
        // Also optional, also STALLed by some devices.
        if (ctrl_step(i) == CS_ERROR || ctrl_finished(i))
            dv->state = E_BIND;
        break;

    case E_BIND:
        // Hand the configuration to whatever driver recognises it. An
        // unrecognised device is not an error -- it is a device with
        // no driver, and it sits enumerated and idle.
        // Parse the SAVED COPY, not the live packet buffer.
        //
        // SET_PROTOCOL and SET_IDLE run between the configuration
        // read and this point, and every control transfer uses the
        // same scratch region for its data stage. The descriptor the
        // walk needs had been overwritten by the time it ran -- so a
        // keyboard whose descriptor was perfect, and which lsusb
        // printed correctly from the copy, matched nothing here.
        dv->blk = (int8_t)z_usbh_hid_bind(i, dv->addr, dv->xa_flags,
                                          dv->port, dv->cfg_raw,
                                          dv->cfg_rawn);
        if (dv->blk < 0 &&
            z_usbh_msc_bind(dv->addr, dv->xa_flags, dv->port,
                            dv->mps0, dv->cfg_raw, dv->cfg_rawn)) {
            // Claimed, but NOT started.
            //
            // z_usbh_msc_start() waits for the unit to report ready,
            // which a card reader with no card can refuse for
            // hundreds of milliseconds. This runs from the IRQ 9
            // handler and the ktimer, so it must not block -- the
            // geometry read happens in disk_initialize() instead,
            // which FatFs calls from f_mount() in process context.
            dv->cls = Z_USBH_CLASS_MSC;
        }

        if (dv->blk >= 0) {
            dv->cls = Z_USBH_CLASS_HID;
            // An auto-poll slot counts FRAMES, so the frame timer has
            // to be running by the time one is enabled -- it is off
            // during enumeration (see z_usbh_init). Co-simulation
            // caught this the moment the timer moved: everything
            // enumerated and the cursor never moved again.
            z_usbh_wr(Z_USBH_CTRL,
                      z_usbh_rd(Z_USBH_CTRL) | Z_USBH_CTRL_FRAME_EN);
        }
        usbh_any_ready = 1;
        dv->state = E_RUNNING;
        break;

    case E_FAILED:
        // Enumeration can fail for reasons that are not permanent --
        // a device that was still settling, a reset that landed badly.
        // Retry a few times before giving up, and give up quietly
        // rather than spinning: an unplug clears this state anyway,
        // via the not-connected branch at the top.
        if (dv->retries < 3) {
            uint8_t fs = dv->fail_state, fx = dv->fail_status;
            uint8_t fc = dv->fail_ctrl, fn = dv->fail_naks;
            uint8_t r = dv->retries + 1;
            dv->retries++;
            addr_free(dv->addr);
            dv->addr = 0;
            dv->state = E_EMPTY;
            dv->fail_state = fs;
            dv->fail_status = fx;
            dv->fail_ctrl = fc;
            dv->fail_naks = fn;
            dv->retries = r;
        }
        break;

    case E_RUNNING:
    default:
        break;
    }
}

// ---------------------------------------------------------------
// public
// ---------------------------------------------------------------

void z_usbh_init(void)
{
    uint32_t cfg;
    int feat;
    int i;

    usbh_present = 0;
    usbh_any_ready = 0;
    addr_bitmap = 0;

    // -- the CONFIG magic is the probe, the feature bit is advisory --
    //
    // Deliberately this way round. The feature bit is set by
    // rtl/csrs.vh from a `define and says what the bitstream was
    // MEANT to contain; the magic is read from the block itself and
    // says what is actually at that address. When they disagree it is
    // the bit that is wrong, and gating on it first means a perfectly
    // good controller stays silent and invisible because of a
    // bookkeeping error two files away.
    //
    // Every combination says something specific, because "nothing
    // printed" is the one outcome that leaves you guessing.
    cfg = z_usbh_rd(Z_USBH_CONFIG);
    feat = z_soc_has_feature2(Z_FEATURE2_USB_HOST);

    if (Z_USBH_CFG_MAGIC(cfg) != Z_USBH_MAGIC) {
        if (feat)
            printf("usb host: feature bit set but NOT FOUND "
                   "(config reads %08lx)\n", (unsigned long)cfg);
        return;
    }

    if (!feat)
        printf("usb host: present, but CSR_FEATURES2 bit is clear "
               "-- check `USB_HOST in rtl/csrs.vh\n");

    usbh_present = 1;

    // Say so. Without this the block is silent on a board that has it
    // and silent on a board that does not, and the only way to tell
    // the two apart is to plug something in and see whether it works.
    printf("usb host: %d port%s, %d poll slots, %d KB buffer (v%d)\n",
           (int)Z_USBH_CFG_PORTS(cfg),
           Z_USBH_CFG_PORTS(cfg) == 1 ? "" : "s",
           (int)Z_USBH_CFG_SLOTS(cfg),
           (int)Z_USBH_CFG_BUFKB(cfg),
           (int)Z_USBH_CFG_VERSION(cfg));

    for (i = 0; i < Z_USBH_MAX_PORTS; i++) {
        devs[i].blk = -1;
        dev_reset_state(i);
    }

// Frame timer ON from the start.
    //
    // On a low-speed port its only job is the keepalive, and the
    // keepalive is what stops the device suspending. Turning it off
    // during enumeration was tried and is wrong: a device suspends
    // after 3 ms of bus inactivity and then ignores us completely.
    z_usbh_wr(Z_USBH_CTRL, Z_USBH_CTRL_FRAME_EN |
                           Z_USBH_CTRL_POLL_EN |
                           Z_USBH_CTRL_PORT_EN(0) |
                           Z_USBH_CTRL_PORT_EN(1));

    z_usbh_wr(Z_USBH_IRQSTAT, 0x3f);
    z_usbh_wr(Z_USBH_IRQEN, Z_USBH_IRQ_XACT_DONE | Z_USBH_IRQ_PORT0 |
                            Z_USBH_IRQ_PORT1 | Z_USBH_IRQ_POLL_ERROR);
}

void z_usbh_poll(void)
{
    uint32_t s;
    int i;

    if (!usbh_present) return;

    // IRQ 9 is a LEVEL derived from (IRQSTAT & IRQEN). Clearing it
    // first means a change arriving while this runs sets the bit
    // again and we are re-entered, rather than being lost.
    s = z_usbh_rd(Z_USBH_IRQSTAT);
    if (s) z_usbh_wr(Z_USBH_IRQSTAT, s);

    for (i = 0; i < Z_USBH_MAX_PORTS; i++) port_step(i);
}

// Names for the enumeration states, in the order they are #defined.
// Kept next to that list because they are two halves of one thing and
// nothing checks that they agree.
static const char *state_name(int st)
{
    switch (st) {
    case E_EMPTY:       return "empty";
    case E_DEBOUNCE:    return "debounce";
    case E_RESET:       return "reset";
    case E_RESET_WAIT:  return "reset-wait";
    case E_DESC8:       return "get-desc8";
    case E_SET_ADDR:    return "set-address";
    case E_ADDR_SETTLE: return "addr-settle";
    case E_DESC18:      return "get-desc";
    case E_CONFIG9:     return "get-cfg9";
    case E_CONFIG_ALL:  return "get-cfg";
    case E_SET_CONFIG:  return "set-config";
    case E_SET_PROTO:   return "set-protocol";
    case E_SET_IDLE:    return "set-idle";
    case E_BIND:        return "binding";
    case E_RUNNING:     return "running";
    case E_FAILED:      return "FAILED";
    default:            return "?";
    }
}

static const char *status_name(int st)
{
    switch (st) {
    case Z_USBH_ST_OK:      return "ok";
    case Z_USBH_ST_NAK:     return "NAK (retries exhausted)";
    case Z_USBH_ST_STALL:   return "STALL";
    case Z_USBH_ST_TIMEOUT: return "TIMEOUT (no response)";
    case Z_USBH_ST_CRCERR:  return "CRC/bitstuff error";
    case Z_USBH_ST_BABBLE:  return "babble";
    case Z_USBH_ST_SHORT:   return "short packet";
    case Z_USBH_ST_ABORT:   return "aborted (port went away)";
    default:                return "?";
    }
}

static const char *typ_name(int t)
{
    switch (t) {
    case Z_USBH_TYP_KBD:   return "keyboard";
    case Z_USBH_TYP_MOUSE: return "mouse";
    case Z_USBH_TYP_PAD:   return "gamepad";
    default:               return "-";
    }
}

/*
 * What is plugged in, and where each port got stuck if it did.
 *
 * The state name is the useful part. A port that never leaves
 * "debounce" is not seeing a pull-up; one stuck at "get-desc8" is
 * attached and not answering; "FAILED" means it answered badly enough
 * to give up. Those are three different faults that all present as
 * "the mouse does not work".
 */
void z_usbh_dump(void)
{
    uint32_t cfg, ps, pa, pb;
    int i;

    if (!usbh_present) {
        printf("usb: no host controller\n");
        return;
    }

    cfg = z_usbh_rd(Z_USBH_CONFIG);
    ps = z_usbh_rd(Z_USBH_PORTSTAT);

    printf("usb: controller v%d, %d port(s), %d slot(s), frame %lu\n",
           (int)Z_USBH_CFG_VERSION(cfg), (int)Z_USBH_CFG_PORTS(cfg),
           (int)Z_USBH_CFG_SLOTS(cfg),
           (unsigned long)Z_USBH_PS_FRAME(ps));

    {
        uint32_t d0 = z_usbh_rd(Z_USBH_DEBUG0);
        uint32_t d1 = z_usbh_rd(Z_USBH_DEBUG1);
        printf("usb: wire: %lu tx, %lu rx started, %lu good, %lu bad\n",
               (unsigned long)(d0 & 0xffff),
               (unsigned long)(d0 >> 16),
               (unsigned long)(d1 & 0xff),
               (unsigned long)((d1 >> 8) & 0xff));
    }

    for (i = 0; i < Z_USBH_MAX_PORTS; i++) {
        uint32_t p = Z_USBH_PS(ps, i);

        printf("usb: port %d %s%s%s%s speed=%s\n", i,
               (p & Z_USBH_PS_CONNECTED) ? "connected " : "empty ",
               (p & Z_USBH_PS_ENABLED) ? "enabled " : "",
               (p & Z_USBH_PS_RESETTING) ? "resetting " : "",
               (p & Z_USBH_PS_CHANGE) ? "changed " : "",
               !(p & Z_USBH_PS_CONNECTED) ? "-" :
               (p & Z_USBH_PS_LOWSPEED) ? "low" : "full");

        if (!(p & Z_USBH_PS_CONNECTED)) continue;

        printf("usb:   state=%s addr=%d mps0=%d",
               state_name(devs[i].state), devs[i].addr, devs[i].mps0);
        if (devs[i].blk >= 0)
            printf(" block=%d typ=%s", devs[i].blk,
                   typ_name(devs[i].blk == 0 ?
                            (int)((z_usbh_rd(Z_USBH_HID0_INFO) >> 24) & 3) :
                            (int)((z_usbh_rd(Z_USBH_HID1_INFO) >> 24) & 3)));
        if (devs[i].cls == Z_USBH_CLASS_MSC) printf(" class=msc");
        if (devs[i].retries) printf(" retries=%d", devs[i].retries);
        printf("\n");

        {
            int a;
            for (a = 0; a < devs[i].att_n; a++)
                printf("usb:   attempt %d: %s, %s: %s (%d nak%s)\n",
                       a + 1, state_name(devs[i].att_state[a]),
                       devs[i].att_ctrl[a] == CE_SETUP ? "SETUP stage" :
                       devs[i].att_ctrl[a] == CE_DATA ? "DATA stage" :
                       devs[i].att_ctrl[a] == CE_STATUS ? "STATUS stage" :
                       "before any transaction",
                       status_name(devs[i].att_status[a]),
                       devs[i].att_naks[a],
                       devs[i].att_naks[a] == 1 ? "" : "s");
            for (a = 0; a < devs[i].att_n; a++)
                printf("usb:     %d: act_len=%d toggle got=%d want=%d\n",
                       a + 1, devs[i].att_alen[a], devs[i].att_tgl[a],
                       devs[i].att_want[a]);
        }

        if (devs[i].got_desc)
            printf("usb:   device %04x:%04x class %d, %d config(s)\n",
                   (unsigned)(devs[i].d_vid_lo |
                              (devs[i].d_vid_hi << 8)),
                   (unsigned)(devs[i].d_pid_lo |
                              (devs[i].d_pid_hi << 8)),
                   devs[i].d_class, devs[i].d_nconf);
        if (devs[i].got_cfg)
            printf("usb:   config value %d, %d interface(s), "
                   "attributes %02x\n",
                   devs[i].cfg_val, devs[i].cfg_nif, devs[i].cfg_attr);

        if (devs[i].got_cfg && devs[i].blk < 0) {
            int k;
            printf("usb:   cfg desc (%d of %d bytes):",
                   devs[i].cfg_rawn, devs[i].cfg_len);
            for (k = 0; k < devs[i].cfg_rawn; k++) {
                if ((k & 15) == 0) printf("\nusb:    ");
                printf(" %02x", devs[i].cfg_raw[k]);
            }
            printf("\n");
        }

        if (devs[i].state == E_FAILED)
            printf("usb:   failed in %s, %s: %s (%d nak%s)\n",
                   state_name(devs[i].fail_state),
                   devs[i].fail_ctrl == CE_SETUP ? "SETUP stage" :
                   devs[i].fail_ctrl == CE_DATA ? "DATA stage" :
                   devs[i].fail_ctrl == CE_STATUS ? "STATUS stage" :
                   "before any transaction",
                   status_name(devs[i].fail_status),
                   devs[i].fail_naks, devs[i].fail_naks == 1 ? "" : "s");

        // Raw line state, straight off the pins. The speed the port
        // reports is derived from which line the device pulls up, and
        // if that derivation is wrong everything downstream fails in
        // ways that look like protocol errors. J means D+ high: full
        // speed idle. K means D- high: low speed idle.
        printf("usb:   lines: D+=%d D-=%d (%s)\n",
               (int)((p >> 7) & 1), (int)((p >> 6) & 1),
               ((p >> 6) & 3) == 2 ? "J, full-speed idle" :
               ((p >> 6) & 3) == 1 ? "K, low-speed idle" :
               ((p >> 6) & 3) == 0 ? "SE0" : "SE1 (bad)");

        pa = z_usbh_rd(Z_USBH_POLL_A(i));
        pb = z_usbh_rd(Z_USBH_POLL_B(i));
        if (pa & Z_USBH_PA_ENABLE)
            printf("usb:   poll slot %d: ep%lu every %lu frame(s), "
                   "mode %lu, last status %lu\n",
                   i, (unsigned long)((pa >> 7) & 0xf),
                   (unsigned long)((pa >> 22) & 0xff),
                   (unsigned long)((pb >> 11) & 7),
                   (unsigned long)Z_USBH_PB_STATUS(pb));
        else
            printf("usb:   poll slot %d: idle\n", i);
    }
}

/*
 * Release both ports completely and report what the lines do.
 *
 * The decisive experiment for "an empty port reads J". With the port
 * disabled, usb_port.v sits in P_DISCON and never drives, and the
 * scheduler has nothing to run, so the controller is not touching the
 * pins at all. Whatever the lines read then is the BOARD talking, not
 * us.
 *
 *   SE0        correct. An empty port with its 15k pull-downs.
 *   J or K     something external holds a line high -- a missing
 *              pull-down, an FPGA pull-up left at its default, or a
 *              device that is actually attached after all.
 *
 * Restores the previous CTRL afterwards, so it is safe to run with
 * devices plugged in; they will be re-enumerated.
 */
void z_usbh_probe_idle(void)
{
    uint32_t save, ps;
    volatile int d;
    int i, seen_se0, seen_rst;

    if (!usbh_present) {
        printf("usb: no host controller\n");
        return;
    }

    save = z_usbh_rd(Z_USBH_CTRL);
    z_usbh_wr(Z_USBH_CTRL, 0);

    // The port state machines need a moment to fall back to
    // P_DISCON and stop driving. This is a busy wait, which is
    // normally forbidden here -- it is acceptable only because this
    // is an interactive diagnostic and not part of the driver.
    for (d = 0; d < 200000; d++) { }

    ps = z_usbh_rd(Z_USBH_PORTSTAT);

    for (i = 0; i < Z_USBH_MAX_PORTS; i++) {
        uint32_t p = Z_USBH_PS(ps, i);
        int lines = (p >> 6) & 3;
        printf("usb: port %d released: D+=%d D-=%d (%s)%s\n", i,
               (int)((p >> 7) & 1), (int)((p >> 6) & 1),
               lines == 2 ? "J" : lines == 1 ? "K" :
               lines == 0 ? "SE0" : "SE1",
               lines == 0 ? " -- correct for an empty port" :
                            " -- SOMETHING EXTERNAL HOLDS THIS");
    }

    // -- second half: does the readback follow what we DRIVE? --
    //
    // Reading the same thing with two devices, one and none says the
    // lines are not tracking the bus. That has two completely
    // different causes -- something external pinning them, or an
    // input path that returns a constant -- and they need opposite
    // fixes, so guessing between them is worthless.
    //
    // A port reset drives SE0 hard for 10 ms from usb_port.v, which is
    // a state we command and can therefore check. If PORTSTAT shows
    // SE0 during it, the input path works and the idle J above is
    // real, external, and a board question. If it still shows J while
    // we are provably driving both lines low, the readback is a
    // constant and every line reading so far has been fiction.
    //
    // The port must reach P_READY before it will accept a reset, so
    // this enables and waits out the attach debounce first.
    z_usbh_wr(Z_USBH_CTRL, Z_USBH_CTRL_PORT_EN(0));
    for (d = 0; d < 3000000; d++) { }

    z_usbh_wr(Z_USBH_CTRL,
              Z_USBH_CTRL_PORT_EN(0) | Z_USBH_CTRL_PORT_RST(0));

    seen_se0 = 0;
    seen_rst = 0;
    for (d = 0; d < 400000; d++) {
        ps = z_usbh_rd(Z_USBH_PORTSTAT);
        if (Z_USBH_PS(ps, 0) & Z_USBH_PS_RESETTING) seen_rst = 1;
        if (((Z_USBH_PS(ps, 0) >> 6) & 3) == 0) seen_se0 = 1;
    }

    printf("usb: port 0 driven SE0: resetting seen=%d, SE0 seen=%d\n",
           seen_rst, seen_se0);
    if (seen_rst && !seen_se0)
        printf("usb: READBACK IS STUCK -- pins do not follow our own "
               "drive, so every line reading is meaningless\n");
    else if (seen_se0)
        printf("usb: readback works -- the idle J above is real and "
               "external\n");
    else
        printf("usb: port never reached reset (never saw an attach)\n");

    z_usbh_wr(Z_USBH_CTRL, save);
    for (i = 0; i < Z_USBH_MAX_PORTS; i++) dev_reset_state(i);
    printf("usb: ports re-enabled\n");
}

/*
 * Does the packet buffer actually hold what we put in it?
 *
 * Everything the controller transmits comes out of this 2 KB block,
 * reached by byte writes that the Wishbone side turns into a word
 * cycle with one lane selected. That path has only ever been
 * exercised by simulation, where the lane select was generated by my
 * own testbench rather than by the CPU -- so "the SETUP packet is
 * eight bytes of garbage" has never been ruled out on hardware, and
 * it would present exactly as a device that ignores us.
 */
void z_usbh_buftest(void)
{
    uint32_t base = Z_USBH_BUF + Z_USBH_OFF_PSETUP(0);
    int i, bad = 0;

    if (!usbh_present) {
        printf("usb: no host controller\n");
        return;
    }

    // Byte writes at every lane offset, with a pattern where each
    // byte differs from its neighbours -- a lane-select fault that
    // writes the right value to the wrong lane stays invisible to a
    // uniform pattern.
    for (i = 0; i < 16; i++) z_usbh_wb(base + i, (uint8_t)(0xa0 + i));
    for (i = 0; i < 16; i++) {
        uint8_t got = z_usbh_rb(base + i);
        if (got != (uint8_t)(0xa0 + i)) {
            printf("usb: buf[%d] wrote %02x read %02x\n",
                   i, 0xa0 + i, got);
            bad++;
        }
    }

    printf("usb: buffer byte test: %s\n",
           bad ? "FAILED" : "ok (16/16)");

    // And the real thing: build the SETUP we would send and read it
    // straight back out of the buffer. This is the exact byte
    // sequence that goes on the wire after the token.
    put_setup(0, DIR_IN, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, 8);
    printf("usb: setup packet reads back:");
    for (i = 0; i < 8; i++) printf(" %02x", z_usbh_rb(base + i));
    printf("\n");
    printf("usb: expected:              80 06 00 01 00 00 08 00\n");
}

/*
 * Start enumeration over on every port.
 *
 * Enumeration gives up after four attempts and the driver then sits
 * idle, so by the time anyone types a command at the shell there is no
 * USB traffic at all -- which is why arming the probe and waiting
 * caught nothing. This puts the ports back to square one so there is
 * something to trigger on.
 */
void z_usbh_rescan(void)
{
    int i;
    if (!usbh_present) return;
    for (i = 0; i < Z_USBH_MAX_PORTS; i++) dev_reset_state(i);
}

int z_usbh_ready(void)
{
    return usbh_any_ready;
}

int z_usbh_status(uint8_t *typ, int max)
{
    int i, n = 0;
    if (!usbh_present) return 0;
    for (i = 0; i < Z_USBH_MAX_PORTS && i < max; i++) {
        if (devs[i].state == E_RUNNING) {
            typ[n++] = devs[i].cls;
        }
    }
    return n;
}
