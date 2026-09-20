/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB hub class driver. See docs/usb_host.md, "Ports, hubs and
 * topology" and "Hub class driver".
 *
 * This is a full-speed host, so a hub is a repeater with a port
 * controller: no transaction translators, no split transactions. What
 * it takes is ordinary control requests to the hub's own address, an
 * interrupt-IN endpoint reporting which ports changed (an auto-poll
 * slot in RAW mode), and PRE for low-speed devices behind it -- which
 * the SIE already does, selected per device by USE_PRE in xa_flags.
 *
 * Like the rest of the driver this is a state machine that does at
 * most one thing per call: it runs from the ISR and the ktimer, and
 * must never wait. One machine per hub, stored in the hub's own device
 * record (h_* in usbh_int.h), using that record's control engine for
 * every request. Every request therefore goes through ctrl_step(), so
 * the rule that mass storage owns the transaction engine for a whole
 * SCSI command (usbh.h) covers hub traffic without anything here.
 *
 * Devices behind the hub get their own records, created here on a
 * connect and torn down on a disconnect; usbh.c enumerates them from
 * E_DESC8 on exactly as it does a root-port device.
 */

#include <stdio.h>
#include <string.h>

#include "usbh.h"
#include "usbh_hw.h"
#include "usbh_int.h"

// -- hub class, USB 2.0 chapter 11 --

#define HUB_CLASS           0x09

#define RT_HUB_OUT          0x20    // class, recipient device (the hub)
#define RT_HUB_IN           0xa0
#define RT_PORT_OUT         0x23    // class, recipient "other" (a port)
#define RT_PORT_IN          0xa3

#define RQ_GET_STATUS       0x00
#define RQ_CLEAR_FEATURE    0x01
#define RQ_SET_FEATURE      0x03
#define RQ_GET_DESCRIPTOR   0x06

#define DT_HUB              0x29

#define F_C_HUB_LOCAL_POWER 0
#define F_C_HUB_OVER_CURRENT 1
#define F_PORT_RESET        4
#define F_PORT_POWER        8
#define F_C_PORT_CONNECTION 16      // C_PORT_* = 16 + change bit number

// wPortStatus
#define PS_CONNECTION       0x0001
#define PS_ENABLE           0x0002
#define PS_LOW_SPEED        0x0200

// wPortChange
#define PC_CONNECTION       0x0001
#define PC_ENABLE           0x0002  // C_PORT_ENABLE: the hub disabled it
#define PC_RESET            0x0010
#define PC_ALL              0x001f

// xa_flags as usbh.c builds them: bit 0 LOWSPEED, 1 INVERTED, 2 USE_PRE.
// A low-speed device behind a hub is LOWSPEED|USE_PRE and NOT inverted:
// the segment between us and the hub is full speed, driven by the hub.
#define XA_LS_BEHIND_HUB    0x05

// Deepest hub whose ports we serve. The spec allows five tiers; three
// covers anything real and bounds the device table. See the topology
// table in docs/usb_host.md.
#define HUB_MAX_DEPTH       3

// Consecutive failed hub requests before the hub is given up on. A NAK
// is already retried by the control engine; this counts STALLs and
// timeouts, which a working hub does not produce.
#define HUB_MAX_ERRS        8

// -- the machine --

#define HS_DESC         0
#define HS_DESC_W       1
#define HS_POWER        2
#define HS_POWER_W      3
#define HS_SETTLE       4
#define HS_IDLE         5
#define HS_PSTAT_W      6
#define HS_PCLR         7
#define HS_PCLR_W       8
#define HS_RESET_W      9
#define HS_RESET_POLL   10
#define HS_RESET_ST_W   11
#define HS_RESET_CLR_W  12
#define HS_HUBCLR_W     13
#define HS_DISABLE_W    14

#define F_PORT_ENABLE       1

// Is dv a hub? Device class 9, or an interface of class 9 -- some hubs
// put it in only one of the two. Also finds the status change endpoint.
int usbh_hub_probe(const z_usbh_dev_t *dv)
{
    int i = 0, in_hub = (dv->d_class == HUB_CLASS);

    while (i + 1 < dv->cfg_rawn) {
        int len = dv->cfg_raw[i], type = dv->cfg_raw[i + 1];
        if (len < 2) break;
        if (type == 0x04 && i + 5 < dv->cfg_rawn)
            in_hub = (dv->cfg_raw[i + 5] == HUB_CLASS) ||
                     (dv->d_class == HUB_CLASS);
        i += len;
    }
    return in_hub;
}

