/*
 * Zeitlos -- simulation only.
 *
 * Co-simulation: the real sw/os/usb driver against the real rtl/usb
 * gateware. See rtl/tb/cosim/usbh_vpi.c for how the two are joined.
 *
 * This is the test that neither half could do alone. tb_usb_host.v
 * proves the gateware answers correctly when driven by Verilog tasks
 * written alongside it; compiling the driver proves it is valid C.
 * Neither says whether the driver's idea of the register map matches
 * the hardware's, whether the enumeration sequence is one a real
 * device will answer, or whether "poll until not pending" terminates.
 *
 * What it checks: a device plugged into port 0 is detected, reset,
 * enumerated through the full chapter-9 sequence by the driver, bound
 * by the HID driver, and ends up with the compat block reporting a
 * mouse and the auto-poll slot running -- at which point a report
 * moves the hardware cursor with the CPU no longer involved.
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

module tb_usb_cosim;

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
    tb_usb_device #(.MODE(0), .MPS0(8), .SKEW_NS(`DEV_SKEW),
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
        integer wa;
        integer lane;
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
        integer k;
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

        repeat (20) @(posedge clk);
        rst = 1'b0;
        repeat (20) @(posedge clk);

        $display("");
        $display("== cosim: real driver, real gateware ==");

        // z_usbh_init() runs on the first step.
        run_for(4);

        // Plug a full-speed device into port 0.
        att_fs = 1'b1;

        // Enumeration: debounce, reset, GET_DESCRIPTOR(8), SET_ADDRESS,
        // GET_DESCRIPTOR(18), GET_DESCRIPTOR(CONFIG), SET_CONFIGURATION,
        // then the HID bind. Every one of those is the driver's own
        // code deciding what to do next.
        run_for(4000);

        check("device enumerated", dev.dev_addr, 1);
        check("setups seen by device", (dev.setup_count >= 5) ? 1 : 0, 1);
        check("compat block 0 is a mouse", typ0, 2);

        // The slot is running: from here the CPU is out of the loop.
        cx = curs_x0;
        dev.hid_r0 = 8'h00;
        dev.hid_r1 = 8'd10;
        dev.hid_r2 = 8'd0;
        dev.hid_r3 = 8'h00;
        dev.hid_have = 1'b1;
        // The descriptor asked for bInterval 10, so the slot polls
        // once every 10 frames -- 10 ms, 480000 clocks. Waiting less
        // than that and concluding the cursor is broken would be a
        // testbench bug, not a design one.
        repeat (700000) @(posedge clk);

        check("report delivered", (dev.ep1_reports >= 1) ? 1 : 0, 1);
        check("cursor moved with no CPU", curs_x0 - cx, 20);

        // -----------------------------------------------------------
        $display("");
        $display("== hot unplug ==");

        att_fs = 1'b0;
        run_for(300);

        check("typ cleared on unplug", typ0, 0);
        check("poll slot stopped", dut.poll_a[0][30], 0);

        // -----------------------------------------------------------
        $display("");
        $display("== replug, same port ==");

        att_fs = 1'b1;
        run_for(4000);

        check("re-enumerated", dev.dev_addr, 1);
        check("typ restored", typ0, 2);

        cx = curs_x0;
        dev.hid_r1 = 8'd10;
        dev.hid_r2 = 8'd0;
        dev.hid_have = 1'b1;
        repeat (700000) @(posedge clk);
        check("cursor works after replug", curs_x0 - cx, 20);

        // -----------------------------------------------------------
        $display("");
        $display("== second port, low speed, both at once ==");

        att_ls = 1'b1;
        run_for(12000);

        check("port 0 still a mouse", typ0, 2);
        check("port 1 enumerated", dev1.dev_addr, 2);
        check("port 1 is a mouse", typ1, 2);
        // Distinct addresses matter: address 0 is the enumeration
        // address and only one device may hold it at a time, which is
        // what E_DEBOUNCE serialises.
        check("addresses distinct",
              (dev.dev_addr != dev1.dev_addr) ? 1 : 0, 1);

        // Both cursors move, each from its own compat block, at two
        // different bit rates on one shared SIE.
        cx = curs_x1;
        dev1.hid_r1 = 8'd10;
        dev1.hid_r2 = 8'd0;
        dev1.hid_have = 1'b1;
        repeat (900000) @(posedge clk);
        check("low-speed cursor moved", curs_x1 - cx, 20);

        // -----------------------------------------------------------
        $display("");
        $display("== unplug one port, other keeps working ==");

        att_fs = 1'b0;
        run_for(400);

        check("port 0 cleared", typ0, 0);
        check("port 1 untouched", typ1, 2);

        cx = curs_x1;
        dev1.hid_r1 = 8'd10;
        dev1.hid_have = 1'b1;
        repeat (900000) @(posedge clk);
        check("port 1 still moving", curs_x1 - cx, 20);

        // -----------------------------------------------------------
        // Three plug cycles in total by now. The compat blocks are a
        // pool of two and are claimed on bind; if they are not
        // released on unplug this is where it shows -- not on the
        // first replug, on the third.
        $display("");
        $display("== plug cycles do not leak compat blocks ==");

        att_fs = 1'b1;
        run_for(4000);
        check("third plug still binds", typ0, 2);

        $display("");
        if (errors == 0) $display("PASS -- cosim ok");
        else $display("FAIL -- %0d check(s) failed", errors);
        $display("");
        $finish;

    end

    initial begin
        #900000000;
        $display("FAIL -- cosim global timeout");
        $finish;
    end

endmodule
