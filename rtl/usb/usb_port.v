/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB root port front end -- attach, speed, reset, disconnect.
 *
 * Written from the USB 2.0 specification, chapter 7.1.7 (signalling)
 * and 11.5 (port state). Clean-room; see docs/usb_host.md.
 *
 * One of these per physical port. It is the only part of the
 * controller that is per-port: the SIE, the transaction engine and the
 * packet buffer are all shared, because only one transaction can be on
 * the wire at a time anyway and a 64-byte full-speed transaction is
 * about 50 us inside a 1 ms frame. See docs/usb_host.md.
 *
 * -- what this owns, and what it deliberately does not --
 *
 * It owns the long timers: the 100 ms attach debounce, the 10 ms reset
 * drive, the 10 ms recovery. It owns speed detection, which is simply
 * which of the two lines the device chose to pull up. It owns
 * disconnect detection.
 *
 * It does NOT own the pins except while it is driving a reset. The
 * rest of the time the SIE drives them during its own packets and
 * releases them otherwise -- which is correct and is why there is no
 * "drive idle" state here. A USB host only drives the bus during
 * packets it is transmitting; between packets the DEVICE's pull-up
 * holds the line idle, because that 1.5k pull-up beats the board's
 * 15k pull-down. Driving idle ourselves would be wrong, and releasing
 * the bus is not what puts a device into reset -- an ABSENT device is
 * what leaves the pull-downs to win and shows SE0.
 *
 * -- timings are parameters --
 *
 * Every interval is a parameter in clock cycles so a testbench can run
 * the same RTL with the 100 ms debounce shortened to microseconds.
 * Simulating a real 100 ms attach at 48 MHz is 4.8 million cycles per
 * plug event, which turns a test suite into a coffee break for no
 * added coverage: the debounce either counts or it does not.
 */