// The first interrupt-IN endpoint in the configuration is the status
// change endpoint; a hub has exactly one.
static int find_status_ep(z_usbh_dev_t *dv)
{
    int i = 0;
    while (i + 1 < dv->cfg_rawn) {
        int len = dv->cfg_raw[i], type = dv->cfg_raw[i + 1];
        if (len < 2) break;
        if (type == 0x05 && i + 6 < dv->cfg_rawn &&
            (dv->cfg_raw[i + 2] & 0x80) &&
            (dv->cfg_raw[i + 3] & 0x03) == 0x03) {
            dv->h_ep = dv->cfg_raw[i + 2] & 0x0f;
            dv->h_mps = dv->cfg_raw[i + 4];
            dv->h_ival = dv->cfg_raw[i + 6];
            return 1;
        }
        i += len;
    }
    return 0;
}

int usbh_hub_bind(int h)
{
    z_usbh_dev_t *dv = &usbh_devs[h];

    if (dv->depth >= HUB_MAX_DEPTH) {
        printf("usb: hub at depth %d not served (limit %d)\n",
               dv->depth, HUB_MAX_DEPTH);
        return 0;
    }
    if (!find_status_ep(dv)) {
        printf("usb: hub %d has no status endpoint\n", dv->addr);
        return 0;
    }
    dv->scr = usbh_scr_take_hub();
    if (!dv->scr) {
        printf("usb: hub %d not served, two hubs already\n", dv->addr);
        return 0;
    }
    if (dv->h_mps == 0 || dv->h_mps > 8) dv->h_mps = 1;
    if (dv->h_ival == 0) dv->h_ival = 12;
    dv->h_state = HS_DESC;
    dv->h_changes = 0;
    dv->h_errs = 0;
    return 1;
}

// Status and change of the port just read, from the scratch area.
static void read_pstat(z_usbh_dev_t *dv)
{
    uint32_t d = Z_USBH_BUF + dv->scr + 8u;
    dv->h_pstat = (uint16_t)(z_usbh_rb(d + 0) | (z_usbh_rb(d + 1) << 8));
    dv->h_pchg = (uint16_t)(z_usbh_rb(d + 2) | (z_usbh_rb(d + 3) << 8));
}

// Poll the hub's current request: 1 done, -1 failed, 0 in progress.
static int req(int h)
{
    int st = usbh_ctrl_step(h);
    if (st == CS_ERROR) return -1;
    return usbh_ctrl_finished(h) ? 1 : 0;
}

// The reset under way on h_port is being abandoned: put its device
// back to waiting, so it is reset again later, and give back the
// address-0 claim and scratch it holds. Leaving it in E_HUB_RESET held
// address 0 for ever -- co-simulation showed the whole tree stuck
// behind one abandoned reset.
static void reset_abort(int h)
{
    z_usbh_dev_t *dv = &usbh_devs[h];
    int j = usbh_child_find(h, dv->h_port);

    if (j >= 0 && usbh_devs[j].state == E_HUB_RESET) {
        usbh_scr_free(usbh_devs[j].scr);
        usbh_devs[j].scr = 0;
        usbh_devs[j].state = E_HUB_WAIT;
    }
}

// A hub request failed. Go back to idle and try that port again, unless
// the hub keeps failing, in which case give up on it -- and on
// everything behind it (usbh_dev_fail does that).
static void hub_err(int h, const char *what)
{
    z_usbh_dev_t *dv = &usbh_devs[h];
    uint32_t xs = z_usbh_rd(Z_USBH_XACT_S);

    // Counted, not printed: behind a real hub an occasional request
    // times out and succeeds on the retry (docs/usb_host.md, "Known
    // issues"), and a line per retry buried everything else. lsusb shows
    // the running count; the line below appears only when the hub is
    // given up.
    dv->h_fails++;
    if (++dv->h_errs > HUB_MAX_ERRS) {
        printf("usb: hub %d: %s failed (port %d, stage %d, status %lu) "
               "%d times in a row, hub given up\n", dv->addr, what,
               dv->h_port, dv->ctrl_err_stage,
               (unsigned long)Z_USBH_XS_STATUS(xs), dv->h_errs);
        usbh_dev_fail(dv);
        return;
    }
    if (dv->h_state >= HS_RESET_W && dv->h_state <= HS_RESET_CLR_W)
        reset_abort(h);
    if (dv->h_port) dv->h_changes |= (uint8_t)(1u << dv->h_port);
    dv->h_state = HS_IDLE;
}

