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
#include "usbh_int.h"
#include "usbh_cdc.h"
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

// -- ...except for a low-speed device behind a hub --
//
// The hardware budget retries a NAKed transaction ~3 us after the NAK.
// No real host does that: UHCI, OHCI and a high-speed hub's TT revisit
// a NAKed control endpoint on a later list pass, typically the next
// frame. A low-speed keyboard behind a hub (Holtek 04d9:1203) failed
// every enumeration with the same signature -- NAK on a data stage,
// then no response to the retry -- while the same keyboard worked on a
// root port and a mouse that never NAKs worked behind the hub. So a
// low-speed device behind a hub gets this budget instead, 0 by default:
// every NAK goes back to software, which retries after CTRL_NAK_TICKS,
// in a later frame, as a real host would. `usbnak N` changes it at run
// time for comparison.
//
// Round 21 widened this to the HUB ITSELF and every device behind one.
// On hardware, low-speed devices behind a hub failed intermittently,
// and became completely reliable while `usbcap` was running -- whose
// only protocol effect is NAK budget 0 on every transaction on port 0.
// Low-speed devices already had it, so the difference was the hub's own
// control endpoint: a hub NAKs its requests while it is busy with a
// port reset, which is exactly when a device behind it is enumerating,
// and the hardware re-sent each NAKed request every ~5 us, four times
// (seen in captures). `usbnak 3` restores the old behaviour to compare.
static uint8_t hub_nak_budget = 0;

static uint8_t nak_budget(const z_usbh_dev_t *dv)
{
    if (dv->parent >= 0 || dv->cls == Z_USBH_CLASS_HUB)
        return hub_nak_budget;
    return Z_USBH_NAK_BUDGET;
}

void z_usbh_set_ls_hub_nak(int n)
{
    if (n < 0) n = 0;
    if (n > 15) n = 15;
    hub_nak_budget = (uint8_t)n;
    printf("usb: hardware NAK retries for hubs and devices behind them: "
           "%d\n", n);
}

// HID class requests, addressed to an interface.
#define HID_OUT_IFACE       0x21
#define REQ_SET_IDLE        0x0a
#define REQ_SET_PROTOCOL    0x0b


z_usbh_dev_t usbh_devs[Z_USBH_MAX_DEVS];
// Shared with usbh_hub.c through usbh_int.h; the short name keeps the
// many references in this file readable.
#define devs usbh_devs

// The control transfer engine's state now lives in z_usbh_dev_t, one
// instance per device, reached through `ce` inside usbh_ctrl_step().
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
// Set by z_usbh_bus_reserve(); see usbh.h. Written in process context,
// read from the ISR, so volatile, and a single byte so every access is
// atomic on this core.
static volatile uint8_t bus_reserved;

// -- whose result is in XACT_S --
//
// The device whose control transaction was launched last and has not
// yet been judged, or -1. Only that device may read XACT_S or launch;
// every other device waits its turn.
//
// Without it, each device's ctrl_step() waited only for "not pending"
// and then read XACT_S as its own. z_usbh_poll() steps devices in
// index order, so when device 1 had launched, device 0 ran first on
// the next poll and consumed device 1's result -- judging, say, a
// 4-byte GET_PORT_STATUS reply as its own SETUP. Two devices
// enumerating on the two root ports at once could hit it; the retry
// logic mostly hid it. With a hub it is routine, because the hub's
// port requests run alongside every child's enumeration, and the hub
// co-simulation failed on it at once.
static int8_t xact_owner = -1;

// -- wire capture; see z_usbh_cap_start() in usbh.h --
//
// The probe's registers, through cap_rd()/cap_wr(). On hardware, plain
// memory accesses. In a co-simulation built with Z_USBH_COSIM_PROBE they
// go over the simulated bus to the real rtl/probe.v, so this same code
// is exercised against known traffic -- tb_usb_hub_cosim's capture
// case. The tool had misled more than once before it was checked that
// way.
#if !defined(Z_USBH_COSIM)
#define CAP_HAVE_PROBE 1
#define cap_rd(o)       (*(volatile uint32_t *)(0x7f000000u + (o)))
#define cap_wr(o, v)    (*(volatile uint32_t *)(0x7f000000u + (o)) = (v))
#elif defined(Z_USBH_COSIM_PROBE)
#define CAP_HAVE_PROBE 1
#define cap_rd(o)       z_usbh_rd(0x7f000000u + (o))
#define cap_wr(o, v)    z_usbh_wr(0x7f000000u + (o), (v))
#endif
static uint8_t cap_on, cap_frozen;
// 0: freeze on the first failing transaction. 1 (`usbcapok`): freeze on
// the first SUCCESSFUL IN with data from a low-speed device behind a
// hub -- which holds the device's data packet and, after it, our own
// PRE + ACK. That handshake had never been captured, because every
// failure ends before it; and a full-speed host sending bytes after its
// low-speed ACK is a documented way to make a hub drop the port
// (docs/usb_host.md, "Known issues", item 5).
static uint8_t cap_mode;
static uint8_t cap_addr, cap_state, cap_stage, cap_status, cap_ls;
static uint8_t cap_pid, cap_last_pid;   // PID of the failing transaction
// The wire counters just before the last captured transaction, and at
// the freeze: whether the receiver even STARTED on a reply the host then
// called a timeout. DEBUG0[31:16] rx started; DEBUG1[7:0] good, [15:8]
// bad -- the layout z_usbh_dump() prints.
static uint32_t cap_d0_before, cap_d0_last, cap_d0_after;
static uint32_t cap_d1_before, cap_d1_last, cap_d1_after;
static int8_t ctrl_cur = -1;    // the device inside ctrl_step()
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

