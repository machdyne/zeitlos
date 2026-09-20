/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB HID compatibility block -- the reg_usbN_* registers.
 *
 * Two instances, one per compat block. Presents byte-for-byte the same
 * five registers rtl/usb_hid.v does today, so sw/os/hid.c,
 * sw/apps/wm/wm.c, sw/apps/gpu3d/gpu3d.c and sw/bios/bios.c need no
 * change at all. docs/user_input.md stays correct.
 *
 * -- what is lifted and what is dropped --
 *
 * THE POINTER ARITHMETIC IS LIFTED VERBATIM FROM rtl/usb_hid.v. That
 * file is Lone Dynamics copyright, written for this project, so this
 * is reuse rather than a clean-room concern -- and it is deliberate:
 * the acceleration curve, the SENS_SHIFT divide with its carried
 * remainder, and the saturation at 639/479 are exactly the parts that
 * have to stay bit-compatible. Re-deriving them would be the one sure
 * way to introduce a pointer that "feels different" and cannot be
 * diffed against anything.
 *
 * What is dropped is all of the clock-domain crossing. usb_hid.v is
 * largely synchronisers -- report_s0/s1/s2, typ_s0/s1/s2, the
 * dx_cap/dy_cap capture-a-cycle-early trick -- because usb_hid_host
 * runs at 12 MHz and the Wishbone side at 48. This core is 48 MHz
 * throughout, so the reports arrive in this clock domain already and
 * the synchronisers are not just unnecessary, they would be wrong:
 * report_edge exists to turn a foreign-domain level into a local
 * pulse, and in_valid is already a local pulse.
 *
 * -- where the reports come from --
 *
 * Not from a microcode sequencer. The auto-poll engine in usb_host.v
 * repeats an interrupt-IN transfer on its own and hands the payload
 * here, with no CPU in the path at all. Software sets a poll slot up
 * once, at enumeration, and then stops being involved -- which is what
 * keeps the cursor free of the interrupt jitter that a software-driven
 * pointer would inherit from k_hid_read_key() and k_fs_enter(). See
 * docs/usb_host.md.
 *
 * -- typ is now assigned, not inferred --
 *
 * usb_hid_host guessed a device type from the first descriptor it saw.
 * Here the kernel's enumeration decides and writes it. Between
 * physical attach and the end of enumeration typ reads 0, which is a
 * couple of hundred milliseconds longer than before -- but zero
 * already means "nothing on this port", which every consumer handles,
 * so it is a longer version of a case they already cope with rather
 * than a new one.
 */

// Pointer acceleration knobs, copied from rtl/usb_hid.v so that file's
// tuning survives its removal. A `define is global from the point it
// appears, so these sit at file scope as they do there -- putting one
// in a module body only hides that fact.
`define USB_HID_ACCEL
`define USB_HID_ACCEL_THRESHOLD 3