static void arm_status_slot(int h)
{
    z_usbh_dev_t *dv = &usbh_devs[h];
    int s = usbh_slot_take();

    dv->slot = (int8_t)s;
    if (s < 0) {
        // No slot: fall back to reading every port's status now and
        // then. Slower to notice a plug, but it works.
        printf("usb: hub %d: no poll slot, polling ports instead\n",
               dv->addr);
        return;
    }
    z_usbh_wr(Z_USBH_POLL_B(s), Z_USBH_PB_OFF(Z_USBH_OFF_POLL(s)) |
                                Z_USBH_PB_MODE(Z_USBH_MODE_RAW));
    z_usbh_wr(Z_USBH_POLL_A(s), Z_USBH_PA_ADDR(dv->addr) |
                                Z_USBH_PA_ENDP(dv->h_ep) |
                                ((uint32_t)dv->xa_flags << 11) |
                                Z_USBH_PA_PORT(dv->port) |
                                Z_USBH_PA_MPS(dv->h_mps) |
                                Z_USBH_PA_INTERVAL(dv->h_ival) |
                                Z_USBH_PA_ENABLE);
}

// Ports with a change, from the status change endpoint's latest report.
// Bit 0 is the hub itself, bit n is port n. The hub keeps reporting a
// change until it is cleared, so a report missed here comes round again
// on the next poll -- which is why clearing `changed` by rewriting the
// register, not atomically, is safe.
static uint8_t take_changes(z_usbh_dev_t *dv)
{
    uint32_t pb;
    uint8_t bits;

    if (dv->slot < 0) {
        // Polling fallback: every port, about four times a second.
        if ((int32_t)(usbh_ticks() - dv->h_deadline) < 0) return 0;
        dv->h_deadline = usbh_ticks() + 180;
        return (uint8_t)(((1u << dv->h_nports) - 1u) << 1);
    }
    pb = z_usbh_rd(Z_USBH_POLL_B(dv->slot));
    if (!(pb & Z_USBH_PB_CHANGED)) return 0;
    bits = z_usbh_rb(Z_USBH_BUF + Z_USBH_OFF_POLL(dv->slot));
    z_usbh_wr(Z_USBH_POLL_B(dv->slot), pb & ~Z_USBH_PB_CHANGED);
    return bits;
}

// A child waiting to be reset, if address 0 and a scratch area are both
// free: claim them and start the reset. Returns 1 if one was started.
static int start_reset(int h)
{
    z_usbh_dev_t *dv = &usbh_devs[h];
    int j;

    for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++) {
        z_usbh_dev_t *c = &usbh_devs[j];
        if (c->parent != h || c->state != E_HUB_WAIT) continue;
        // Itself excepted: a child retrying after failing on address 0
        // is still marked as on it, and must be allowed to be reset.
        if (usbh_addr0_busy_except(j)) return 0;
        c->scr = usbh_scr_take_big();
        if (!c->scr) return 0;
        c->state = E_HUB_RESET;
        dv->h_port = c->hport;
        dv->h_tries = 0;
        usbh_ctrl_begin(h, RT_PORT_OUT, RQ_SET_FEATURE, F_PORT_RESET,
                        c->hport, 0);
        dv->h_state = HS_RESET_W;
        return 1;
    }
    return 0;
}

// The port's status and changes are known and cleared: bring the child
// record into line with them.
static void port_act(int h)
{
    z_usbh_dev_t *dv = &usbh_devs[h];
    int j = usbh_child_find(h, dv->h_port);
    int connected = (dv->h_pstat & PS_CONNECTION) != 0;

    // A connect change means whatever was there is gone, even if
    // something is connected now: a quick swap, or a bounce.
    if (j >= 0 && ((dv->h_pchg & PC_CONNECTION) || !connected)) {
        usbh_teardown(j);
        j = -1;
    }
    // A hub disables a port on a babbling or failed device. If it had
    // got as far as enumerating, drop it and start again.
    if (j >= 0 && connected && !(dv->h_pstat & PS_ENABLE) &&
        !usbh_devs[j].port_off &&
        usbh_devs[j].state != E_HUB_DEBOUNCE &&
        usbh_devs[j].state != E_HUB_WAIT &&
        usbh_devs[j].state != E_HUB_RESET) {
        usbh_teardown(j);
        j = -1;
    }
    if (connected && j < 0) usbh_child_new(h, dv->h_port);
}

