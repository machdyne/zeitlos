/*
 * Zeitlos SOC -- simulation only.
 *
 * Testbench for rtl/usb/usb_host.v. See docs/usb_host.md phase 1.
 *
 * The cases here are not a sampling of what the controller does; they
 * are the specific things that can silently be wrong:
 *
 *   1  full-speed attach, speed detection, reset sequencing
 *   2  a complete control transfer, with the data stage split across
 *      three packets by auto-continue and terminated by a short one
 *   3  the received descriptor actually matching, byte for byte --
 *      a CRC that passes on both sides can still be wrong on both
 *      sides, so the payload is compared against the model's own copy
 *   4  NAK retry preserving the data toggle
 *   5  STALL reported as STALL rather than as a timeout
 *   6  LOW SPEED, DIRECT: inverted polarity, 32 clocks per bit
 *   7  LOW SPEED BEHIND A HUB: normal polarity, 32 clocks per bit,
 *      every host packet preceded by a PRE sent at the full-speed rate
 *   8  SOF generation, and a transaction still completing around it
 *   9  auto-poll driving the cursor with no CPU involvement
 *  10  an exact-multiple auto-continue IN (ends on OK, not SHORT),
 *      with a poll slot live, and XACT_S still holding software's
 *      result frames later
 *
 * Throughout, a monitor checks that whenever XACT_S's pending bit is
 * clear, its status and length are software's own last result.
 *
 * Cases 6 and 7 differing is the entire point of running both. A host
 * that treats "low speed" as one thing passes 6 and fails 7, and the
 * failure looks like a dead hub rather than like a polarity bug.
 */

