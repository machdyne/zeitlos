/*
 * Zeitlos SOC -- simulation only.
 *
 * USB ethernet co-simulation: the REAL sw/os/usb driver, including
 * usbh_ecm.c, against the REAL rtl/usb gateware and a CDC-ECM adapter
 * model shaped like the RTL8152 (tb_usb_device.v with ECM=1): its first
 * configuration vendor-specific, its second CDC-ECM. See
 * docs/usb_ethernet.md.
 *
 * As in the storage bench, a low-speed mouse polls every frame on port
 * 1 while the adapter works on port 0, so auto-poll traffic lands
 * between the bulk transactions.
 *
 *   enumerate      configuration 2 chosen over the vendor one; MAC
 *                  string read; SET_INTERFACE alt 1; packet filter
 *                  directed + broadcast
 *   receive        frames of every length that matters: short, exact
 *                  multiples of 64 (zero-length packet), either side of
 *                  the 512-byte chunk, full size; a burst; oversize
 *                  (dropped, next frame intact); too big for the caller
 *   send           the same lengths, checked byte for byte by the model
 *   faults         NAKs mid-frame past the hardware budget, a bad CRC
 *                  mid-frame, an adapter NAKing OUT while its buffer
 *                  is full
 *   link           NETWORK_CONNECTION up and down
 *   unplug         idle and during a burst; replug rebinds; a different
 *                  address wanted by net makes the next bind promiscuous
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

module tb_usb_ecm_cosim;

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
    integer wa, lane, k;
    integer gen0;            // bus_cycle / run_for scratch

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

    // SKEW_NS reproduces the one-sample SE1 real devices emit at every
    // transition -- see tb_usb_device.v. Without it this harness
    // cannot see the receive failure that stopped every full-speed
    // device on hardware, which is how three separate attempts at
    // fixing the receiver were evaluated against a test that could not
    // fail.
    tb_usb_device #(.MODE(0), .MPS0(64), .ECM(1), .SKEW_NS(`DEV_SKEW),
                   .CLK_PPM(`DEV_PPM_FS)) dev (
        .dp(dp[0]), .dm(dm[0]), .attach(att_fs)
    );

    // Port 1: low speed, wired directly, so inverted polarity. Mixing
    // the two speeds across the two ports at once is the case where a
    // shared SIE has to reconfigure itself per transaction -- and
    // where a bug would look like one port working and the other not.
    tb_usb_device #(.MODE(1), .MPS0(8), .SKEW_NS(`DEV_SKEW),
                   .CLK_PPM(`DEV_PPM_LS)) dev1 (
        .dp(dp[1]), .dm(dm[1]), .attach(att_ls)
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
    always @(dp[1] or dm[1])
        if (dp[1] === 1'bx || dm[1] === 1'bx) begin
            xcount = xcount + 1;
            if (xcount <= 4)
                $display("[wire %0t] CONTENTION on port 1", $time);
        end


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

    task ecm_op;
        input integer op;
        input integer arg;
        begin
            $usbh_msc_arm(op, arg);
            driver_step;
            $usbh_msc_result(rc, bad, first_bad);
        end
    endtask

    // Queue one frame on the model and receive it; check length and
    // content.
    task rx_one;
        input integer len;
        input integer seed;
        begin
            dev.ecm_push(len, seed);
            ecm_op(10, 0);
            if (rc != len || bad != 0 || first_bad != seed) begin
                $display("  FAIL rx %0d bytes (seed %0d): got %0d, %0d bad, seed %0d",
                         len, seed, rc, bad, first_bad);
                errors = errors + 1;
            end else begin
                $display("  ok   rx %0d bytes", len);
            end
        end
    endtask

    task tx_one;
        input integer len;
        input integer seed;
        begin
            tmp = dev.ecm_rx_frames;
            tmp2 = dev.ecm_rx_bad;
            ecm_op(11, len | (seed << 16));
            // The host's engine completes when it sees the adapter's
            // ACK begin; the model counts the frame after its ACK's EOP.
            repeat (200) @(posedge clk);
            if (rc != 0 || dev.ecm_rx_frames != tmp + 1 ||
                dev.ecm_rx_last != len || dev.ecm_rx_bad != tmp2 ||
                dev.ecm_rb[0] !== seed[7:0]) begin
                $display("  FAIL tx %0d bytes: rc %0d, frames +%0d, last %0d, bad +%0d",
                         len, rc, dev.ecm_rx_frames - tmp, dev.ecm_rx_last,
                         dev.ecm_rx_bad - tmp2);
                errors = errors + 1;
            end else begin
                $display("  ok   tx %0d bytes", len);
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
        att_fs = 1'b0;
        att_ls = 1'b0;

        // Poll the mouse every frame, not every ten.
        #1;
        dev1.cfgd[33] = 8'h01;

        repeat (20) @(posedge clk);
        rst = 1'b0;
        repeat (20) @(posedge clk);

        $display("");
        $display("== ecm cosim: real driver, real gateware ==");

        run_for(4);
        att_ls = 1'b1;
        run_for(12000);
        check("mouse bound", typ0, 2);

        att_fs = 1'b1;
        run_for(14000);

        // -----------------------------------------------------------
        $display("");
        $display("== enumeration ==");
        check("configuration 2 (ECM) chosen", dev.ecm_cfg, 2);
        check("data interface alt 1", dev.ecm_alt, 1);
        check("filter directed+broadcast", dev.ecm_filter, 8'h0c);
        ecm_op(9, 0);
        check("present+mac, link ?, no promisc", rc, 3);
        check("MAC bytes wrong", bad, 0);
        gen0 = first_bad;
        ecm_op(12, 0);                  // net: "I send from your MAC"
        $usbh_msc_arm(4, 0);            // lsusb into the log
        driver_step;

        // -----------------------------------------------------------
        $display("");
        $display("== receive ==");
        ecm_op(10, 0);
        check("idle receive returns 0", rc, 0);
        rx_one(60, 1);
        rx_one(64, 2);                  // one full packet, then a ZLP
        rx_one(100, 3);
        rx_one(128, 4);
        rx_one(511, 5);
        rx_one(512, 6);                 // exactly one chunk, then a ZLP
        rx_one(513, 7);
        rx_one(1024, 8);
        rx_one(1472, 9);                // 23 x 64
        rx_one(1514, 10);
        ecm_op(10, 0);
        check("queue empty after", rc, 0);

        $display("");
        $display("== receive, a burst of five ==");
        for (n = 0; n < 5; n = n + 1) dev.ecm_push(200 + n * 300, 20 + n);
        fails = 0;
        for (n = 0; n < 5; n = n + 1) begin
            ecm_op(10, 0);
            if (rc != 200 + n * 300 || bad != 0 || first_bad != 20 + n)
                fails = fails + 1;
        end
        check("burst frames wrong of 5", fails, 0);

        $display("");
        $display("== receive, NAKs mid-frame past the hardware budget ==");
        dev.ecm_naks_mid = 6;
        rx_one(1514, 30);
        rx_one(700, 31);
        dev.ecm_naks_mid = 0;

        $display("");
        $display("== receive, a bad CRC mid-frame ==");
        dev.ecm_crc_at = 5;
        dev.ecm_crc_bad = 1;
        rx_one(1514, 32);
        dev.ecm_crc_at = 0;
        dev.ecm_crc_bad = 2;
        rx_one(300, 33);
        dev.ecm_crc_bad = 0;

        $display("");
        $display("== receive, oversize and too-big-for-caller ==");
        ecm_op(13, 0);
        tmp = rc;
        dev.ecm_push(1600, 40);
        ecm_op(10, 0);
        check("1600-byte frame dropped", rc, 0);
        check("nothing past the buffer written", bad, 0);
        ecm_op(13, 0);
        check("rx_drop counted", rc - tmp, 1);
        rx_one(90, 41);
        dev.ecm_push(400, 42);
        ecm_op(14, 0);                  // into a 100-byte buffer
        check("400 bytes into 100: dropped", rc, 0);
        check("100-byte buffer overrun", bad, 0);
        rx_one(90, 43);

        // -----------------------------------------------------------
        $display("");
        $display("== send ==");
        tx_one(60, 50);
        tx_one(64, 51);                 // needs a ZLP after it
        tx_one(100, 52);
        tx_one(512, 53);
        tx_one(513, 54);
        tx_one(1024, 55);
        tx_one(1514, 56);
        check("model: ZLP with no frame", dev.ecm_rx_zlp_only, 0);

        $display("");
        $display("== send, the adapter NAKing while its buffer is full ==");
        dev.ecm_out_naks = 12;
        tx_one(1514, 57);
        tx_one(60, 58);

        // -----------------------------------------------------------
        $display("");
        $display("== link notifications ==");
        dev.ecm_notify = 1;
        run_for(200);                   // past the driver's link poll interval
        ecm_op(10, 0);
        ecm_op(9, 0);
        check("link up", (rc >> 2) & 3, 1);
        dev.ecm_notify = 2;
        run_for(200);
        ecm_op(10, 0);
        ecm_op(9, 0);
        check("link down", (rc >> 2) & 3, 2);

        // -----------------------------------------------------------
        $display("");
        $display("== unplug, replug ==");
        att_fs = 1'b0;
        run_for(3000);
        ecm_op(9, 0);
        check("unbound", rc & 1, 0);
        check("generation moved", (first_bad != gen0) ? 1 : 0, 1);
        ecm_op(10, 0);
        check("receive with no adapter", rc, -1);
        ecm_op(11, 60 | (60 << 16));
        check("send with no adapter", rc, -1);
        att_fs = 1'b1;
        run_for(14000);
        ecm_op(9, 0);
        check("rebound, same MAC, no promisc", rc & 17, 1);
        rx_one(1514, 61);
        tx_one(1514, 62);

        $display("");
        $display("== net wants a different address: next bind promiscuous ==");
        ecm_op(12, 1);
        att_fs = 1'b0;
        run_for(3000);
        att_fs = 1'b1;
        run_for(14000);
        check("filter has PROMISCUOUS", dev.ecm_filter, 8'h0d);
        ecm_op(9, 0);
        check("promisc reported", (rc >> 4) & 1, 1);
        rx_one(1000, 63);
        ecm_op(12, 0);

        $display("");
        $display("== unplug during receives, interrupts emulated ==");
        $usbh_irq(37, 16);
        for (n = 0; n < 8; n = n + 1) dev.ecm_push(1514, 70 + n);
        fork
            begin #400000; att_fs = 1'b0; end
            begin
                fails = 0;
                for (n = 0; n < 8; n = n + 1) begin
                    ecm_op(10, 0);
                    if (rc > 0 && (rc != 1514 || bad != 0)) fails = fails + 1;
                end
            end
        join
        $usbh_irq(0, 0);
        check("partial frames delivered", fails, 0);
        run_for(3000);
        ecm_op(9, 0);
        check("unbound", rc & 1, 0);
        // The queue still holds what was not sent; a fresh adapter
        // starts empty.
        dev.ecm_qh = dev.ecm_qt;
        att_fs = 1'b1;
        run_for(14000);
        ecm_op(9, 0);
        check("rebound", rc & 1, 1);
        rx_one(777, 80);
        tx_one(777, 81);

        // -----------------------------------------------------------
        $display("");
        $display("== protocol ==");
        check("data ep before SET_INTERFACE", dev.ecm_bad_ep, 0);
        check("duplicate OUT", dev.ecm_dup_out, 0);
        check("ecm: SOFs late >2us", sof_late_n, 0);
        $display("  (latest SOF: %0d ns after its frame tick)", sof_late_max);
        check("ecm: bus contention", xcount, 0);
        check("mouse still bound", (typ0 == 2 || typ1 == 2) ? 1 : 0, 1);
        $display("  (%0d frames sent by the model, %0d received)",
                 dev.ecm_sent, dev.ecm_rx_frames);

        $display("");
        if (errors == 0) $display("PASS -- ecm cosim ok");
        else $display("FAIL -- %0d check(s) failed", errors);
        $display("");
        $finish;
    end

endmodule
