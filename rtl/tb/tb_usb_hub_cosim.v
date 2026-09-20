/*
 * Zeitlos SOC -- simulation only.
 *
 * Hub co-simulation: the REAL sw/os/usb driver, including the hub
 * class driver (usbh_hub.c) and mass storage (usbh_msc.c), against the
 * REAL rtl/usb gateware, a four-port hub model, and devices behind it.
 * See docs/usb_host.md, "Hub class driver".
 *
 * Behind the hub on root port 0: a full-speed bulk-only stick on hub
 * port 1 and a low-speed boot mouse on hub port 3. The mouse is the
 * case PRE exists for -- low speed, normal polarity, every packet
 * preceded by a full-speed PRE -- and its auto-poll slot has to carry
 * USE_PRE. Every sector read is compared byte for byte.
 *
 *   enumerate      the hub, then both devices behind it
 *   mouse          a report through PRE moves the hardware cursor
 *   storage        start, reads, write and read back, through the hub
 *   unplug/replug  each device behind the hub, the hub reporting it
 *   concurrent     the mouse re-enumerating behind the hub during
 *                  reads, interrupts emulated
 *   hub unplug     everything behind it goes, and comes back
 *   protocol       the stick saw every CBW answered by one CSW
 */

`timescale 1ns/1ps

// Line-transition skew for the device models, in nanoseconds.
// Override with -DDEV_SKEW=20 to reproduce real silicon.
`ifndef DEV_SKEW
`define DEV_SKEW 0
`endif
// Separate, because the spec limits differ by a factor of six:
// +-2500 ppm at full speed, +-15000 ppm at low speed.
`ifndef DEV_PPM_FS
`define DEV_PPM_FS 0
`endif
`ifndef DEV_PPM_LS
`define DEV_PPM_LS 0
`endif