module usb_hid_compat #(
    // Divides the pointer delta by 2^SENS_SHIFT, remainder carried so
    // slow movement is not rounded away. Set per board from
    // rtl/boards.vh's USB_HID_SENS_SHIFT, exactly as before.
    parameter SENS_SHIFT = 0
) (
    input wire clk,
    input wire rst,

    // -- one report, from the auto-poll engine --
    // mode 2 is a boot mouse, 3 is a boot keyboard. Both layouts are
    // fixed by the specification, which is the whole reason this can
    // be a byte mux rather than a report-descriptor parser.
    input wire in_valid,
    input wire [1:0] in_mode,
    input wire [7:0] in_b0,
    input wire [7:0] in_b1,
    input wire [7:0] in_b2,
    input wire [7:0] in_b3,
    input wire [7:0] in_b4,
    input wire [7:0] in_b5,
    input wire [7:0] in_b6,
    input wire [7:0] in_b7,
    // Payload bytes the report actually had (0-8). A 3-byte boot mouse
    // report has no wheel byte, and in_b3 then holds a stale one.
    input wire [3:0] in_len,
    // Byte 3 is a wheel the driver confirmed from the report descriptor
    // (report protocol). Only then is it counted: the boot mouse report
    // is three bytes by the HID spec, and a fourth one in boot protocol
    // is undefined -- often a wheel, sometimes vendor data, sometimes
    // nothing (a Microsoft 045e:0737 sends no wheel there at all).
    input wire in_wheel,

    // -- device type, written by the kernel --
    input wire typ_we,
    input wire [1:0] typ_i,

    // -- register images --
    output wire [31:0] reg_info,
    output wire [31:0] reg_keys,
    output wire [31:0] reg_mouse,
    output wire [31:0] reg_cursor,
    output wire [31:0] reg_pad,

    // -- into gpu_cursor.v --
    output reg [9:0] curs_x,
    output reg [9:0] curs_y,
    output wire [1:0] typ,

    output wire int_o
);

    localparam MODE_BOOT_MOUSE = 2'd2;
    localparam MODE_BOOT_KBD   = 2'd3;

    reg [1:0] typ_r;
    reg [1:0] typ_prev;
    reg report;

    reg [7:0] key_modifiers;
    reg [7:0] key1, key2, key3, key4;
    reg [7:0] mouse_btn;
    reg signed [7:0] mouse_dx;
    reg signed [7:0] mouse_dy;
    // -- scroll wheel --
    //
    // A free-running signed accumulator of the wheel, which software
    // reads and subtracts, rather than a per-report delta: a wheel is
    // discrete notches, and an accumulator never loses one to a report
    // software did not read in time. Taken from byte 3 of the report,
    // and only when the driver has confirmed from the mouse's report
    // descriptor that byte 3 IS the wheel -- report protocol, simple
    // layout (in_wheel). Not in boot protocol: the HID spec defines the
    // boot mouse report as three bytes. Positive is away from the user
    // (scroll up), the HID convention.
    reg signed [7:0] wheel_acc;
    reg [9:0] game_state;

    reg mouse_move;
    reg signed [11:0] sub_x;
    reg signed [11:0] sub_y;

    wire typ_changed = (typ_r != typ_prev);

    assign typ = typ_r;

    // A report OR a device type change. The type change has to be an
    // interrupt in its own right: on unplug there are no more reports,
    // so without it sw/os/hid.c would never learn to flush the keys a
    // yanked keyboard was holding down. Same reasoning as usb_hid.v.
    assign int_o = report || typ_changed;

    // -- register images, bit-for-bit as rtl/usb_hid.v produces them --
    //
    // Including the quirks, deliberately. typ sits at [25:24] of info
    // because of where it falls in usb_hid.v's concatenation, and the
    // cursor word is truncated from the TOP -- usb_hid.v concatenates
    // 39 bits into a 32-bit register, so the leading 11'd0 loses seven
    // of its bits and what survives is four zeros, the buttons, y and
    // x. Tidying either would be a gratuitous change to something that
    // works and that docs/user_input.md already documents as a trap.
    assign reg_info = { report, 5'b00000, typ_r, 16'b0, key_modifiers };
    assign reg_keys = { key1, key2, key3, key4 };
    assign reg_mouse = { wheel_acc, mouse_btn, mouse_dy, mouse_dx };
    assign reg_cursor = { 4'd0, mouse_btn, curs_y, curs_x };
    assign reg_pad = { 20'd0, typ_r, game_state };

    // ---------------------------------------------------------------
    // pointer acceleration -- lifted from rtl/usb_hid.v
    // ---------------------------------------------------------------
    //
    // Small movements stay 1:1, which is what fine positioning needs;
    // movements above a threshold are doubled, so a sweep across the
    // screen gets there twice as fast. One threshold and one shift,
    // because this sits in the middle of a cursor path and is not a
    // place for a multiplier.

    wire signed [11:0] dx_s = { {4{mouse_dx[7]}}, mouse_dx };
    wire signed [11:0] dy_s = { {4{mouse_dy[7]}}, mouse_dy };

`ifdef USB_HID_ACCEL
    wire dx_fast = (mouse_dx > $signed(`USB_HID_ACCEL_THRESHOLD)) ||
        (mouse_dx < -$signed(`USB_HID_ACCEL_THRESHOLD));
    wire dy_fast = (mouse_dy > $signed(`USB_HID_ACCEL_THRESHOLD)) ||
        (mouse_dy < -$signed(`USB_HID_ACCEL_THRESHOLD));

    wire signed [11:0] dx_move = dx_fast ? (dx_s <<< 1) : dx_s;
    wire signed [11:0] dy_move = dy_fast ? (dy_s <<< 1) : dy_s;
`else
    wire signed [11:0] dx_move = dx_s;
    wire signed [11:0] dy_move = dy_s;
`endif

    // Extra headroom bits so overshoot past [0,639]/[0,479] can be
    // saturated instead of wrapping in curs_x/curs_y's own 10-bit
    // unsigned width. Real HID deltas are commonly more than +-1, so a
    // single update can jump straight past a boundary -- testing the
    // current value against the boundary before applying the delta
    // only catches the case where it already sits exactly on it.
    wire signed [11:0] acc_x = sub_x + dx_move;
    wire signed [11:0] acc_y = sub_y + dy_move;
    wire signed [11:0] step_x = acc_x >>> SENS_SHIFT;
    wire signed [11:0] step_y = acc_y >>> SENS_SHIFT;
    wire signed [11:0] rem_x = acc_x - (step_x <<< SENS_SHIFT);
    wire signed [11:0] rem_y = acc_y - (step_y <<< SENS_SHIFT);

    wire signed [11:0] curs_x_sum = $signed({2'b00, curs_x}) + step_x;
    wire signed [11:0] curs_y_sum = $signed({2'b00, curs_y}) + step_y;

    // ---------------------------------------------------------------

    always @(posedge clk) begin

        report <= 1'b0;
        mouse_move <= 1'b0;
        typ_prev <= typ_r;

        if (rst) begin

            typ_r <= 2'd0;
            typ_prev <= 2'd0;
            key_modifiers <= 8'd0;
            key1 <= 8'd0;
            key2 <= 8'd0;
            key3 <= 8'd0;
            key4 <= 8'd0;
            mouse_btn <= 8'd0;
            mouse_dx <= 8'sd0;
            mouse_dy <= 8'sd0;
            wheel_acc <= 8'sd0;
            game_state <= 10'd0;
            curs_x <= 10'd320;
            curs_y <= 10'd240;
            sub_x <= 12'sd0;
            sub_y <= 12'sd0;

        end else begin

            if (typ_we) typ_r <= typ_i;

            // Pad state is cleared whenever this block is not
            // currently a gamepad, and the clear is tested FIRST so it
            // wins over a capture. On the report that accompanies a
            // device change, "no pad here" is the newer and more
            // useful truth than whatever buttons the outgoing device
            // last reported. This is what makes hot unplug safe, and
            // it is kept verbatim from usb_hid.v for that reason.
            if (typ_r != 2'd3) game_state <= 10'd0;

            if (in_valid) begin

                report <= 1'b1;

                if (in_mode == MODE_BOOT_MOUSE) begin
                    // Boot mouse report: buttons, dx, dy, wheel. The
                    // wheel is byte 3 by convention (see wheel_acc);
                    // a mouse without one sends three bytes.
                    mouse_btn <= in_b0;
                    mouse_dx <= in_b1;
                    mouse_dy <= in_b2;
                    if (in_wheel && in_len >= 4'd4)
                        wheel_acc <= wheel_acc + in_b3;
                    mouse_move <= 1'b1;
                end

                if (in_mode == MODE_BOOT_KBD) begin
                    // Boot keyboard report: modifiers, a reserved
                    // byte, then six keycodes. The compat registers
                    // carry four, as they always have.
                    key_modifiers <= in_b0;
                    key1 <= in_b2;
                    key2 <= in_b3;
                    key3 <= in_b4;
                    key4 <= in_b5;
                end

            end

            if (mouse_move) begin

                sub_x <= rem_x;
                sub_y <= rem_y;

                if (curs_x_sum < 0) curs_x <= 10'd0;
                else if (curs_x_sum > 639) curs_x <= 10'd639;
                else curs_x <= curs_x_sum[9:0];

                if (curs_y_sum < 0) curs_y <= 10'd0;
                else if (curs_y_sum > 479) curs_y <= 10'd479;
                else curs_y <= curs_y_sum[9:0];

            end

        end

    end

endmodule