uint32_t usbh_ticks(void)
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

    if (cap_on && !cap_frozen && port == 0) {
        cap_last_pid = pid;
        cap_d0_last = z_usbh_rd(Z_USBH_DEBUG0);
        cap_d1_last = z_usbh_rd(Z_USBH_DEBUG1);
        // One attempt per transaction, so one capture holds it whole.
        nak = 0;
#ifdef CAP_HAVE_PROBE
        // Re-arm only when the previous window is over. While a window
        // is still armed or recording, this transaction falls inside
        // it, so a frozen capture holds the failing transaction AND
        // what came just before it -- the SETUP of its own control
        // transfer, any PRE traffic ahead of it. Re-arming at every
        // transaction gave only the failing one: on hardware, a clean
        // IN to the hub followed by silence, with no way to see what
        // had left the hub deaf. CTRL bit 0 armed, bit 1 running.
        //
        // That left a hole: a transaction LAUNCHED near the end of a
        // window transmits after it closes and is never captured -- on
        // hardware, twice, the SETUP that actually failed. So arm at
        // every transaction again: the failing one is then always in the
        // capture from its first bit.
        cap_wr(0, 1);
#endif
    }

    z_usbh_wr(Z_USBH_XACT_A, a);
    z_usbh_wr(Z_USBH_XACT_B, Z_USBH_XB_OFF(off) | Z_USBH_XB_LEN(len) |
                             Z_USBH_XB_NAK(nak) | Z_USBH_XB_START);
    // Only ever called from ctrl_step(), which has set ctrl_cur.
    xact_owner = ctrl_cur;
}