module tb_usb_hub_cosim;

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

    reg att_hub;                    // hub plugged into root port 0
    reg plug_stick, plug_mouse;     // devices plugged into the hub
    reg plug_bad;                   // a device that fails enumeration
    wire [3:0] hub_en, hub_rst;

    // VPI handshake
    integer q_valid, q_we, q_adr, q_wdat, q_bw;
    integer rdat;
    integer guard;
    integer errors;
    integer i;
    integer cx;
    integer rc, bad, first_bad, fails, n;
    integer tmp, tmp2;
    time t0;
    integer wa, lane, k;            // bus_cycle / run_for scratch

    assign (highz1, weak0) dp[0] = 1'b0;
    assign (highz1, weak0) dm[0] = 1'b0;
    assign (highz1, weak0) dp[1] = 1'b0;
    assign (highz1, weak0) dm[1] = 1'b0;

    initial clk = 1'b0;
    always #10.4167 clk = ~clk;      // 48 MHz

    usb_host #(
        .PORTS(2),
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

    // Root port 0: a four-port full-speed hub.
    tb_usb_device #(.MODE(0), .MPS0(64), .HUB(1), .PORT_LS(4'b0100),
                   .SKEW_NS(`DEV_SKEW), .CLK_PPM(`DEV_PPM_FS)) hub (
        .dp(dp[0]), .dm(dm[0]), .attach(att_hub),
        .hub_en(hub_en), .hub_rst(hub_rst),
        .hub_conn({1'b0, plug_mouse, plug_bad, plug_stick})
    );

    // Behind it, on the same wires: a full-speed stick on hub port 1
    // and a low-speed mouse on hub port 3. A full-speed hub is a
    // repeater, so electrically that is what they see; each is live
    // only while the hub has its port enabled AND the hub itself is
    // plugged in, and is reset by the hub's port reset, which never
    // appears upstream.
    tb_usb_device #(.MODE(0), .MPS0(64), .MSC(1), .SKEW_NS(`DEV_SKEW),
                   .CLK_PPM(`DEV_PPM_FS)) dev (
        .dp(dp[0]), .dm(dm[0]), .attach(att_hub & hub_en[0]),
        .ext_reset(hub_rst[0])
    );
    // MODE 2: low speed with NORMAL polarity, reached through PRE --
    // the case docs/usb_host.md warns fails silently if it is ever
    // treated like a directly attached low-speed device.
    tb_usb_device #(.MODE(2), .MPS0(8), .SKEW_NS(`DEV_SKEW),
                   .CLK_PPM(`DEV_PPM_LS)) mouse (
        .dp(dp[0]), .dm(dm[0]), .attach(att_hub & hub_en[2]),
        .ext_reset(hub_rst[2])
    );


    // -- bus contention: the host and a device driving at once --
    //
    // Shows as x on the wire. Every bench passed with it present:
    // the models tolerate it, a real hub does not. After a low-speed
    // transaction a full-speed SOF started while the device was still
    // driving its EOP; see gap_limit in rtl/usb/usb_xact.v.
    integer xcount;
    initial xcount = 0;
    always @(dp[0] or dm[0])
        if (dp[0] === 1'bx || dm[0] === 1'bx) begin
            xcount = xcount + 1;
            if (xcount <= 4)
                $display("[wire %0t] CONTENTION on port 0", $time);
        end

    // Hub port 2: a low-speed device that fails enumeration and is left
    // on address 0 -- stall_set_addr is set before it is plugged in.
    tb_usb_device #(.MODE(2), .MPS0(8), .SKEW_NS(`DEV_SKEW),
                   .CLK_PPM(`DEV_PPM_LS)) failer (
        .dp(dp[0]), .dm(dm[0]), .attach(att_hub & hub_en[1]),
        .ext_reset(hub_rst[1])
    );


    // -- SOF on time --
    //
    // A hub times each 1 ms frame from our SOFs and cuts traffic still
    // running at its end, so the SOF must start at the frame tick, not
    // whenever the engine next goes idle. Before the end-of-frame guard
    // (usb_host.v) it went out up to 207 us late here, and on hardware
    // every transfer crossing a frame boundary behind a hub failed.
    integer sof_late_ns, sof_late_max, sof_late_n;
    time sof_tick_t;
    reg sof_tick_seen, sof_busy_q;
    initial begin
        sof_late_max = 0; sof_late_n = 0; sof_tick_seen = 0; sof_busy_q = 0;
    end
    always @(posedge clk) begin
        sof_busy_q <= dut.x_busy;
        if (dut.frame_tick) begin
            sof_tick_t = $time;
            sof_tick_seen = 1;
        end
        if (sof_tick_seen && dut.x_busy && !sof_busy_q && dut.sched_is_sof) begin
            sof_late_ns = $time - sof_tick_t;
            if (sof_late_ns > sof_late_max) sof_late_max = sof_late_ns;
            if (sof_late_ns > 2000) sof_late_n = sof_late_n + 1;
            sof_tick_seen = 0;
        end
    end
    // ---------------------------------------------------------------
    // one Wishbone cycle on the driver's behalf
    // ---------------------------------------------------------------
    //
    // The driver works in byte addresses in the 0xc nibble; the block
    // takes the word-shifted address rtl/sysctl.v hands its slaves.
    // Byte accesses to the packet buffer become a word cycle with one
    // lane selected, which is what a RISC-V sb/lb produces on this bus.

    task bus_cycle;
        input integer a;
        input integer we;
        input integer wd;
        input integer bw;
        begin
            wa = (a & 32'h0fffffff) >> 2;
            lane = a & 3;

            @(posedge clk);
            wb_adr <= wa[10:0];
            wb_we <= we[0];
            if (we && bw) begin
                wb_sel <= (4'b0001 << lane);
                wb_wdat <= wd << (8 * lane);
            end else begin
                wb_sel <= 4'hf;
                wb_wdat <= wd;
            end
            wb_stb <= 1'b1;
            wb_cyc <= 1'b1;
            @(posedge clk);
            guard = 0;
            while (!wb_ack && guard < 1000) begin
                @(posedge clk);
                guard = guard + 1;
            end
            if (guard >= 1000) begin
                $display("  FAIL bus timeout at %08x", a);
                errors = errors + 1;
            end
            if (bw) rdat = (wb_rdat >> (8 * lane)) & 32'hff;
            else rdat = wb_rdat;
`ifdef COSIM_TRACE
            $display("[bus %0t] %s %08x = %08x", $time,
                     we ? "WR" : "RD", a, we ? wd : rdat);
`endif
            wb_stb <= 1'b0;
            wb_cyc <= 1'b0;
            wb_we <= 1'b0;
        end
    endtask

    // Run the driver until it has nothing more to do this step,
    // servicing every bus cycle it asks for.
    task driver_step;
        begin
            $usbh_step(q_valid, q_we, q_adr, q_wdat, q_bw);
            while (q_valid) begin
                bus_cycle(q_adr, q_we, q_wdat, q_bw);
                $usbh_done(rdat, q_valid, q_we, q_adr, q_wdat, q_bw);
            end
        end
    endtask

    // The kernel calls z_usbh_poll() from the ~732 Hz ktimer and from
    // the IRQ 9 handler. Here one step per 64 clocks stands in for
    // both -- the point is that it is periodic and infrequent, not
    // that it is either source.
    task run_for;
        input integer steps;
        begin
            for (k = 0; k < steps; k = k + 1) begin
                driver_step;
                $usbh_tick;
                repeat (64) @(posedge clk);
            end
        end
    endtask

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

    task msc_op;
        input integer op;
        input integer lba;
        begin
            $usbh_msc_arm(op, lba);
            driver_step;
            $usbh_msc_result(rc, bad, first_bad);
        end
    endtask

    // The mouse's cursor, whichever compat block it was given.
    function [9:0] mouse_x;
        input dummy;
        begin
            mouse_x = (typ0 == 2'd2) ? curs_x0 : curs_x1;
        end
    endfunction

    task check_reads;
        input integer count;
        begin
            fails = 0;
            for (n = 0; n < count; n = n + 1) begin
                msc_op(1, n % 6);
                if (rc != 0 || bad != 0) begin
                    fails = fails + 1;
                    $display("  read %0d (lba %0d): rc %0d, %0d bad bytes from %0d",
                             n, n % 6, rc, bad, first_bad);
                end
                run_for(2);
            end
        end
    endtask

    initial begin

        errors = 0;
        rst = 1'b1;
        wb_adr = 11'd0;
        wb_wdat = 32'd0;
        wb_we = 1'b0;
        wb_sel = 4'd0;
        wb_stb = 1'b0;
        wb_cyc = 1'b0;
        att_hub = 1'b0;
        plug_stick = 1'b0;
        plug_mouse = 1'b0;
        plug_bad = 1'b0;

        // Poll the mouse every frame, not every ten.
        #1;
        mouse.cfgd[33] = 8'h01;

        repeat (20) @(posedge clk);
        rst = 1'b0;
        repeat (20) @(posedge clk);

        $display("");
        $display("== hub cosim: real driver, real gateware ==");
        run_for(4);

        // -----------------------------------------------------------
        $display("");
        $display("== enumerate: the hub, then what is behind it ==");
        // Both devices are already in the hub when it is plugged in,
        // which is the order a user produces most often.
        plug_stick = 1'b1;
        plug_mouse = 1'b1;
        // The mouse NAKs its first descriptor INs, as a real keyboard
        // behind a real hub did. Each NAK is retried inside the engine;
        // before retries waited out the device's EOP, every retried PRE
        // was mangled and the mouse needed three attempts -- which the
        // port-reset count below catches.
        mouse.nak_budget = 3;
        att_hub = 1'b1;
        run_for(40000);
        check("hub addressed", (hub.dev_addr != 0) ? 1 : 0, 1);
        check("hub ports powered", hub.hub_powers, 4);
        check("hub port resets (2 devices)", hub.hub_resets, 2);
        check("stick addressed", (dev.dev_addr != 0) ? 1 : 0, 1);
        check("mouse addressed", (mouse.dev_addr != 0) ? 1 : 0, 1);
        check("addresses distinct",
              (hub.dev_addr != dev.dev_addr &&
               hub.dev_addr != mouse.dev_addr &&
               dev.dev_addr != mouse.dev_addr) ? 1 : 0, 1);
        check("mouse bound", (typ0 == 2'd2 || typ1 == 2'd2) ? 1 : 0, 1);
        msc_op(3, 0);
        check("stick claimed: present", rc, 1);

        // -----------------------------------------------------------
        $display("");
        $display("== a mouse report through the hub: PRE, auto-poll ==");
        // Nothing in software: the auto-poll slot reaches the mouse with
        // USE_PRE set, and the report drives the hardware cursor.
        cx = mouse_x(0);
        tmp = mouse.ep1_reports;
        mouse.hid_r0 = 8'h00;
        mouse.hid_r1 = 8'd2;
        mouse.hid_r2 = 8'd0;
        mouse.hid_r3 = 8'h00;
        mouse.hid_have = 1'b1;
        run_for(3000);
        check("report delivered", mouse.ep1_reports - tmp, 1);
        check("cursor moved", mouse_x(0) - cx, 2);

        // -----------------------------------------------------------
        $display("");
        $display("== storage through the hub ==");
        msc_op(0, 0);
        check("z_usbh_msc_start", rc, 0);
        check_reads(12);
        check("reads failed of 12", fails, 0);
        msc_op(2, 7);
        check("write+read rc", rc, 0);
        check("write+read bad bytes", bad, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== unplug the mouse from the hub, then replug ==");
        plug_mouse = 1'b0;
        run_for(8000);
        check("mouse gone: compat block released",
              (typ0 != 2'd2 && typ1 != 2'd2) ? 1 : 0, 1);
        plug_mouse = 1'b1;
        run_for(30000);
        check("mouse re-bound", (typ0 == 2'd2 || typ1 == 2'd2) ? 1 : 0, 1);
        cx = mouse_x(0);
        mouse.hid_r1 = 8'd2;
        mouse.hid_have = 1'b1;
        run_for(3000);
        check("cursor moved after replug", mouse_x(0) - cx, 2);

        // -----------------------------------------------------------
        $display("");
        $display("== the mouse re-enumerating behind the hub DURING reads ==");
        // Hub port requests, the mouse's enumeration and storage
        // commands all on one transaction engine, with the ISR
        // preempting the storage driver between register accesses.
        plug_mouse = 1'b0;
        run_for(8000);
        $usbh_irq(37, 16);
        plug_mouse = 1'b1;
        check_reads(40);
        $usbh_irq(0, 0);
        check("reads failed of 40", fails, 0);
        run_for(30000);
        check("mouse bound after", (typ0 == 2'd2 || typ1 == 2'd2) ? 1 : 0, 1);

        // -----------------------------------------------------------
        $display("");
        $display("== unplug the stick from the hub, then replug ==");
        plug_stick = 1'b0;
        run_for(8000);
        msc_op(3, 0);
        check("stick unbound", rc, 0);
        msc_op(1, 1);
        check("read with no stick fails", rc, 3);
        plug_stick = 1'b1;
        run_for(30000);
        msc_op(3, 0);
        check("stick rebound", rc, 1);
        msc_op(0, 0);
        check("start", rc, 0);
        msc_op(1, 1);
        check("read rc", rc, 0);
        check("read bad bytes", bad, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== unplug the hub: everything behind it goes ==");
        att_hub = 1'b0;
        run_for(8000);
        check("mouse gone", (typ0 != 2'd2 && typ1 != 2'd2) ? 1 : 0, 1);
        msc_op(3, 0);
        check("stick gone", rc, 0);
        att_hub = 1'b1;
        run_for(50000);
        check("mouse back", (typ0 == 2'd2 || typ1 == 2'd2) ? 1 : 0, 1);
        msc_op(3, 0);
        check("stick back", rc, 1);
        msc_op(0, 0);
        check("start", rc, 0);
        msc_op(1, 3);
        check("read rc", rc, 0);
        check("read bad bytes", bad, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== a failed device behind the hub must not poison address 0 ==");
        // As on hardware: a low-speed device fails enumeration while
        // still answering on address 0, with the hub's port for it
        // enabled. Behind a hub every device shares one segment, so the
        // next device to enumerate at address 0 got two answers to each
        // SETUP. The failed device's port has to be disabled, and
        // nothing else may use address 0 while it might be there.
        plug_mouse = 1'b0;
        run_for(8000);
        failer.stall_set_addr = 1'b1;
        plug_bad = 1'b1;
        run_for(40000);
        check("failed device: port disabled", hub.hp_ena[1], 0);
        plug_mouse = 1'b1;
        run_for(30000);
        check("mouse enumerates past it", (typ0 == 2'd2 || typ1 == 2'd2) ? 1 : 0, 1);
        cx = mouse_x(0);
        mouse.hid_r1 = 8'd2;
        mouse.hid_have = 1'b1;
        run_for(3000);
        check("and works", mouse_x(0) - cx, 2);
        plug_bad = 1'b0;
        run_for(8000);

        // -----------------------------------------------------------
        $display("");
        $display("== protocol, from the stick's side ==");
        check("CBWs answered by CSWs",
              dev.msc_cmds - dev.msc_csws - dev.msc_aborted, 0);
        check("out-of-sequence", dev.msc_proto_err, 0);
        check("duplicate OUT", dev.msc_dup_out, 0);
        check("hub: SOFs late >2us", sof_late_n, 0);
        $display("  (latest SOF: %0d ns after its frame tick)", sof_late_max);
        check("hub: bus contention", xcount, 0);
        $display("  (%0d commands, %0d hub port resets, %0d change reports)",
                 dev.msc_cmds, hub.hub_resets, hub.hub_reports);

        $display("");
        if (errors == 0) $display("PASS -- hub cosim ok");
        else $display("FAIL -- %0d check(s) failed", errors);
        $display("");
        $finish;
    end

endmodule