`timescale 1ns/1ps

module tb_usb_host;

    localparam A_CTRL     = 11'h040;
    localparam A_PORTSTAT = 11'h041;
    localparam A_IRQSTAT  = 11'h042;
    localparam A_IRQEN    = 11'h043;
    localparam A_XACT_A   = 11'h044;
    localparam A_XACT_B   = 11'h045;
    localparam A_XACT_S   = 11'h046;
    localparam A_CONFIG   = 11'h047;
    localparam A_POLL     = 11'h080;

    // Compat block registers, word addresses. Block 1 is at +8.
    localparam A_HID_INFO   = 11'h000;
    localparam A_HID_KEYS   = 11'h001;
    localparam A_HID_MOUSE  = 11'h002;
    localparam A_HID_CURSOR = 11'h003;

    localparam ST_OK    = 4'd0;
    localparam ST_NAK   = 4'd1;
    localparam ST_STALL = 4'd2;
    localparam ST_SHORT = 4'd6;

    // Buffer offsets, byte addresses inside the 2 KB window.
    localparam integer O_DATA  = 0;
    localparam integer O_SETUP = 512;

    reg clk;
    reg rst;

    reg [10:0] wb_adr;
    reg [31:0] wb_wdat;
    wire [31:0] wb_rdat;
    reg wb_we;
    reg [3:0] wb_sel;
    reg wb_stb;
    reg wb_cyc;
    wire wb_ack;
    wire usb_int;

    wire [1:0] dp;
    wire [1:0] dm;

    wire [9:0] curs_x0, curs_y0, curs_x1, curs_y1;
    wire [1:0] typ0, typ1;
    wire hid0_int, hid1_int;

    reg att_fs;
    reg att_ls;
    reg att_pre;

    integer errors;
    integer i;
    reg [31:0] r;
    reg [31:0] tmp;
    integer cx0, cy0, polls0;

    // The board: 22R series resistors are invisible here, but the 15k
    // pull-downs are not -- they are what makes an empty port read SE0
    // and what a device's 1.5k pull-up has to beat. Modelled `weak` so
    // the device's `pull` wins and any real driver wins over both.
    assign (highz1, weak0) dp[0] = 1'b0;
    assign (highz1, weak0) dm[0] = 1'b0;
    assign (highz1, weak0) dp[1] = 1'b0;
    assign (highz1, weak0) dm[1] = 1'b0;

    initial clk = 1'b0;
    always #10.4167 clk = ~clk;      // 48 MHz

    usb_host #(
        .PORTS(2),
        // Port timers shortened so a plug event costs microseconds
        // rather than five million cycles. The frame timer is NOT
        // shortened -- see the parameter's own comment.
        .T_MS(32'd20),
        .DEBOUNCE_MS(32'd100),
        .RESET_MS(32'd10),
        .RECOVERY_MS(32'd10),
        .T_FRAME(32'd48000)
    ) dut (
        .wb_clk_i(clk),
        .wb_rst_i(rst),
        .wb_adr_i(wb_adr),
        .wb_dat_i(wb_wdat),
        .wb_dat_o(wb_rdat),
        .wb_we_i(wb_we),
        .wb_sel_i(wb_sel),
        .wb_stb_i(wb_stb),
        .wb_ack_o(wb_ack),
        .wb_cyc_i(wb_cyc),
        .usb_dp(dp),
        .usb_dm(dm),
        .curs_x0(curs_x0), .curs_y0(curs_y0),
        .curs_x1(curs_x1), .curs_y1(curs_y1),
        .typ0(typ0), .typ1(typ1),
        .hid0_int_o(hid0_int), .hid1_int_o(hid1_int),
        .int_o(usb_int)
    );

    // Port 0: a full-speed device.
    tb_usb_device #(.MODE(0), .MPS0(8)) dev_fs (
        .dp(dp[0]), .dm(dm[0]), .attach(att_fs)
    );

    // Port 1 carries two models, one at a time. Low speed wired
    // directly, and low speed as seen through a hub.
    tb_usb_device #(.MODE(1), .MPS0(8)) dev_ls (
        .dp(dp[1]), .dm(dm[1]), .attach(att_ls)
    );

    tb_usb_device #(.MODE(2), .MPS0(8)) dev_pre (
        .dp(dp[1]), .dm(dm[1]), .attach(att_pre)
    );

    // ---------------------------------------------------------------
    // result integrity monitor
    // ---------------------------------------------------------------
    //
    // Software's contract with XACT_S is: once the pending bit reads
    // zero, the status and length fields are the result of the
    // transaction software itself started. This checks that contract
    // on every cycle, sampled at the falling edge because that is
    // what the register decode sees at the next rising one.
    //
    // A software transaction is a completion that is neither a SOF
    // nor an auto-poll slot; those two have their own destinations.
    reg sh_valid;
    reg [3:0] sh_status;
    reg [10:0] sh_len;
    integer stale_cycles;
    initial begin
        sh_valid = 1'b0;
        sh_status = 4'd0;
        sh_len = 11'd0;
        stale_cycles = 0;
    end
    always @(negedge clk) begin
        if (!rst) begin
            if (dut.x_done && !dut.sched_is_sof && !dut.sched_is_poll) begin
                sh_valid = 1'b1;
                sh_status = dut.x_status;
                sh_len = dut.x_act_len;
            end
            if (sh_valid && !dut.xact_pending &&
                (dut.res_status !== sh_status || dut.res_len !== sh_len))
                stale_cycles = stale_cycles + 1;
        end
    end

    // ---------------------------------------------------------------
    // Wishbone
    // ---------------------------------------------------------------

    task wb_write;
        input [10:0] a;
        input [31:0] d;
        begin
            @(posedge clk);
            wb_adr <= a;
            wb_wdat <= d;
            wb_we <= 1'b1;
            wb_sel <= 4'hf;
            wb_stb <= 1'b1;
            wb_cyc <= 1'b1;
            @(posedge clk);
            while (!wb_ack) @(posedge clk);
            wb_stb <= 1'b0;
            wb_cyc <= 1'b0;
            wb_we <= 1'b0;
        end
    endtask

    task wb_read;
        input [10:0] a;
        begin
            @(posedge clk);
            wb_adr <= a;
            wb_we <= 1'b0;
            wb_sel <= 4'hf;
            wb_stb <= 1'b1;
            wb_cyc <= 1'b1;
            @(posedge clk);
            while (!wb_ack) @(posedge clk);
            r = wb_rdat;
            wb_stb <= 1'b0;
            wb_cyc <= 1'b0;
        end
    endtask

    // Byte offset in the packet buffer to a Wishbone word address.
    function [10:0] bufw;
        input integer off;
        begin
            bufw = 11'h400 + (off >> 2);
        end
    endfunction

    task check;
        input [255:0] name;
        input integer got;
        input integer want;
        begin
            if (got !== want) begin
                $display("  FAIL %0s: got %0d want %0d", name, got, want);
                errors = errors + 1;
            end else begin
                $display("  ok   %0s = %0d", name, got);
            end
        end
    endtask

    // ---------------------------------------------------------------
    // helpers
    // ---------------------------------------------------------------

    // ctrl: port enables in bits 8/16, resets in 9/17, frame enable 1
    task port_enable;
        input integer p;
        input frame;
        begin
            wb_write(A_CTRL, (32'd1 << (8 + p * 8)) | (frame ? 32'd2 : 32'd0));
        end
    endtask

    task port_reset;
        input integer p;
        input frame;
        begin
            wb_write(A_CTRL, (32'd1 << (8 + p * 8)) |
                             (32'd1 << (9 + p * 8)) |
                             (frame ? 32'd2 : 32'd0));
            #5000;
            wb_write(A_CTRL, (32'd1 << (8 + p * 8)) |
                             (frame ? 32'd2 : 32'd0));
        end
    endtask

    task wait_enabled;
        input integer p;
        integer guard;
        begin
            guard = 0;
            r = 0;
            while (((r >> (p * 8 + 1)) & 1) !== 1 && guard < 20000) begin
                wb_read(A_PORTSTAT);
                guard = guard + 1;
            end
            if (guard >= 20000) begin
                $display("  FAIL port %0d never enabled", p);
                errors = errors + 1;
            end
        end
    endtask

    // One transaction, synchronously. pid: 0 SETUP, 1 IN, 2 OUT.
    task xact;
        input integer p;
        input [1:0] pid;
        input [6:0] addr;
        input [3:0] endp;
        input ls;
        input inv;
        input pre;
        input toggle;
        input autocont;
        input integer mps;
        input integer off;
        input integer len;
        input integer nak;
        integer guard;
        begin
            wb_write(A_XACT_A,
                {6'd0, mps[6:0], autocont, toggle, p[0], pre, inv, ls,
                 pid, endp, addr});
            wb_write(A_XACT_B,
                {1'b1, 5'd0, nak[3:0], len[10:0], off[10:0]});
            guard = 0;
            r = 32'h10000;
            while (r[16] && guard < 400000) begin
                wb_read(A_XACT_S);
                guard = guard + 1;
            end
            // Status is only valid once busy has fallen; read it once
            // more so the caller sees the settled value.
            wb_read(A_XACT_S);
        end
    endtask

    task put_setup;
        input [7:0] b0, b1, b2, b3, b4, b5, b6, b7;
        begin
            wb_write(bufw(O_SETUP), {b3, b2, b1, b0});
            wb_write(bufw(O_SETUP + 4), {b7, b6, b5, b4});
        end
    endtask

    // ---------------------------------------------------------------

    task enumerate;
        input integer p;
        input ls;
        input inv;
        input pre;
        begin
            // SETUP: GET_DESCRIPTOR(DEVICE), 18 bytes
            put_setup(8'h80, 8'h06, 8'h00, 8'h01,
                      8'h00, 8'h00, 8'h12, 8'h00);
            xact(p, 2'd0, 7'd0, 4'd0, ls, inv, pre, 1'b0, 1'b0,
                 8, O_SETUP, 8, 3);
            check("setup status", r[14:11], ST_OK);

            // Data stage. 18 bytes at 8 per packet is three
            // transactions, the last of them short -- so this is the
            // auto-continue path and the short-packet terminator in
            // one go.
            xact(p, 2'd1, 7'd0, 4'd0, ls, inv, pre, 1'b1, 1'b1,
                 8, O_DATA, 18, 3);
            check("in status", r[14:11], ST_SHORT);
            check("in length", r[10:0], 18);

            // Status stage: a zero-length OUT.
            xact(p, 2'd2, 7'd0, 4'd0, ls, inv, pre, 1'b1, 1'b0,
                 8, O_DATA, 0, 3);
            check("status stage", r[14:11], ST_OK);
        end
    endtask

    task check_descriptor;
        begin
            wb_read(bufw(O_DATA));
            check("desc[0..3]", r, 32'h02000112);
            wb_read(bufw(O_DATA + 4));
            check("desc[4..7]", r, 32'h08000000);
            wb_read(bufw(O_DATA + 8));
            check("desc[8..11]", r, 32'h123416d8);
            wb_read(bufw(O_DATA + 12));
            check("desc[12..15]", r, 32'h02010100);
            wb_read(bufw(O_DATA + 16));
            check("desc[16..17]", r[15:0], 16'h0103);
        end
    endtask

    // ---------------------------------------------------------------

    initial begin

        $dumpfile("output/tb_usb_host.vcd");
        $dumpvars(0, tb_usb_host);

        errors = 0;
        rst = 1'b1;
        wb_adr = 11'd0;
        wb_wdat = 32'd0;
        wb_we = 1'b0;
        wb_sel = 4'd0;
        wb_stb = 1'b0;
        wb_cyc = 1'b0;
        att_fs = 1'b0;
        att_ls = 1'b0;
        att_pre = 1'b0;

        repeat (20) @(posedge clk);
        rst = 1'b0;
        repeat (20) @(posedge clk);

        $display("");
        $display("== 0: identity ==");
        wb_read(A_CONFIG);
        check("config magic", r[31:20], 12'h05b);
        check("config ports", r[11:8], 2);

        // -----------------------------------------------------------
        $display("");
        $display("== 1: full-speed attach and reset ==");
        wb_write(A_CTRL, 32'd0);
        port_enable(0, 1'b0);
        att_fs = 1'b1;
        #60000;
        wb_read(A_PORTSTAT);
        check("p0 connected", r[0], 1);
        check("p0 low speed", r[2], 0);
        port_reset(0, 1'b0);
        wait_enabled(0);
        wb_read(A_PORTSTAT);
        check("p0 enabled", r[1], 1);

        // -----------------------------------------------------------
        $display("");
        $display("== 2/3: full-speed control transfer ==");
        enumerate(0, 1'b0, 1'b0, 1'b0);
        check_descriptor();

        // -----------------------------------------------------------
        $display("");
        $display("== 4: NAK retry ==");
        dev_fs.nak_budget = 2;
        put_setup(8'h80, 8'h06, 8'h00, 8'h01,
                  8'h00, 8'h00, 8'h12, 8'h00);
        xact(0, 2'd0, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0,
             8, O_SETUP, 8, 3);
        xact(0, 2'd1, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b1, 1'b1,
             8, O_DATA, 18, 3);
        check("nak-retry status", r[14:11], ST_SHORT);
        check("nak-retry length", r[10:0], 18);
        wb_read(A_XACT_S);
        check("naks absorbed", r[24:17], 2);
        check_descriptor();
        xact(0, 2'd2, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b1, 1'b0,
             8, O_DATA, 0, 3);

        // -----------------------------------------------------------
        $display("");
        $display("== 5: STALL ==");
        put_setup(8'h80, 8'h06, 8'h00, 8'h01,
                  8'h00, 8'h00, 8'h12, 8'h00);
        xact(0, 2'd0, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0,
             8, O_SETUP, 8, 3);
        dev_fs.stall_next = 1'b1;
        xact(0, 2'd1, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b1, 1'b0,
             8, O_DATA, 8, 3);
        check("stall status", r[14:11], ST_STALL);

        // -----------------------------------------------------------
        $display("");
        $display("== 6: low speed, DIRECT (inverted polarity) ==");
        wb_write(A_CTRL, 32'd0);
        port_enable(1, 1'b0);
        att_ls = 1'b1;
        #60000;
        wb_read(A_PORTSTAT);
        check("p1 connected", r[8], 1);
        check("p1 low speed", r[10], 1);
        port_reset(1, 1'b0);
        wait_enabled(1);
        enumerate(1, 1'b1, 1'b1, 1'b0);
        check_descriptor();
        att_ls = 1'b0;
        #20000;

        // -----------------------------------------------------------
        $display("");
        $display("== 7: low speed BEHIND A HUB (normal polarity, PRE) ==");
        wb_write(A_CTRL, 32'd0);
        #20000;
        port_enable(1, 1'b0);
        att_pre = 1'b1;
        #60000;
        wb_read(A_PORTSTAT);
        check("p1 connected", r[8], 1);
        // A low-speed device behind a full-speed hub pulls up D+ via
        // the hub, so the ROOT port sees full speed. The low-speed-ness
        // is something software learns from the hub's port status, not
        // something this port can detect -- which is exactly why
        // lowspeed and inverted are separate bits in XACT_A.
        check("p1 sees full speed", r[10], 0);
        port_reset(1, 1'b0);
        wait_enabled(1);
        enumerate(1, 1'b1, 1'b0, 1'b1);
        check_descriptor();

        // -----------------------------------------------------------
        $display("");
        $display("== 8: SOF ==");
        att_pre = 1'b0;
        #20000;
        wb_write(A_CTRL, 32'd0);
        #20000;
        port_enable(0, 1'b1);
        #60000;
        port_reset(0, 1'b1);
        wait_enabled(0);
        tmp = dev_fs.sof_count;
        #2200000;
        $display("  SOF frames seen by device: %0d (was %0d)",
                 dev_fs.sof_count, tmp);
        if (dev_fs.sof_count <= tmp) begin
            $display("  FAIL no SOF generated");
            errors = errors + 1;
        end
        // ...and a transaction still gets through between frames.
        enumerate(0, 1'b0, 1'b0, 1'b0);
        check_descriptor();

        // -----------------------------------------------------------
        // Phase 2. The cursor datapath, end to end, with the CPU doing
        // nothing but the one-time setup.
        $display("");
        $display("== 9: auto-poll drives the cursor, no CPU ==");

        // The kernel's part: say what this device is, then describe
        // the endpoint once. After this the CPU is finished.
        wb_write(A_HID_INFO, 32'd2 << 24);        // typ = 2, a mouse
        wb_write(A_POLL, {
            1'b0,            // toggle, hardware maintains it
            1'b1,            // enable
            8'd1,            // interval, frames
            7'd8,            // mps
            1'b0,            // port 0
            1'b0,            // use_pre
            1'b0,            // inverted
            1'b0,            // lowspeed
            4'd1,            // endpoint 1
            7'd0 });         // address 0
        wb_write(A_POLL + 11'd1, {
            12'd0, 2'd0,     // ctgt: compat block 0
            3'd2,            // mode: BOOT_MOUSE
            11'd768 });      // buffer offset
        // poll_en, frame_en, port 0 enabled
        wb_write(A_CTRL, 32'h00000106);

        wb_read(A_HID_INFO);
        check("typ reads back", r[25:24], 2);

        cx0 = curs_x0;
        cy0 = curs_y0;
        polls0 = dev_fs.ep1_reports;

        // An idle mouse NAKs every poll. Nothing should move, and
        // nothing should be reported -- a NAK is the device saying
        // there is no news, not a report of zero movement.
        #3000000;
        check("idle: cursor still", (curs_x0 == cx0[9:0]) ? 1 : 0, 1);
        check("idle: no reports", dev_fs.ep1_reports, polls0);

        // Now a report: buttons=1, dx=+10, dy=+6. Above the
        // acceleration threshold of 3, so both are doubled.
        dev_fs.hid_r0 = 8'h01;
        dev_fs.hid_r1 = 8'd10;
        dev_fs.hid_r2 = 8'd6;
        dev_fs.hid_r3 = 8'h00;
        dev_fs.hid_have = 1'b1;
        #3000000;

        check("report delivered", dev_fs.ep1_reports, polls0 + 1);
        check("cursor x moved", curs_x0 - cx0, 20);
        check("cursor y moved", curs_y0 - cy0, 12);
        wb_read(A_HID_CURSOR);
        check("cursor reg x", r[9:0], curs_x0);
        check("cursor reg y", r[19:10], curs_y0);
        check("cursor reg buttons", r[27:20], 1);
        wb_read(A_HID_MOUSE);
        check("mouse reg dx", r[7:0], 10);
        check("mouse reg dy", r[15:8], 6);

        // A small delta stays 1:1 -- that is the point of the curve.
        cx0 = curs_x0;
        dev_fs.hid_r1 = 8'd2;
        dev_fs.hid_r2 = 8'd0;
        dev_fs.hid_have = 1'b1;
        #3000000;
        check("small delta is 1:1", curs_x0 - cx0, 2);

        // Saturation: a long sweep left must stop at 0, not wrap.
        for (i = 0; i < 40; i = i + 1) begin
            dev_fs.hid_r1 = 8'hd8;      // -40
            dev_fs.hid_r2 = 8'h00;
            dev_fs.hid_have = 1'b1;
            #400000;
        end
        check("cursor saturates at 0", curs_x0, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== 10: exact-multiple auto-continue, auto-poll live ==");

        // The shape of a mass-storage sector read: a request that is
        // an exact multiple of the packet size, so the sequence ends
        // on remaining == 0 with ST_OK rather than on a short packet.
        // 32 bytes of the configuration descriptor at 8 per packet is
        // four full packets. Nothing else in this bench runs more
        // than three, and none of them end this way.
        //
        // The mouse slot from case 9 is still enabled and NAKing
        // every frame, as on hardware with a keyboard and mouse
        // attached while the stick is read.
        check("monitor: stale before 10", stale_cycles, 0);

        put_setup(8'h80, 8'h06, 8'h00, 8'h02,
                  8'h00, 8'h00, 8'h20, 8'h00);
        xact(0, 2'd0, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0,
             8, O_SETUP, 8, 3);
        check("cfg setup status", r[14:11], ST_OK);
        xact(0, 2'd1, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b1, 1'b1,
             8, O_DATA, 32, 3);
        check("cfg in status", r[14:11], ST_OK);
        check("cfg in length", r[10:0], 32);
        wb_read(bufw(O_DATA));
        check("cfg[0..3]", r, {dev_fs.cfgd[3], dev_fs.cfgd[2],
                               dev_fs.cfgd[1], dev_fs.cfgd[0]});
        wb_read(bufw(O_DATA + 28));
        check("cfg[28..31]", r, {dev_fs.cfgd[31], dev_fs.cfgd[30],
                                 dev_fs.cfgd[29], dev_fs.cfgd[28]});
        xact(0, 2'd2, 7'd0, 4'd0, 1'b0, 1'b0, 1'b0, 1'b1, 1'b0,
             8, O_DATA, 0, 3);
        check("cfg status stage", r[14:11], ST_OK);

        // Several frames later, with polls completing in between,
        // XACT_S must still say what software's last request did.
        #3000000;
        wb_read(A_XACT_S);
        check("result survives polls: status", r[14:11], ST_OK);
        check("result survives polls: pending", r[16], 0);
        check("monitor: stale cycles", stale_cycles, 0);

        // -----------------------------------------------------------
        $display("");
        if (errors == 0) $display("PASS -- all checks ok");
        else $display("FAIL -- %0d check(s) failed", errors);
        $display("");
        $finish;

    end

    initial begin
        #40000000;
        $display("FAIL -- global timeout");
        $finish;
    end

endmodule