static void put_setup(int d, uint8_t bmType, uint8_t bReq, uint16_t wVal,
                      uint16_t wIdx, uint16_t wLen)
{
    uint32_t p = Z_USBH_BUF + devs[d].scr;
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

void usbh_ctrl_begin(int d, uint8_t bmType, uint8_t bReq,
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
// The data stage is complete: send the status stage NOW, a zero-length
// packet in the opposite direction, always DATA1.
//
// Both places that reach this used to set ctrl_state = CS_STATUS and
// return, leaving CS_STATUS to judge XACT_S on the NEXT call. But this
// device had already read its result and released the engine, so by
// then another device's transaction could have run and XACT_S was
// THEIRS. A split transfer -- a multi-packet read the end-of-frame
// guard cut and CS_DATA_RUN resumed, which is a keyboard's 59-byte
// configuration descriptor almost every time -- then failed with a hub
// request's timeout, and a hub request resumed after a NAK inherited
// the keyboard's. Hardware: both keyboards failed get-cfg with "TIMEOUT
// (0 naks)" and hub requests failed only while a keyboard was present.
// And CS_STATUS's own all-data-in case re-entered CS_STATUS, re-read
// its own NAK, counted the length again, and could loop.
static void ctrl_send_status(int d)
{
    z_usbh_dev_t *dv = &devs[d];
    z_usbh_dev_t *ce = dv;

    xact_start(ce->ctrl_dir_in ? Z_USBH_PID_OUT : Z_USBH_PID_IN,
               dv->addr, 0, dv->xa_flags, dv->port, 1, 0,
               dv->mps0, (devs[d].scr + 8u), 0,
               nak_budget(dv));
    ce->ctrl_pend = 1;
    ce->ctrl_state = CS_FINAL;
}

int usbh_ctrl_step(int d)
{
    z_usbh_dev_t *dv = &devs[d];
    z_usbh_dev_t *ce = dv;
    uint32_t s;
    uint8_t st;

    if (ce->ctrl_state == CS_DONE || ce->ctrl_state == CS_ERROR)
        return ce->ctrl_state;

    // The one place a new control transfer puts its first packet on the
    // bus. While mass storage holds the engine, stay here: the caller
    // sees "still in progress" and simply asks again later. Transfers
    // past this point are left to finish -- the MSC side waits for
    // them in z_usbh_bus_reserve().
    if (ce->ctrl_state == CS_SETUP && bus_reserved)
        return ce->ctrl_state;

    if (xact_pending()) return ce->ctrl_state;
    // Another device's result is waiting to be read: its turn first.
    if (xact_owner >= 0 && xact_owner != d) return ce->ctrl_state;
    if ((int32_t)(usbh_ticks() - ce->ctrl_delay_until) < 0)
        return ce->ctrl_state;

    s = z_usbh_rd(Z_USBH_XACT_S);
    st = (uint8_t)Z_USBH_XS_STATUS(s);
    if (cap_on && !cap_frozen && xact_owner == d && dv->port == 0 &&
        (cap_mode == 0 ?
            (st == Z_USBH_ST_TIMEOUT || st == Z_USBH_ST_CRCERR ||
             st == Z_USBH_ST_BABBLE) :
            (cap_last_pid == Z_USBH_PID_IN && dv->parent >= 0 &&
             (dv->xa_flags & 1) && Z_USBH_XS_LEN(s) > 0 &&
             (st == Z_USBH_ST_OK || st == Z_USBH_ST_SHORT)))) {
        cap_frozen = 1;
        cap_addr = dv->addr;
        cap_state = dv->state;
        cap_stage = ce->ctrl_state;
        cap_status = st;
        cap_ls = dv->xa_flags & 1;
        cap_pid = cap_last_pid;
        cap_d0_before = cap_d0_last;
        cap_d1_before = cap_d1_last;
        cap_d0_after = z_usbh_rd(Z_USBH_DEBUG0);
        cap_d1_after = z_usbh_rd(Z_USBH_DEBUG1);
        printf("usb: capture frozen on a %s transaction -- "
               "run usbcapd\n", cap_mode ? "successful low-speed" :
               "failing");
    }
    // Whatever was ours in XACT_S is read now and judged below. If the
    // stage below launches another transaction, xact_start() makes this
    // device the owner again.
    xact_owner = -1;
    ctrl_cur = (int8_t)d;

    switch (ce->ctrl_state) {

    case CS_SETUP:
        // The SETUP token and its 8 data bytes always go out as DATA0.
        xact_start(Z_USBH_PID_SETUP, dv->addr, 0, dv->xa_flags,
                   dv->port, 0, 0, dv->mps0,
                   devs[d].scr, 8, nak_budget(dv));
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
                    // Exhausted, not silent. usbh_dev_fail() reads the
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
                // Return WITHOUT relaunching. This used to fall through
                // to the relaunch below in the same call, so the delay
                // only postponed judging the next result -- the retry
                // itself went out microseconds after the NAK. The next
                // call after the delay finds ctrl_pend clear and
                // launches. A slow low-speed keyboard behind a hub
                // answered NAK, then nothing, to retries that fast.
                break;
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
                    ctrl_send_status(d);
                    break;
                }
            }
        }
        {
            uint16_t want = ce->ctrl_len - ce->ctrl_xferred;
            if (want > dv->mps0) want = dv->mps0;
            xact_start(ce->ctrl_dir_in ? Z_USBH_PID_IN : Z_USBH_PID_OUT,
                       dv->addr, 0, dv->xa_flags, dv->port, ce->ctrl_tgl, 0,
                       dv->mps0, (devs[d].scr + 8u) + ce->ctrl_xferred, want,
                       nak_budget(dv));
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
                ctrl_send_status(d);
                break;
            }
            ce->ctrl_soft_naks++;
            ce->ctrl_delay_until = usbh_ticks() + CTRL_NAK_TICKS;
            // Resume the data stage AFTER the delay, a packet at a time,
            // from where the hardware stopped: CS_DATA_RUN with nothing
            // outstanding launches on its next call. This relaunched
            // immediately, like CS_DATA_RUN did.
            ce->ctrl_tgl = (uint8_t)Z_USBH_XS_TOGGLE(s);
            ce->ctrl_pend = 0;
            ce->ctrl_state = CS_DATA_RUN;
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
                   dv->mps0, (devs[d].scr + 8u), 0,
                   nak_budget(dv));
        ce->ctrl_pend = 1;
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
        // Nothing outstanding: a NAKed status stage that has waited out
        // its delay. Launch it again now.
        if (!ce->ctrl_pend) {
            xact_start(ce->ctrl_dir_in ? Z_USBH_PID_OUT : Z_USBH_PID_IN,
                       dv->addr, 0, dv->xa_flags, dv->port, 1, 0,
                       dv->mps0, (devs[d].scr + 8u), 0,
                       nak_budget(dv));
            ce->ctrl_pend = 1;
            break;
        }
        ce->ctrl_pend = 0;
        if (st == Z_USBH_ST_NAK && ce->ctrl_soft_naks < CTRL_SOFT_NAKS) {
            // Same for the status stage: a device that has not
            // finished acting on the request NAKs here, and
            // SET_ADDRESS is allowed 50 ms to complete. Relaunched
            // after the delay, at the top of this case.
            ce->ctrl_soft_naks++;
            ce->ctrl_delay_until = usbh_ticks() + CTRL_NAK_TICKS;
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
int usbh_ctrl_finished(int d)
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
int usbh_addr0_busy(void)
{
    return usbh_addr0_busy_except(-1);
}

int usbh_addr0_busy_except(int me)
{
    int i;
    for (i = 0; i < Z_USBH_MAX_DEVS; i++) {
        uint8_t st = devs[i].state;
        if (i == me) continue;
        // A device behind a hub that may still be on address 0 --
        // including one that FAILED there, whose port is still enabled.
        // The enumeration states below did not cover it, and on
        // hardware the next device got two answers to every SETUP on
        // address 0. A failed root-port device has a wire to itself and
        // does not count.
        if (devs[i].parent >= 0 && devs[i].on_addr0 && !devs[i].port_off)
            return 1;
        // From the reset that puts a device on address 0 until
        // SET_ADDRESS's status stage has moved it off it. E_DESC18 is
        // the first state that talks to the new address. Behind a hub
        // the reset is the hub's, and starts at E_HUB_RESET.
        if ((st > E_DEBOUNCE && st < E_DESC18) ||
            st == E_HUB_RESET || st == E_HUB_RECOVER)
            return 1;
    }
    return 0;
}

// ---------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------

// Record why, then fail. Called instead of assigning E_FAILED.
void usbh_dev_fail(z_usbh_dev_t *dv)
{
    // Behind a hub: ask the hub what it thinks of this port. On
    // hardware a device behind a hub stopped answering a clean token
    // (capture: correct PRE, IN, CRC5, EOP, then silence), which is what
    // a port the HUB has disabled -- babble or loss of activity at end
    // of frame -- or one whose device has suspended looks like. The hub
    // driver reads the port status and logs enabled / suspended.
    if (dv->parent >= 0 && dv->parent < Z_USBH_MAX_DEVS) {
        dv->fail_probe = 1;
        usbh_devs[dv->parent].h_changes |= (uint8_t)(1u << dv->hport);
    }

    uint32_t st = z_usbh_rd(Z_USBH_XACT_S);
    dv->fail_state = dv->state;
    dv->fail_status = (uint8_t)Z_USBH_XS_STATUS(st);
    dv->fail_naks = (uint8_t)Z_USBH_XS_NAKS(st);
    // ctrl_state has ALREADY advanced past the stage that failed:
    // usbh_ctrl_step() sets the next stage when it LAUNCHES a transaction
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
    // A failing device gives its scratch back so another can
    // enumerate; it takes a fresh one if it is retried. A hub that
    // fails takes everything behind it down too.
    usbh_scr_free(dv->scr);
    dv->scr = 0;
    // And its poll slot: a hub fails while running, with its status
    // endpoint slot still live.
    if (dv->slot >= 0) {
        z_usbh_wr(Z_USBH_POLL_A(dv->slot), 0);
        usbh_slot_free(dv->slot);
        dv->slot = -1;
    }
    {
        int me = (int)(dv - devs), j;
        for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++)
            if (devs[j].parent == me) usbh_teardown(j);
    }
    dv->state = E_FAILED;
}