void usbh_hub_step(int h)
{
    z_usbh_dev_t *dv = &usbh_devs[h];
    int r, b, j;

    switch (dv->h_state) {

    case HS_DESC:
        // The first eight bytes are all we use: bNbrPorts at 2,
        // bPwrOn2PwrGood at 5.
        usbh_ctrl_begin(h, RT_HUB_IN, RQ_GET_DESCRIPTOR, DT_HUB << 8, 0, 8);
        dv->h_state = HS_DESC_W;
        break;

    case HS_DESC_W:
        r = req(h);
        if (r < 0) { hub_err(h, "GET_DESCRIPTOR(HUB)"); dv->h_state = HS_DESC; break; }
        if (!r) break;
        dv->h_nports = z_usbh_rb(Z_USBH_BUF + dv->scr + 8u + 2);
        dv->h_pwr2good = z_usbh_rb(Z_USBH_BUF + dv->scr + 8u + 5);
        // One change byte covers seven ports; more are not served.
        if (dv->h_nports > 7) dv->h_nports = 7;
        printf("usb: hub %d: %d port(s)\n", dv->addr, dv->h_nports);
        dv->h_port = 1;
        dv->h_errs = 0;
        dv->h_state = dv->h_nports ? HS_POWER : HS_IDLE;
        break;

    case HS_POWER:
        // Harmless on a hub without power switching; required on one
        // with it, which powers up with every port off.
        usbh_ctrl_begin(h, RT_PORT_OUT, RQ_SET_FEATURE, F_PORT_POWER,
                        dv->h_port, 0);
        dv->h_state = HS_POWER_W;
        break;

    case HS_POWER_W:
        r = req(h);
        if (r < 0) { hub_err(h, "SET_FEATURE(PORT_POWER)"); dv->h_state = HS_POWER; break; }
        if (!r) break;
        if (++dv->h_port <= dv->h_nports) { dv->h_state = HS_POWER; break; }
        // bPwrOn2PwrGood is in 2 ms units; a tick is ~1.37 ms.
        dv->h_deadline = usbh_ticks() + (uint32_t)dv->h_pwr2good * 3u / 2u + 2u;
        dv->h_state = HS_SETTLE;
        break;

    case HS_SETTLE:
        if ((int32_t)(usbh_ticks() - dv->h_deadline) < 0) break;
        arm_status_slot(h);
        // Read every port once. Devices already plugged in at power-up
        // report a connect change anyway; this does not rely on it.
        dv->h_changes = (uint8_t)(((1u << dv->h_nports) - 1u) << 1);
        dv->h_deadline = usbh_ticks() + 180;
        dv->h_port = 0;
        dv->h_state = HS_IDLE;
        break;

    case HS_IDLE:
        // No reset is in progress in this state, so a child still in
        // E_HUB_RESET was abandoned by some path not covered above.
        // Recover it rather than let it hold address 0.
        for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++)
            if (usbh_devs[j].parent == h &&
                usbh_devs[j].state == E_HUB_RESET) {
                printf("usb: hub %d port %d: abandoned reset recovered\n",
                       dv->addr, usbh_devs[j].hport);
                usbh_scr_free(usbh_devs[j].scr);
                usbh_devs[j].scr = 0;
                usbh_devs[j].state = E_HUB_WAIT;
            }

        dv->h_changes |= take_changes(dv);

        if (dv->h_changes & 1) {
            // The hub's own status changed: local power or overcurrent.
            // Clearing both acknowledges either; nothing else is done.
            dv->h_changes &= (uint8_t)~1u;
            dv->h_port = 0;
            dv->h_tries = 0;
            usbh_ctrl_begin(h, RT_HUB_OUT, RQ_CLEAR_FEATURE,
                            F_C_HUB_OVER_CURRENT, 0, 0);
            dv->h_state = HS_HUBCLR_W;
            break;
        }

        if (dv->h_changes) {
            for (b = 1; b <= 7; b++)
                if (dv->h_changes & (1u << b)) break;
            dv->h_changes &= (uint8_t)~(1u << b);
            if (b > dv->h_nports) break;
            dv->h_port = (uint8_t)b;
            usbh_ctrl_begin(h, RT_PORT_IN, RQ_GET_STATUS, 0, b, 4);
            dv->h_state = HS_PSTAT_W;
            break;
        }

        // A device that has failed for good: disable its port, so it
        // stops answering -- above all on address 0, where it would
        // collide with every device enumerating after it. Unplugging it
        // clears the record as usual.
        for (j = Z_USBH_MAX_PORTS; j < Z_USBH_MAX_DEVS; j++) {
            z_usbh_dev_t *c = &usbh_devs[j];
            if (c->parent == h && c->state == E_FAILED &&
                c->retries >= 3 && !c->port_off) {
                printf("usb: hub %d port %d: device failed, port disabled\n",
                       dv->addr, c->hport);
                dv->h_port = c->hport;
                usbh_ctrl_begin(h, RT_PORT_OUT, RQ_CLEAR_FEATURE,
                                F_PORT_ENABLE, c->hport, 0);
                dv->h_state = HS_DISABLE_W;
                return;
            }
        }

        start_reset(h);
        break;

    case HS_DISABLE_W:
        r = req(h);
        if (r < 0) { hub_err(h, "CLEAR_FEATURE(PORT_ENABLE)"); break; }
        if (!r) break;
        j = usbh_child_find(h, dv->h_port);
        if (j >= 0) {
            usbh_devs[j].port_off = 1;
            usbh_devs[j].on_addr0 = 0;
        }
        dv->h_state = HS_IDLE;
        break;

    case HS_HUBCLR_W:
        r = req(h);
        if (r < 0) { hub_err(h, "CLEAR_HUB_FEATURE"); break; }
        if (!r) break;
        if (dv->h_tries++ == 0) {
            usbh_ctrl_begin(h, RT_HUB_OUT, RQ_CLEAR_FEATURE,
                            F_C_HUB_LOCAL_POWER, 0, 0);
            break;
        }
        dv->h_state = HS_IDLE;
        break;

    case HS_PSTAT_W:
        r = req(h);
        if (r < 0) { hub_err(h, "GET_PORT_STATUS"); break; }
        if (!r) break;
        read_pstat(dv);
        {
            // A device on this port just failed: report what the hub
            // says about the port (see usbh_dev_fail()). Status bits:
            // 0 connected, 1 enabled, 2 suspended, 3 over-current,
            // 4 in reset, 8 powered, 9 low speed.
            int c = usbh_child_find(h, dv->h_port);
            if (c >= 0 && usbh_devs[c].fail_probe) {
                z_usbh_dev_t *cd = &usbh_devs[c];
                cd->fail_probe = 0;
                if ((dv->h_pstat & PS_CONNECTION) &&
                    !(dv->h_pstat & PS_ENABLE) &&
                    (dv->h_pchg & PC_ENABLE)) {
                    // -- the hub disabled the port itself --
                    //
                    // A port error (USB 2.0 11.8.1). Linux treats this
                    // as recoverable -- hub.c, "disabled by hub (EMI?),
                    // re-enabling..." -- and so does this: the failed
                    // attempt is refunded, a port already given up on
                    // is taken back, and the normal retry path resets
                    // the port and enumerates again. Capped, so a
                    // device that really cannot work stops eventually.
                    // The CAUSE is not known (docs/usb_host.md, "Known
                    // issues", item 5); h_hubdis and h_recov in lsusb
                    // show whether it is still happening.
                    dv->h_hubdis++;
                    if (cd->recoveries < 16) {
                        cd->recoveries++;
                        dv->h_recov++;
                        if (cd->retries) cd->retries--;
                        cd->port_off = 0;
                        printf("usb: hub %d port %d disabled by hub (port "
                               "error), re-enabling (%d)\n", dv->addr,
                               dv->h_port, cd->recoveries);
                    } else {
                        printf("usb: hub %d port %d disabled by hub again; "
                               "%d recoveries, giving up\n", dv->addr,
                               dv->h_port, cd->recoveries);
                    }
                } else {
                    // The other kind: the port is still enabled and the
                    // device did not answer. Not recovered here; logged
                    // so it stays visible.
                    printf("usb: hub %d port %d failure with port "
                           "enabled: status %04x change %04x\n",
                           dv->addr, dv->h_port, dv->h_pstat, dv->h_pchg);
                }
            }
        }
        dv->h_clr = (uint8_t)(dv->h_pchg & PC_ALL);
        dv->h_errs = 0;
        dv->h_state = HS_PCLR;
        break;

    case HS_PCLR:
        // Acknowledge each change bit, lowest first, then act.
        if (dv->h_clr) {
            for (b = 0; b < 5; b++)
                if (dv->h_clr & (1u << b)) break;
            dv->h_clr &= (uint8_t)~(1u << b);
            usbh_ctrl_begin(h, RT_PORT_OUT, RQ_CLEAR_FEATURE,
                            (uint16_t)(F_C_PORT_CONNECTION + b),
                            dv->h_port, 0);
            dv->h_state = HS_PCLR_W;
            break;
        }
        port_act(h);
        dv->h_state = HS_IDLE;
        break;

    case HS_PCLR_W:
        r = req(h);
        if (r < 0) { hub_err(h, "CLEAR_PORT_FEATURE"); break; }
        if (!r) break;
        dv->h_state = HS_PCLR;
        break;

    // -- resetting a port for the device behind it --

    case HS_RESET_W:
        r = req(h);
        if (r < 0) {
            j = usbh_child_find(h, dv->h_port);
            if (j >= 0) usbh_dev_fail(&usbh_devs[j]);
            hub_err(h, "SET_FEATURE(PORT_RESET)");
            break;
        }
        if (!r) break;
        // The hub drives the 10-20 ms reset itself; look after ~11 ms.
        dv->h_deadline = usbh_ticks() + 8;
        dv->h_state = HS_RESET_POLL;
        break;

    case HS_RESET_POLL:
        if ((int32_t)(usbh_ticks() - dv->h_deadline) < 0) break;
        usbh_ctrl_begin(h, RT_PORT_IN, RQ_GET_STATUS, 0, dv->h_port, 4);
        dv->h_state = HS_RESET_ST_W;
        break;

    case HS_RESET_ST_W:
        r = req(h);
        if (r < 0) { hub_err(h, "GET_PORT_STATUS (reset)"); break; }
        if (!r) break;
        read_pstat(dv);
        j = usbh_child_find(h, dv->h_port);
        if (j < 0 || !(dv->h_pstat & PS_CONNECTION)) {
            // Unplugged mid-reset. The connect change will be picked up
            // from idle and tidy the record.
            reset_abort(h);
            dv->h_changes |= (uint8_t)(1u << dv->h_port);
            dv->h_state = HS_IDLE;
            break;
        }
        if ((dv->h_pchg & PC_RESET) && (dv->h_pstat & PS_ENABLE)) {
            usbh_ctrl_begin(h, RT_PORT_OUT, RQ_CLEAR_FEATURE,
                            F_C_PORT_CONNECTION + 4, dv->h_port, 0);
            dv->h_state = HS_RESET_CLR_W;
            break;
        }
        if (++dv->h_tries < 25) {
            dv->h_deadline = usbh_ticks() + 4;
            dv->h_state = HS_RESET_POLL;
            break;
        }
        printf("usb: hub %d port %d: reset never completed\n",
               dv->addr, dv->h_port);
        usbh_dev_fail(&usbh_devs[j]);
        dv->h_state = HS_IDLE;
        break;

    case HS_RESET_CLR_W:
        r = req(h);
        if (r < 0) { hub_err(h, "CLEAR_FEATURE(C_PORT_RESET)"); break; }
        if (!r) break;
        j = usbh_child_find(h, dv->h_port);
        if (j >= 0 && usbh_devs[j].state == E_HUB_RESET) {
            z_usbh_dev_t *c = &usbh_devs[j];
            // The hub reports the speed of what is behind the port. A
            // low-speed device there needs PRE and normal polarity --
            // never the INVERTED of a directly attached one.
            c->xa_flags = (dv->h_pstat & PS_LOW_SPEED) ? XA_LS_BEHIND_HUB
                                                        : 0x00;
            c->mps0 = 8;
            c->addr = 0;
            c->on_addr0 = 1;
            c->deadline = usbh_ticks() + 8;     // TRSTRCY, 10 ms
            c->state = E_HUB_RECOVER;
        }
        dv->h_errs = 0;
        dv->h_state = HS_IDLE;
        break;

    default:
        dv->h_state = HS_IDLE;
        break;
    }
}
