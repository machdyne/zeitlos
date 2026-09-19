/*
 * Zeitlos SOC -- simulation only.
 *
 * Mass storage co-simulation: the REAL sw/os/usb driver, including
 * usbh_msc.c, against the REAL rtl/usb gateware and a bulk-only SCSI
 * device model (tb_usb_device.v with MSC=1).
 *
 * Until this existed, nothing had run a mass-storage transfer before
 * hardware did. That is how a 512-byte READ(10) returning 13 bytes was
 * debugged on a board for several rounds.
 *
 * The setup is the hardware one that failed: a low-speed mouse on port
 * 1 with its auto-poll slot live and NAKing, and the drive on port 0.
 * The mouse polls every frame here rather than every 10, so the
 * windows in which a poll can land next to a bulk transaction come up
 * in dozens of reads rather than thousands.
 *
 * What it checks, each read compared byte for byte with the model's
 * medium after the buffer is poisoned:
 *
 *   start          TEST UNIT READY and READ CAPACITY through the driver
 *   reads          single-sector READ(10), repeated, polls running
 *   NAK, in budget the device pauses after the first packet for fewer
 *                  NAKs than the hardware retries
 *   NAK, over      ...for more, so the transfer ends part-way and the
 *                  driver has to RESUME it -- the case that loses data
 *                  if act_len is ignored on a NAK
 *   write          WRITE(10) then read back
 *   protocol       the model saw every CBW answered by exactly one CSW
 *                  and nothing out of sequence
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

module tb_usb_msc_cosim;

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
    integer tmp;
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

    // SKEW_NS reproduces the one-sample SE1 real devices emit at every
    // transition -- see tb_usb_device.v. Without it this harness
    // cannot see the receive failure that stopped every full-speed
    // device on hardware, which is how three separate attempts at
    // fixing the receiver were evaluated against a test that could not
    // fail.
    tb_usb_device #(.MODE(0), .MPS0(64), .MSC(1), .SKEW_NS(`DEV_SKEW),
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
        $display("== msc cosim: real driver, real gateware ==");

        run_for(4);

        // The mouse first, so its slot is live before the drive arrives.
        att_ls = 1'b1;
        run_for(12000);
        check("mouse bound", typ0, 2);

        att_fs = 1'b1;
        run_for(12000);
        check("drive enumerated", (dev.dev_addr != 0) ? 1 : 0, 1);

        // -----------------------------------------------------------
        $display("");
        $display("== start ==");
        msc_op(0, 0);
        check("z_usbh_msc_start", rc, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== reads, polls running ==");
        fails = 0;
        for (n = 0; n < 40; n = n + 1) begin
            msc_op(1, n % 7);
            if (rc != 0 || bad != 0) begin
                fails = fails + 1;
                $display("  read %0d (lba %0d): rc %0d, %0d bad bytes from %0d",
                         n, n % 7, rc, bad, first_bad);
            end
            // Let frames and polls run between commands, as FatFs's
            // own gaps would.
            run_for(1 + (n * 37) % 23);
        end
        check("reads failed of 40", fails, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== NAK after first packet, within hardware budget ==");
        dev.msc_naks_mid = 2;
        msc_op(1, 3);
        check("read rc", rc, 0);
        check("read bad bytes", bad, 0);

        $display("");
        $display("== NAK after first packet, OVER hardware budget ==");
        dev.msc_naks_mid = 6;
        msc_op(1, 4);
        check("read rc", rc, 0);
        check("read bad bytes", bad, 0);
        dev.msc_naks_mid = 0;

        // After whatever that did, the stream must still be in step.
        msc_op(1, 0);
        check("next read rc", rc, 0);
        check("next read bad bytes", bad, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== bit error mid-sector: one bad CRC after 3 packets ==");
        // The host must refuse the packet (no ACK), keep the three
        // packets that already landed, and resume at the fourth with
        // the same toggle. Giving up here would drop a whole sector
        // for one flipped bit.
        dev.msc_crc_at = 3;
        dev.msc_crc_bad = 1;
        tmp = dev.msc_crc_sent;
        msc_op(1, 5);
        check("corrupt packets sent", dev.msc_crc_sent - tmp, 1);
        check("read rc", rc, 0);
        check("read bad bytes", bad, 0);

        $display("");
        $display("== bit errors: two in a row on the first packet ==");
        dev.msc_crc_at = 0;
        dev.msc_crc_bad = 2;
        tmp = dev.msc_crc_sent;
        msc_op(1, 6);
        check("corrupt packets sent", dev.msc_crc_sent - tmp, 2);
        check("read rc", rc, 0);
        check("read bad bytes", bad, 0);
        dev.msc_crc_bad = 0;

        msc_op(1, 0);
        check("next read rc", rc, 0);
        check("next read bad bytes", bad, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== write, read back ==");
        msc_op(2, 7);
        check("write+read rc", rc, 0);
        check("write+read bad bytes", bad, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== protocol, from the device's side ==");
        check("CBWs answered by CSWs", dev.msc_cmds - dev.msc_csws, 0);
        check("out-of-sequence", dev.msc_proto_err, 0);
        check("duplicate OUT", dev.msc_dup_out, 0);
        $display("  (%0d commands)", dev.msc_cmds);

        $display("");
        if (errors == 0) $display("PASS -- msc cosim ok");
        else $display("FAIL -- %0d check(s) failed", errors);
        $display("");
        $finish;
    end

endmodule