void usbh_teardown(int i)
{
    int j;

    // A device torn down with a result outstanding must not leave the
    // engine owned by nobody who will ever read it.
    if (xact_owner == i) xact_owner = -1;

    // Everything behind a hub goes with it, deepest first. A hub pulled
    // from a root port takes its whole subtree; so does a hub pulled
    // from another hub.
    for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++)
        if (devs[j].parent == i && j != i) usbh_teardown(j);

    // Stop the auto-poll slot BEFORE anything else: it is running in
    // hardware against a device that is no longer there, and every poll
    // it makes now times out and takes bus time from everything else.
    if (devs[i].slot >= 0) {
        z_usbh_wr(Z_USBH_POLL_A(devs[i].slot), 0);
        usbh_slot_free(devs[i].slot);
    }
    usbh_scr_free(devs[i].scr);

    // Give back everything this device held. The compat block is the
    // one that bites if it is forgotten: blk_claim() has exactly two,
    // so a leak does not show up on the first unplug/replug, it shows
    // up on the third, as a device that enumerates perfectly and then
    // silently does not appear.
    if (devs[i].blk >= 0) z_usbh_hid_release(devs[i].blk);
    // The same for mass storage, and for more than tidiness: without
    // it the driver kept its address and endpoints after an unplug,
    // /usb stayed mounted and failed slowly, and a different device
    // later enumerating at that address would have been sent SCSI
    // commands.
    if (devs[i].cls == Z_USBH_CLASS_MSC) z_usbh_msc_unbind();
    if (devs[i].cls == Z_USBH_CLASS_CDC) z_usbh_cdc_unbind();
    addr_free(devs[i].pend_addr);
    if (devs[i].addr != devs[i].pend_addr) addr_free(devs[i].addr);
    memset(&devs[i], 0, sizeof(z_usbh_dev_t));
    // A root-port record stays bound to its port; a record behind a hub
    // becomes free (parent -1 and not a root index).
    devs[i].port = (uint8_t)(i < Z_USBH_MAX_PORTS ? i : 0);
    devs[i].parent = -1;
    devs[i].state = E_EMPTY;
    devs[i].mps0 = 8;
    devs[i].blk = -1;
    devs[i].iface = -1;
    devs[i].slot = -1;
}

// ---------------------------------------------------------------
// packet-buffer scratch and auto-poll slots
// ---------------------------------------------------------------
//
// Control transfers used to have one scratch area per ROOT PORT, at
// 0x400 and 0x600, indexed by the device's port number. With hubs a
// port can have several devices behind it and the index runs off the
// end of the 2 KB buffer. So scratch is now a small pool:
//
//   0x400, 0x600  512 each: taken by a device when it starts
//                 enumerating (the configuration descriptor lands
//                 here) and given back at bind. At most two devices
//                 enumerate at once; a third waits.
//   0x3c0, 0x3e0  32 each: kept by a HUB for its port requests (a
//                 4-byte port status, an 8-byte hub descriptor). Two
//                 hubs at most.
//
// 0x380-0x3bf is the auto-poll landing area and 0x000-0x26f belongs to
// mass storage; see usbh_hw.h and usbh_msc.c.

#define SCR_BIG0    0x400u
#define SCR_BIG1    0x600u
#define SCR_HUB0    0x3c0u
#define SCR_HUB1    0x3e0u

static uint8_t scr_used;        // bit per area, in the order above
static uint8_t slot_used;       // bit per auto-poll slot

static uint16_t scr_take(uint16_t a, uint8_t abit, uint16_t b,
                         uint8_t bbit)
{
    if (!(scr_used & abit)) { scr_used |= abit; return a; }
    if (!(scr_used & bbit)) { scr_used |= bbit; return b; }
    return 0;
}

uint16_t usbh_scr_take_big(void)
{
    return scr_take(SCR_BIG0, 1, SCR_BIG1, 2);
}

uint16_t usbh_scr_take_hub(void)
{
    return scr_take(SCR_HUB0, 4, SCR_HUB1, 8);
}

void usbh_scr_free(uint16_t off)
{
    if (off == SCR_BIG0) scr_used &= (uint8_t)~1;
    if (off == SCR_BIG1) scr_used &= (uint8_t)~2;
    if (off == SCR_HUB0) scr_used &= (uint8_t)~4;
    if (off == SCR_HUB1) scr_used &= (uint8_t)~8;
}

// Four hardware slots. Keyboard, mouse, and a hub's status change
// endpoint are the users. They used to be indexed by root port.
int usbh_slot_take(void)
{
    int s;
    for (s = 0; s < 4; s++)
        if (!(slot_used & (1u << s))) {
            slot_used |= (uint8_t)(1u << s);
            return s;
        }
    return -1;
}

void usbh_slot_free(int s)
{
    if (s >= 0 && s < 4) slot_used &= (uint8_t)~(1u << s);
}

// -- devices behind hubs --

int usbh_child_find(int h, int hport)
{
    int j;
    for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++)
        if (devs[j].parent == h && devs[j].hport == hport) return j;
    return -1;
}

// A free record for a device just connected to hub h, port hport,
// or -1 if the table is full (the device is then left alone).
int usbh_child_new(int h, int hport)
{
    int j;
    for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++) {
        if (devs[j].parent >= 0) continue;
        usbh_teardown(j);               // clean record
        devs[j].parent = (int8_t)h;
        devs[j].hport = (uint8_t)hport;
        devs[j].depth = (uint8_t)(devs[h].depth + 1);
        devs[j].port = devs[h].port;    // the ROOT port it is behind
        devs[j].deadline = usbh_ticks() + 73;   // 100 ms debounce
        devs[j].state = E_HUB_DEBOUNCE;
        return j;
    }
    printf("usb: device table full, hub %d port %d ignored\n",
           devs[h].addr, hport);
    return -1;
}

static void dev_step(int i, uint32_t ps);

static void port_step(int i)
{
    z_usbh_dev_t *dv = &devs[i];
    uint32_t ps = Z_USBH_PS(z_usbh_rd(Z_USBH_PORTSTAT), i);

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
            usbh_teardown(i);
        }
        return;
    }

    dev_step(i, ps);
}

