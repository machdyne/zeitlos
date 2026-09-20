/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host stack -- INTERNAL interface between usbh.c and usbh_hub.c.
 * Not for anything outside sw/os/usb; the public API is usbh.h.
 *
 * The hub class driver needs the device table, the control transfer
 * engine and the enumeration states, which used to be private to
 * usbh.c. They live here instead of being duplicated.
 */

#ifndef Z_USBH_INT_H
#define Z_USBH_INT_H

#include <stdint.h>
#include "usbh.h"

#ifndef DIR_IN
#define DIR_IN              0x80
#endif

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
// Behind a hub. The hub driver (usbh_hub.c) moves a child through
// these; from E_DESC8 on it enumerates like any other device.
#define E_HUB_DEBOUNCE  16  // connected, waiting out the 100 ms debounce
#define E_HUB_WAIT      17  // debounced, waiting for address 0 and scratch
#define E_HUB_RESET     18  // hub is resetting the port: holds address 0
#define E_HUB_RECOVER   19  // reset done, 10 ms recovery: holds address 0
// CDC-ACM class requests between SET_CONFIGURATION and binding.
#define E_CDC_LINE      20  // SET_LINE_CODING
#define E_CDC_DTR       21  // SET_CONTROL_LINE_STATE

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
    // The whole configuration descriptor, up to the 200 bytes fetched.
    // This was 64, and every class probe reads this copy: a composite
    // CDC device (98 bytes, class 239) had its data interface's bulk
    // endpoints at byte 64 onward, and the CDC probe -- correctly, on
    // what it was shown -- found an ACM interface with no endpoints.
    uint8_t cfg_raw[200];
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
    // uint16_t, not uint8_t: CTRL_SOFT_NAKS is 1000. As a byte this
    // wrapped at 256, every comparison against the limit was constant,
    // and a device NAKing a control stage was retried forever instead
    // of failed after ~160 ms. gcc -Wtype-limits reports it.
    uint16_t ctrl_soft_naks;
    uint8_t ctrl_tgl;
    uint8_t ctrl_pend;
    uint8_t ctrl_dir_in;
    uint16_t ctrl_xferred;
    uint16_t ctrl_len;
    uint32_t ctrl_delay_until;
    uint32_t deadline;      // kernel ticks, 0 for none

    // -- where it is, and what it holds --
    //
    // A root-port device lives at devs[port] and has parent -1; a
    // device behind a hub lives at devs[Z_USBH_MAX_PORTS..] and names
    // its hub and the hub port. `port` is always the ROOT port, because
    // that is what a transaction selects; the hub repeats the rest.
    int8_t parent;          // hub's index in devs[], -1 for a root port
    uint8_t hport;          // hub port, 1-based; 0 on a root port
    uint8_t depth;          // hubs between this device and the root
    // Packet-buffer scratch for this device's control transfers: SETUP
    // at scr, data from scr + 8. 0 when it holds none -- enumeration
    // takes one of two large areas and gives it back at bind; a hub
    // then keeps a small one for its port requests. See usbh.c.
    uint16_t scr;
    int8_t slot;            // auto-poll slot, -1 for none
    // Behind a hub: may be answering on address 0 -- from its port
    // reset until SET_ADDRESS succeeds, INCLUDING after a failure in
    // between. Every device behind a hub shares one segment, so while
    // this is set nothing else may use address 0.
    uint8_t on_addr0;
    // Behind a hub: its port has been disabled after a final failure,
    // so it hears nothing and answers nothing.
    uint8_t port_off;
    // Behind a hub: a transfer to it failed; the hub driver reads the
    // port's status and logs it once (enabled? suspended?).
    uint8_t fail_probe;
    // Times the hub disabled this device's port itself and it was reset
    // and re-enumerated without that counting as a failed attempt.
    uint8_t recoveries;

    // -- hub class driver, usbh_hub.c; meaningful only for a hub --
    uint8_t h_state;
    uint8_t h_nports;
    uint8_t h_pwr2good;     // bPwrOn2PwrGood, units of 2 ms
    uint8_t h_ep, h_mps, h_ival;    // status change endpoint
    uint8_t h_port;         // port being worked on
    uint8_t h_changes;      // ports with a change still to service, bit n = port n
    uint8_t h_clr;          // change bits still to clear on h_port
    uint8_t h_tries;
    uint8_t h_errs;         // consecutive failed hub requests
    uint16_t h_fails;       // failed hub requests, all time -- lsusb
    // Ports this hub disabled on its own (C_PORT_ENABLE: a port error,
    // USB 2.0 11.8.1), and how many of those were recovered. lsusb.
    uint16_t h_hubdis, h_recov;
    uint16_t h_pstat, h_pchg;
    uint32_t h_deadline;
} z_usbh_dev_t;

// The device table. devs[0..Z_USBH_MAX_PORTS-1] are the root ports;
// the rest are handed out to devices behind hubs.
extern z_usbh_dev_t usbh_devs[Z_USBH_MAX_DEVS];

// -- usbh.c --
uint32_t usbh_ticks(void);
void usbh_ctrl_begin(int d, uint8_t bmType, uint8_t bReq,
                     uint16_t wVal, uint16_t wIdx, uint16_t wLen);
int usbh_ctrl_step(int d);
int usbh_ctrl_finished(int d);
int usbh_addr0_busy(void);
int usbh_addr0_busy_except(int me);
void usbh_dev_fail(z_usbh_dev_t *dv);
void usbh_teardown(int i);
uint16_t usbh_scr_take_big(void);
uint16_t usbh_scr_take_hub(void);
void usbh_scr_free(uint16_t off);
int usbh_slot_take(void);
void usbh_slot_free(int s);
int usbh_child_find(int h, int hport);
int usbh_child_new(int h, int hport);

// -- usbh_hub.c --
int usbh_hub_probe(const z_usbh_dev_t *dv);
int usbh_hub_bind(int h);
void usbh_hub_step(int h);

#endif