module usb_port #(
    // Clocks per millisecond. 48_000 at sys_clk.
    parameter T_MS = 32'd48000,
    parameter DEBOUNCE_MS = 32'd100,
    // 30 ms of SE0 rather than the spec's 10 ms floor, and 20 ms of
    // recovery rather than 10. Real hosts commonly hold reset ~50 ms
    // total, and a device that is slow to accept it does not fail
    // loudly -- it just ignores address 0, which from this side is a
    // SETUP that times out with nothing on the wire. Boot-to-boot
    // variation in whether a device answered at all is exactly the
    // signature of a reset landing marginally, and time here is free.
    parameter RESET_MS = 32'd10,
    parameter RECOVERY_MS = 32'd10,
    // Disconnect is SE0 held longer than 2.5 us. The longest legal SE0
    // that is NOT a disconnect is a low-speed EOP, two low-speed bit
    // times, about 1.33 us -- so the threshold has to sit above that
    // and below anything a real unplug produces.
    parameter DISCONNECT_CLK = 32'd200,
    // A low-speed keepalive is an EOP: SE0 for two low-speed bit
    // times, 64 clocks at 48 MHz, then release.
    parameter KEEPALIVE_CLK = 32'd64
) (
    input wire clk,
    input wire rst,

    // raw pins, unpolarised: at attach time we do not yet know which
    // line the device pulls up, which is the whole question
    input wire dp_i,
    input wire dm_i,

    // Asserted only while driving a bus reset. usb_host.v gives this
    // priority over the SIE on the pin mux.
    output reg drive_se0,

    // control, from the CTRL register
    input wire ctl_enable,
    input wire ctl_reset,
    input wire ctl_suspend,
    // One pulse per frame on a low-speed port. A low-speed device
    // never sees a SOF token -- it would not understand one -- so the
    // frame marker on a low-speed segment is a bare EOP.
    input wire ctl_keepalive,

    // status, into the PORTSTAT register
    output reg st_connected,
    output reg st_enabled,
    output reg st_lowspeed,
    output reg st_resetting,
    output reg st_change,
    input wire change_ack
);

    localparam P_DISCON   = 3'd0;
    localparam P_DEBOUNCE = 3'd1;
    localparam P_READY    = 3'd2;
    localparam P_RESET    = 3'd3;
    localparam P_RECOVERY = 3'd4;
    localparam P_ENABLED  = 3'd5;
    localparam P_KA       = 3'd6;

    reg [2:0] ps;
    // 24 bits reaches 349 ms at 48 MHz, which covers every interval
    // above with room to spare. These were 32 bits and the two ports
    // between them spent about a hundred carry cells on counting to
    // numbers that do not need thirty-two bits.
    reg [23:0] tmr;
    reg [15:0] se0_tmr;
    // The keepalive needs its own timer and return state: it can now
    // interrupt RECOVERY as well as ENABLED, and recovery uses tmr.
    reg [7:0] ka_tmr;
    reg [2:0] ka_ret;

    reg dp_s0, dp_s1, dm_s0, dm_s1;

    wire se0 = (dp_s1 == 1'b0) && (dm_s1 == 1'b0);
    // Which line is high says which the device pulled up, and that is
    // the speed. D+ is full speed, D- is low speed. Nothing else about
    // this module cares about polarity; the SIE does that.
    wire saw_fs = (dp_s1 == 1'b1) && (dm_s1 == 1'b0);
    wire saw_ls = (dm_s1 == 1'b1) && (dp_s1 == 1'b0);

    always @(posedge clk) begin
        dp_s0 <= dp_i;
        dp_s1 <= dp_s0;
        dm_s0 <= dm_i;
        dm_s1 <= dm_s0;
    end

    always @(posedge clk) begin

        if (rst) begin

            ps <= P_DISCON;
            tmr <= 24'd0;
            se0_tmr <= 24'd0;
            ka_tmr <= 8'd0;
            ka_ret <= P_ENABLED;
            drive_se0 <= 1'b0;
            st_connected <= 1'b0;
            st_enabled <= 1'b0;
            st_lowspeed <= 1'b0;
            st_resetting <= 1'b0;
            st_change <= 1'b0;

        end else begin

            if (change_ack) st_change <= 1'b0;

            // Free-running SE0 duration counter, used for disconnect
            // detection below. Cleared by any non-SE0 line state, so
            // an EOP never accumulates toward the threshold.
            if (se0) se0_tmr <= se0_tmr + 16'd1;
            else se0_tmr <= 24'd0;

            case (ps)

            P_DISCON: begin
                drive_se0 <= 1'b0;
                st_connected <= 1'b0;
                st_enabled <= 1'b0;
                st_resetting <= 1'b0;
                if (!se0 && ctl_enable) begin
                    tmr <= 24'd0;
                    ps <= P_DEBOUNCE;
                end
            end

            P_DEBOUNCE: begin
                // The device must hold a stable non-SE0 state for the
                // whole interval. Any bounce back to SE0 restarts the
                // whole thing rather than merely pausing it -- a
                // connector being pushed in chatters, and half a
                // debounce is not a debounce.
                if (se0) begin
                    ps <= P_DISCON;
                end else if (tmr >= (T_MS * DEBOUNCE_MS)) begin
                    st_connected <= 1'b1;
                    st_lowspeed <= saw_ls;
                    st_change <= 1'b1;
                    ps <= P_READY;
                end else begin
                    tmr <= tmr + 24'd1;
                end
            end

            P_READY: begin
                // Attached, speed known, not yet reset. Software sees
                // the change bit and drives ctl_reset when it is ready
                // to enumerate -- with hubs there may be several
                // devices arriving at once and only one may hold
                // address 0 at a time, so the kernel serialises this
                // rather than the hardware racing ahead.
                if (se0_tmr >= DISCONNECT_CLK) begin
                    st_change <= 1'b1;
                    ps <= P_DISCON;
                end else if (ctl_reset) begin
                    tmr <= 24'd0;
                    drive_se0 <= 1'b1;
                    st_resetting <= 1'b1;
                    ps <= P_RESET;
                end
            end

            P_RESET: begin
                if (tmr >= (T_MS * RESET_MS)) begin
                    drive_se0 <= 1'b0;
                    tmr <= 24'd0;
                    ps <= P_RECOVERY;
                end else begin
                    tmr <= tmr + 24'd1;
                end
            end

            P_RECOVERY: begin
                // -- keepalives MUST continue through recovery --
                //
                // A low-speed device suspends after 3 ms without bus
                // activity. Recovery is 10 ms of idle J, and no
                // keepalive could fire during it because p_ka is gated
                // on st_enabled, which is not set until recovery ENDS.
                // So every reset was followed by the device falling
                // asleep, and the first SETUP after it went
                // unanswered -- exactly what the probe captured, and
                // why raising RECOVERY_MS made things worse.
                if (ctl_keepalive) begin
                    ka_tmr <= 8'd0;
                    ka_ret <= P_RECOVERY;
                    drive_se0 <= 1'b1;
                    ps <= P_KA;
                end else
                // The device is allowed to be unresponsive for this
                // long after reset. Talking to it early is a
                // legitimate-looking transaction that mysteriously
                // times out.
                if (tmr >= (T_MS * RECOVERY_MS)) begin
                    st_resetting <= 1'b0;
                    st_enabled <= 1'b1;
                    // Re-sample the speed: a reset is also how a hub
                    // reports the speed of whatever is behind it, and
                    // a device that was mis-read during a bouncy
                    // attach gets corrected here.
                    st_lowspeed <= saw_ls;
                    st_change <= 1'b1;
                    ps <= P_ENABLED;
                end else begin
                    tmr <= tmr + 24'd1;
                end
            end

            P_ENABLED: begin
                if (!ctl_enable) begin
                    ps <= P_DISCON;
                end else if (se0_tmr >= DISCONNECT_CLK) begin
                    st_enabled <= 1'b0;
                    st_connected <= 1'b0;
                    st_change <= 1'b1;
                    ps <= P_DISCON;
                end else if (ctl_reset) begin
                    // A second reset is legal and is how a driver
                    // recovers a device that has stopped answering.
                    tmr <= 24'd0;
                    drive_se0 <= 1'b1;
                    st_enabled <= 1'b0;
                    st_resetting <= 1'b1;
                    ps <= P_RESET;
                end else if (ctl_keepalive) begin
                    ka_tmr <= 8'd0;
                    ka_ret <= P_ENABLED;
                    drive_se0 <= 1'b1;
                    ps <= P_KA;
                end
            end

            P_KA: begin
                // Deliberately does NOT feed the disconnect detector:
                // se0_tmr is counting our own SE0 here. That is why
                // the threshold check lives in P_ENABLED and not in a
                // state that can be entered while we drive the bus.
                // Returns where it came from, so a keepalive taken
                // during recovery does not cut recovery short.
                if (ka_tmr >= KEEPALIVE_CLK) begin
                    drive_se0 <= 1'b0;
                    se0_tmr <= 16'd0;
                    ps <= ka_ret;
                end else begin
                    ka_tmr <= ka_tmr + 8'd1;
                end
            end

            default: ps <= P_DISCON;

            endcase

        end

    end

endmodule