// One step of device i's enumeration. ps is its root port's status for
// a root-port device and unused for one behind a hub.
static void dev_step(int i, uint32_t ps)
{
    z_usbh_dev_t *dv = &devs[i];
    uint32_t d = Z_USBH_BUF + dv->scr + 8u;
    int a;

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
        // It also waits for a scratch area to enumerate into.
        if (!usbh_addr0_busy()) {
            dv->scr = usbh_scr_take_big();
            if (dv->scr) dv->state = E_RESET;
        }
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
            usbh_ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                       (DESC_DEVICE << 8), 0, 8);
            dv->state = E_DESC8;
        } else if ((int32_t)(usbh_ticks() - dv->deadline) > 0) {
            usbh_dev_fail(dv);
        }
        break;

    case E_DESC8:
        if (usbh_ctrl_step(i) == CS_ERROR) { usbh_dev_fail(dv); break; }
        if (!usbh_ctrl_finished(i)) break;
        dv->mps0 = z_usbh_rb(d + 7);
        dv->d_class = z_usbh_rb(d + 4);
        if (dv->mps0 < 8 || dv->mps0 > 64) dv->mps0 = 8;
        // One reserved address per device, kept across retries and freed
        // only at teardown. A retry used to free it first -- but a
        // device whose port reset then failed still HELD it, and the
        // next device to enumerate was given the same address.
        a = dv->pend_addr ? dv->pend_addr : addr_alloc();
        if (!a) { usbh_dev_fail(dv); break; }
        dv->addr = 0;
        usbh_ctrl_begin(i, 0, REQ_SET_ADDRESS, (uint16_t)a, 0, 0);
        dv->pend_addr = (uint8_t)a;
        dv->state = E_SET_ADDR;
        break;

    case E_SET_ADDR:
        if (usbh_ctrl_step(i) == CS_ERROR) { usbh_dev_fail(dv); break; }
        if (!usbh_ctrl_finished(i)) break;
        // A device only adopts its new address once the status stage
        // has completed, so this assignment cannot happen earlier.
        dv->addr = dv->pend_addr;
        dv->on_addr0 = 0;
        dv->deadline = usbh_ticks() + 4;
        dv->state = E_ADDR_SETTLE;
        break;

    case E_ADDR_SETTLE:
        // USB gives a device 2 ms to switch addresses. Talking to it
        // before that is a transaction that mysteriously times out.
        if ((int32_t)(usbh_ticks() - dv->deadline) < 0) break;
        usbh_ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                   (DESC_DEVICE << 8), 0, 18);
        dv->state = E_DESC18;
        break;

    case E_DESC18:
        if (usbh_ctrl_step(i) == CS_ERROR) { usbh_dev_fail(dv); break; }
        if (!usbh_ctrl_finished(i)) break;
        // Full device descriptor is in the buffer now.
        dv->d_vid_lo = z_usbh_rb(d + 8);
        dv->d_vid_hi = z_usbh_rb(d + 9);
        dv->d_pid_lo = z_usbh_rb(d + 10);
        dv->d_pid_hi = z_usbh_rb(d + 11);
        dv->d_nconf = z_usbh_rb(d + 17);
        dv->got_desc = 1;
        usbh_ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                   (DESC_CONFIG << 8), 0, 9);
        dv->state = E_CONFIG9;
        break;

    case E_CONFIG9:
        if (usbh_ctrl_step(i) == CS_ERROR) { usbh_dev_fail(dv); break; }
        if (!usbh_ctrl_finished(i)) break;
        // wTotalLength. Clamped to the scratch region rather than
        // trusted: a descriptor longer than the buffer is a device
        // bug, and reading it would walk into the sector area.
        dv->cfg_len = z_usbh_rb(d + 2);
        if (z_usbh_rb(d + 3) || dv->cfg_len > 200) dv->cfg_len = 200;
        usbh_ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                   (DESC_CONFIG << 8), 0, dv->cfg_len);
        dv->state = E_CONFIG_ALL;
        break;

    case E_CONFIG_ALL:
        if (usbh_ctrl_step(i) == CS_ERROR) { usbh_dev_fail(dv); break; }
        if (!usbh_ctrl_finished(i)) break;
        dv->cfg_val = z_usbh_rb(d + 5);
        dv->cfg_nif = z_usbh_rb(d + 4);
        dv->cfg_attr = z_usbh_rb(d + 7);
        dv->got_cfg = 1;
        {
            int k;
            dv->cfg_rawn = dv->cfg_len > sizeof(dv->cfg_raw) ?
                           (uint8_t)sizeof(dv->cfg_raw) : (uint8_t)dv->cfg_len;
            for (k = 0; k < dv->cfg_rawn; k++)
                dv->cfg_raw[k] = z_usbh_rb(d + k);
        }
        usbh_ctrl_begin(i, 0, REQ_SET_CONFIG, dv->cfg_val, 0, 0);
        dv->state = E_SET_CONFIG;
        break;

    case E_SET_CONFIG:
        if (usbh_ctrl_step(i) == CS_ERROR) { usbh_dev_fail(dv); break; }
        if (!usbh_ctrl_finished(i)) break;
        dv->iface = (int8_t)z_usbh_hid_probe(dv->cfg_raw, dv->cfg_rawn);
        if (dv->iface < 0) {
            // CDC-ACM: set the line coding before binding -- 115200
            // 8N1, little-endian, in the data stage.
            a = z_usbh_cdc_probe(dv->cfg_raw, dv->cfg_rawn);
            if (a >= 0) {
                static const uint8_t lc[7] = { 0x00, 0xc2, 0x01, 0x00,
                                               0x00, 0x00, 0x08 };
                int k;
                for (k = 0; k < 7; k++) z_usbh_wb(d + (uint32_t)k, lc[k]);
                dv->iface = (int8_t)a;
                usbh_ctrl_begin(i, HID_OUT_IFACE, 0x20, 0, (uint16_t)a, 7);
                dv->state = E_CDC_LINE;
                break;
            }
            dv->state = E_BIND;
            break;
        }
        // Boot protocol, explicitly. A device whose interface declares
        // subclass 1 usually defaults to it, and "usually" is the
        // problem: a keyboard that comes up in report protocol sends a
        // layout the hardware's byte mux is not expecting, and the
        // symptom is keycodes in the wrong bytes rather than nothing
        // working.
        usbh_ctrl_begin(i, HID_OUT_IFACE, REQ_SET_PROTOCOL, 0,
                   (uint16_t)dv->iface, 0);
        dv->state = E_SET_PROTO;
        break;

    case E_SET_PROTO:
        // A STALL here is not fatal -- plenty of mice refuse
        // SET_PROTOCOL and are in boot protocol anyway. Carry on and
        // let the report layout speak for itself.
        if (usbh_ctrl_step(i) == CS_ERROR || usbh_ctrl_finished(i)) {
            usbh_ctrl_begin(i, HID_OUT_IFACE, REQ_SET_IDLE, 0,
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
        if (usbh_ctrl_step(i) == CS_ERROR || usbh_ctrl_finished(i))
            dv->state = E_BIND;
        break;

    // -- CDC-ACM --
    // A device may STALL either request; neither failure stops the bind.
    case E_CDC_LINE:
        if (usbh_ctrl_step(i) == CS_ERROR || usbh_ctrl_finished(i)) {
            // DTR and RTS: many devices send nothing until DTR is set.
            usbh_ctrl_begin(i, HID_OUT_IFACE, 0x22, 0x0003,
                            (uint16_t)dv->iface, 0);
            dv->state = E_CDC_DTR;
        }
        break;

    case E_CDC_DTR:
        if (usbh_ctrl_step(i) == CS_ERROR || usbh_ctrl_finished(i)) {
            usbh_scr_free(dv->scr);
            dv->scr = 0;
            if (z_usbh_cdc_bind(dv->addr, dv->xa_flags, dv->port,
                                dv->cfg_raw, dv->cfg_rawn))
                dv->cls = Z_USBH_CLASS_CDC;
            usbh_any_ready = 1;
            dv->state = E_RUNNING;
        }
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
        //
        // Enumeration is over, so the scratch goes back for the next
        // device. A hub takes a small one of its own in usbh_hub_bind().
        usbh_scr_free(dv->scr);
        dv->scr = 0;
        // A retried device must not keep the class of a previous bind.
        dv->cls = Z_USBH_CLASS_NONE;

        if (usbh_hub_probe(dv)) {
            if (usbh_hub_bind(i)) dv->cls = Z_USBH_CLASS_HUB;
            usbh_any_ready = 1;
            dv->state = E_RUNNING;
            break;
        }

        // The auto-poll slot is allocated, not the device's index:
        // with hubs there are more devices than slots.
        dv->slot = (int8_t)usbh_slot_take();
        dv->blk = dv->slot < 0 ? -1 :
                  (int8_t)z_usbh_hid_bind(dv->slot, dv->addr,
                                          dv->xa_flags, dv->port,
                                          dv->cfg_raw, dv->cfg_rawn);
        if (dv->blk < 0 && dv->slot >= 0) {
            usbh_slot_free(dv->slot);
            dv->slot = -1;
        }
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
            // The reserved address is NOT freed: the device may still hold
            // it until its next reset, and it is reused on the retry.
            dv->addr = 0;
            // Behind a hub, retrying means asking the hub to reset the
            // port again; at a root port it means starting over.
            dv->state = dv->parent >= 0 ? E_HUB_WAIT : E_EMPTY;
            dv->fail_state = fs;
            dv->fail_status = fx;
            dv->fail_ctrl = fc;
            dv->fail_naks = fn;
            dv->retries = r;
        }
        break;

    // -- behind a hub; usbh_hub.c moves a child between these --

    case E_HUB_DEBOUNCE:
        // Stable for 100 ms, as for a root port (USB 2.0 7.1.7.3). A
        // bounce shows up as another connect change on the hub and
        // replaces this record.
        if ((int32_t)(usbh_ticks() - dv->deadline) >= 0)
            dv->state = E_HUB_WAIT;
        break;

    case E_HUB_WAIT:
    case E_HUB_RESET:
        break;          // the hub driver takes it from here

    case E_HUB_RECOVER:
        // 10 ms reset recovery (TRSTRCY), then it enumerates exactly as
        // a root-port device does.
        if ((int32_t)(usbh_ticks() - dv->deadline) < 0) break;
        usbh_ctrl_begin(i, DIR_IN, REQ_GET_DESCRIPTOR,
                        (DESC_DEVICE << 8), 0, 8);
        dv->state = E_DESC8;
        break;

    case E_RUNNING:
        if (dv->cls == Z_USBH_CLASS_HUB) usbh_hub_step(i);
        break;

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

    // Every record, root ports and the pool behind hubs, from scratch.
    for (i = Z_USBH_MAX_DEVS - 1; i >= 0; i--) {
        devs[i].blk = -1;
        devs[i].slot = -1;
        devs[i].parent = -1;
        devs[i].scr = 0;
        usbh_teardown(i);
    }
    scr_used = 0;
    slot_used = 0;
    xact_owner = -1;

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
    // Devices behind hubs have no root port of their own to watch; the
    // hub driver reports their connects and disconnects.
    for (i = Z_USBH_MAX_PORTS; i < Z_USBH_MAX_DEVS; i++)
        if (devs[i].parent >= 0) dev_step(i, 0);
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
    case E_HUB_DEBOUNCE: return "hub-debounce";
    case E_HUB_WAIT:    return "hub-wait";
    case E_HUB_RESET:   return "hub-reset";
    case E_HUB_RECOVER: return "hub-recover";
    case E_CDC_LINE:    return "cdc-line-coding";
    case E_CDC_DTR:     return "cdc-dtr";
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

// One device's entry in lsusb, then the devices behind it if it is a
// hub. pre is the line prefix; each hub level indents two more.
static void dump_dev(int i, const char *pre)
{
    uint32_t pa, pb;
    int j;
    static char sub[3][32];

    printf("%sstate=%s addr=%d mps0=%d", pre,
           state_name(devs[i].state), devs[i].addr, devs[i].mps0);
    if (devs[i].blk >= 0)
        printf(" block=%d typ=%s", devs[i].blk,
           typ_name(devs[i].blk == 0 ?
                (int)((z_usbh_rd(Z_USBH_HID0_INFO) >> 24) & 3) :
                (int)((z_usbh_rd(Z_USBH_HID1_INFO) >> 24) & 3)));
    if (devs[i].cls == Z_USBH_CLASS_MSC) printf(" class=msc");
    if (devs[i].cls == Z_USBH_CLASS_CDC) printf(" class=cdc");
    if (devs[i].retries) printf(" retries=%d", devs[i].retries);
    if (devs[i].recoveries)
        printf(" recovered=%d", devs[i].recoveries);   // port disabled by hub
    printf("\n");

    {
        int a;
        for (a = 0; a < devs[i].att_n; a++)
        printf("%sattempt %d: %s, %s: %s (%d nak%s)\n", pre,
               a + 1, state_name(devs[i].att_state[a]),
               devs[i].att_ctrl[a] == CE_SETUP ? "SETUP stage" :
               devs[i].att_ctrl[a] == CE_DATA ? "DATA stage" :
               devs[i].att_ctrl[a] == CE_STATUS ? "STATUS stage" :
               "before any transaction",
               status_name(devs[i].att_status[a]),
               devs[i].att_naks[a],
               devs[i].att_naks[a] == 1 ? "" : "s");
        for (a = 0; a < devs[i].att_n; a++)
        printf("%s  %d: act_len=%d toggle got=%d want=%d\n", pre,
               a + 1, devs[i].att_alen[a], devs[i].att_tgl[a],
               devs[i].att_want[a]);
    }

    if (devs[i].got_desc)
        printf("%sdevice %04x:%04x class %d, %d config(s)\n", pre,
           (unsigned)(devs[i].d_vid_lo |
                  (devs[i].d_vid_hi << 8)),
           (unsigned)(devs[i].d_pid_lo |
                  (devs[i].d_pid_hi << 8)),
           devs[i].d_class, devs[i].d_nconf);
    if (devs[i].got_cfg)
        printf("%sconfig value %d, %d interface(s), "
               "attributes %02x\n", pre,
           devs[i].cfg_val, devs[i].cfg_nif, devs[i].cfg_attr);

    if (devs[i].got_cfg && devs[i].blk < 0) {
        int k;
        printf("%scfg desc (%d of %d bytes):", pre,
           devs[i].cfg_rawn, devs[i].cfg_len);
        for (k = 0; k < devs[i].cfg_rawn; k++) {
        if ((k & 15) == 0) printf("\n%s ", pre);
        printf(" %02x", devs[i].cfg_raw[k]);
        }
        printf("\n");
    }

    if (devs[i].state == E_FAILED)
        printf("%sfailed in %s, %s: %s (%d nak%s)\n", pre,
           state_name(devs[i].fail_state),
           devs[i].fail_ctrl == CE_SETUP ? "SETUP stage" :
           devs[i].fail_ctrl == CE_DATA ? "DATA stage" :
           devs[i].fail_ctrl == CE_STATUS ? "STATUS stage" :
           "before any transaction",
           status_name(devs[i].fail_status),
           devs[i].fail_naks, devs[i].fail_naks == 1 ? "" : "s");

    if (devs[i].cls == Z_USBH_CLASS_HUB) {
        printf("%shub: %d port(s), state %d, changes %02x, "
               "failed requests %u\n", pre, devs[i].h_nports,
               devs[i].h_state, devs[i].h_changes, devs[i].h_fails);
        printf("%shub: ports disabled by the hub %u, recovered %u\n", pre,
               devs[i].h_hubdis, devs[i].h_recov);
    }

    if (devs[i].slot >= 0) {
        pa = z_usbh_rd(Z_USBH_POLL_A(devs[i].slot));
        pb = z_usbh_rd(Z_USBH_POLL_B(devs[i].slot));
        printf("%spoll slot %d: ep%lu every %lu frame(s), "
               "mode %lu, last status %lu\n", pre, devs[i].slot,
               (unsigned long)((pa >> 7) & 0xf),
               (unsigned long)((pa >> 22) & 0xff),
               (unsigned long)((pb >> 11) & 7),
               (unsigned long)Z_USBH_PB_STATUS(pb));
    }

    for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++) {
        if (devs[j].parent != i) continue;
        printf("%shub port %d:\n", pre, devs[j].hport);
        if (devs[j].depth <= 3) {
            snprintf(sub[devs[j].depth - 1], sizeof(sub[0]), "%s  ", pre);
            dump_dev(j, sub[devs[j].depth - 1]);
        }
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
    uint32_t cfg, ps;
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
        // Frames in which port 0's line was not idle in the last 4 us
        // before the frame tick, where a hub's EOF points are. A hub
        // disables a port for activity there (USB 2.0 11.8.1).
        printf("usb: wire: %lu frame(s) with port 0 busy at end of frame\n",
               (unsigned long)(d1 >> 16));
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

        dump_dev(i, "usb:   ");

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
    for (i = 0; i < Z_USBH_MAX_PORTS; i++) usbh_teardown(i);
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
    // A fixed area, not a device's scratch: scratch is allocated now,
    // and devs[0].scr may be 0 -- the storage sector buffer. 0x400 is
    // enumeration scratch, so run this with nothing enumerating.
    uint32_t base = Z_USBH_BUF + SCR_BIG0;
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
    {
        static const uint8_t su[8] = { 0x80, 0x06, 0x00, 0x01,
                                       0x00, 0x00, 0x08, 0x00 };
        for (i = 0; i < 8; i++) z_usbh_wb(base + i, su[i]);
    }
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
    for (i = 0; i < Z_USBH_MAX_PORTS; i++) usbh_teardown(i);
}

// A control stage has been launched and its result not yet consumed,
// on any device. CS_SETUP has not launched anything yet; CS_DONE and
// CS_ERROR are reached only after the last result was read.
static int ctrl_on_bus(void)
{
    int i;
    for (i = 0; i < Z_USBH_MAX_DEVS; i++) {
        uint8_t c = devs[i].ctrl_state;
        if (c == CS_DATA || c == CS_DATA_RUN ||
            c == CS_STATUS || c == CS_FINAL)
            return 1;
    }
    return 0;
}

// Bounds the wait for a control transfer already under way, in kernel
// ticks (~1.4 ms): about 200 ms. Longer than any control transfer takes
// unless its device is NAKing it, and then the storage command fails
// rather than waiting that out.
//
// TIME, not iterations. The first version counted loop passes, and a
// pass that short-circuits past the bus read costs almost nothing: in
// co-simulation 400000 of them took no time at all and gave up before
// the control transfer could move. The ticks advance here because the
// ktimer ISR still runs -- FatFs holds off the scheduler, not
// interrupts. BUS_RESERVE_SPINS is only a backstop in case they do
// not.
#define BUS_RESERVE_TICKS   150u
#define BUS_RESERVE_SPINS   2000000u

int z_usbh_bus_reserve(void)
{
    uint32_t n, t0;
    int pending;

    // Set FIRST, then wait. From this store on, the ISR starts nothing
    // new; anything it had already started shows in ctrl_on_bus()
    // until it has read that transfer's final result. No interrupt
    // masking is needed because the ISR runs to completion between
    // any two of these reads.
    t0 = usbh_ticks();
    bus_reserved = 1;
    for (n = 0; n < BUS_RESERVE_SPINS; n++) {
        // The bus read comes first and always happens, so every pass
        // costs a real register access.
        pending = xact_pending();
        if (!pending && !ctrl_on_bus()) return 1;
        if ((uint32_t)(usbh_ticks() - t0) > BUS_RESERVE_TICKS) break;
    }
    bus_reserved = 0;
    // Say who was holding it. This is the one question worth asking
    // when a storage command fails this way.
    printf("usb: bus held: xact_s %08lx\n",
           (unsigned long)z_usbh_rd(Z_USBH_XACT_S));
    for (n = 0; n < Z_USBH_MAX_DEVS; n++)
        if (devs[n].state != E_EMPTY)
        printf("usb: bus held: dev %lu state %d ctrl %d pend %d "
               "delay %ld\n", (unsigned long)n, devs[n].state,
               devs[n].ctrl_state, devs[n].ctrl_pend,
               (long)(devs[n].ctrl_delay_until - usbh_ticks()));
    return 0;
}

void z_usbh_bus_release(void)
{
    bus_reserved = 0;
}

int z_usbh_cap_start(int mode)
{
    (void)mode;
#ifndef CAP_HAVE_PROBE
    return 0;
#else
    uint32_t words = cap_rd(0) >> 16;
    if (words == 0 || words > 4096) {
        printf("usbcap: probe not built in (rebuild with -DPROBE)\n");
        return 0;
    }
    cap_frozen = 0;
    cap_mode = (uint8_t)(mode ? 1 : 0);
    cap_on = 1;
    if (cap_mode)
        printf("usbcap: armed; freezes on the first successful IN with "
               "data from a low-speed device behind a hub -- plug one "
               "in, then usbcapd\n");
    else
        printf("usbcap: armed on every transaction on port 0; plug the "
               "device in, then usbcapd\n");
    return 1;
#endif
}

void z_usbh_cap_dump(void)
{
#ifdef CAP_HAVE_PROBE
    uint32_t words, i;
#endif
    if (!cap_frozen) {
        printf("usbcap: nothing captured%s\n",
               cap_on ? " yet -- no transaction has failed" :
                        " -- run usbcap first");
        return;
    }
    // The header line is what tools/usbcap.py keys on; keep the format.
    // The PID says which transaction failed; the stage alone does not
    // (ctrl_step judges a stage's result in the NEXT state).
    printf("usbcap: addr %d pid %s state %s stage %d status %s speed %s\n",
           cap_addr, cap_pid == Z_USBH_PID_SETUP ? "SETUP" :
                     cap_pid == Z_USBH_PID_IN ? "IN" : "OUT",
           state_name(cap_state), cap_stage,
           status_name(cap_status), cap_ls ? "low" : "full");
    // Across the failing transaction (and any SOF or auto-poll that ran
    // alongside it).
    printf("usbcap: during it: rx started +%lu, good +%lu, bad +%lu\n",
           (unsigned long)(((cap_d0_after >> 16) - (cap_d0_before >> 16)) & 0xffff),
           (unsigned long)((cap_d1_after - cap_d1_before) & 0xff),
           (unsigned long)(((cap_d1_after >> 8) - (cap_d1_before >> 8)) & 0xff));
#ifdef CAP_HAVE_PROBE
    // The window may still be recording what followed the failure.
    for (i = 0; i < 1000000u && (cap_rd(0) & 2u); i++) { }
    words = cap_rd(0) >> 16;
    printf("probe: %lu words, 16 samples each, D+ in the upper bit of "
           "each pair\n", (unsigned long)words);
    for (i = 0; i < words; i++) {
        cap_wr(4, i);
        (void)cap_rd(8);
        if ((i & 7) == 0) printf("%04lx:", (unsigned long)i);
        printf(" %08lx", (unsigned long)cap_rd(8));
        if ((i & 7) == 7) printf("\n");
    }
#endif
    cap_on = 0;
}

int z_usbh_ready(void)
{
    return usbh_any_ready;
}

int z_usbh_status(uint8_t *typ, int max)
{
    int i, n = 0;
    if (!usbh_present) return 0;
    for (i = 0; i < Z_USBH_MAX_DEVS && n < max; i++) {
        if (devs[i].state == E_RUNNING) {
            typ[n++] = devs[i].cls;
        }
    }
    return n;
}

// -- low-speed timing knobs (bring-up) --
//
// Every part of a low-speed transaction through a hub matches the two
// reference hosts (Pico-PIO-USB, TinyUSB on the RP2040) in kind; these
// four differ from them in size. Adjustable here so they can be swept on
// hardware without a gateware rebuild per value. docs/usb_host.md,
// "Known issues", item 5.
void z_usbh_tune_show(void)
{
    uint32_t t = z_usbh_rd(Z_USBH_TUNE);
    unsigned tmo = t & 0xff, turn = (t >> 8) & 0xff, gap = (t >> 16) & 0xff;
    printf("usb: low-speed timeout %u.%u us, turnaround %u.%u us, "
           "gap after a low-speed packet %u.%u us, J after PRE %lu bits\n",
           tmo / 6, (tmo % 6) * 10 / 6, turn / 6, (turn % 6) * 10 / 6,
           gap / 6, (gap % 6) * 10 / 6, (unsigned long)((t >> 24) & 0xf));
}

int z_usbh_tune_set(int tmo_us, int turn_us, int gap_us, int pre_bits)
{
    if (tmo_us < 1 || tmo_us > 42 || turn_us < 1 || turn_us > 42 ||
        gap_us < 1 || gap_us > 42 || pre_bits < 1 || pre_bits > 15) {
        printf("usbtune: timeout/turn/gap 1..42 us, PRE gap 1..15 bits\n");
        return 0;
    }
    z_usbh_wr(Z_USBH_TUNE, (uint32_t)(tmo_us * 6) |
                           ((uint32_t)(turn_us * 6) << 8) |
                           ((uint32_t)(gap_us * 6) << 16) |
                           ((uint32_t)pre_bits << 24));
    z_usbh_tune_show();
    return 1;
}
