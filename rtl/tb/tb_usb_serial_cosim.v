/*
 * Zeitlos SOC -- simulation only.
 *
 * USB serial co-simulation: the REAL sw/os/usb driver, including the
 * USB serial dispatch (usbh_cdc.c) and the CP210x driver
 * (usbh_cp210x.c), against the REAL rtl/usb gateware and two serial
 * device models (tb_usb_device.v, SER=1 and SER=2). See
 * docs/usb_host.md, "USB serial devices".
 *
 *   port 0   a CP2102 (10c4:ea60), the bridge on a Heltec V3
 *   port 1   a CDC-ACM device, for the path this refactor moved
 *
 *   enumerate   CP2102: IFC_ENABLE, SET_BAUDRATE 115200, SET_LINE_CTL
 *               8N1, SET_MHS -- in that order, DTR and RTS set in ONE
 *               request so an ESP32's auto-reset circuit never fires
 *   receive     a byte, a packet, a packet and one, bursts, short
 *               packets, NAKs with data waiting
 *   send        short, exact multiples of 64, long, a device NAKing
 *   refusals    an optional request STALLed: bound anyway; IFC_ENABLE
 *               STALLed: not bound, and no data path
 *   unplug      reads and writes fail; replug rebinds with a fresh line
 *   one at a    a CDC-ACM device plugged in while the CP2102 is bound
 *   time        is left alone; alone, it binds as ACM with its own
 *               two requests, and carries data
 */

`timescale 1ns/1ps

`ifndef DEV_SKEW
`define DEV_SKEW 0
`endif

module tb_usb_serial_cosim;

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

    reg att_cp;
    reg att_acm;

    // VPI handshake
    integer q_valid, q_we, q_adr, q_wdat, q_bw;
    integer rdat;
    integer guard;
    integer errors;
    integer rc, bad, first_bad, n, m;
    integer wa, lane, k;
    integer cp_tx, acm_tx;      // bytes pushed into each model so far
    integer cp_rx, acm_rx;      // bytes of each model's ser_rb checked
    integer mism;

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

    tb_usb_device #(.MODE(0), .MPS0(64), .SER(1), .SKEW_NS(`DEV_SKEW)) cp (
        .dp(dp[0]), .dm(dm[0]), .attach(att_cp)
    );

    tb_usb_device #(.MODE(0), .MPS0(64), .SER(2), .SKEW_NS(`DEV_SKEW)) acm (
        .dp(dp[1]), .dm(dm[1]), .attach(att_acm)
    );

    // -- bus contention: the host and a device driving at once --
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

    // rtl/tb/cosim/usbh_vpi.c's ser_pat, byte for byte.
    function [7:0] ser_pat;
        input integer pos;
        begin
            ser_pat = (pos * 13 + (pos >> 8) * 7 + 8'h21) & 8'hff;
        end
    endfunction

    // ---------------------------------------------------------------
    // one Wishbone cycle on the driver's behalf; see tb_usb_ecm_cosim.v
    // ---------------------------------------------------------------

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

    task driver_step;
        begin
            $usbh_step(q_valid, q_we, q_adr, q_wdat, q_bw);
            while (q_valid) begin
                bus_cycle(q_adr, q_we, q_wdat, q_bw);
                $usbh_done(rdat, q_valid, q_we, q_adr, q_wdat, q_bw);
            end
        end
    endtask

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

    task ser_op;
        input integer op;
        input integer arg;
        begin
            $usbh_msc_arm(op, arg);
            driver_step;
            $usbh_msc_result(rc, bad, first_bad);
        end
    endtask

    // Queue len pattern bytes on the CP2102 and read them back.
    task cp_rx_n;
        input integer len;
        begin
            for (n = 0; n < len; n = n + 1) begin
                cp.ser_push(ser_pat(cp_tx));
                cp_tx = cp_tx + 1;
            end
            ser_op(16, len);
            if (rc != len || bad != 0) begin
                $display("  FAIL rx %0d bytes: got %0d, %0d bad, %0d packets",
                         len, rc, bad, first_bad);
                errors = errors + 1;
            end else begin
                $display("  ok   rx %0d bytes in %0d packet(s)", len, first_bad);
            end
        end
    endtask

    task acm_rx_n;
        input integer len;
        begin
            for (n = 0; n < len; n = n + 1) begin
                acm.ser_push(ser_pat(acm_tx));
                acm_tx = acm_tx + 1;
            end
            ser_op(16, len);
            if (rc != len || bad != 0) begin
                $display("  FAIL acm rx %0d bytes: got %0d, %0d bad",
                         len, rc, bad);
                errors = errors + 1;
            end else begin
                $display("  ok   acm rx %0d bytes", len);
            end
        end
    endtask

    // Send len pattern bytes; check the CP2102 model received exactly
    // them, in order, once.
    task cp_tx_n;
        input integer len;
        begin
            ser_op(17, len);
            // The engine completes on the device's ACK; the model
            // stores the packet after its ACK's EOP.
            repeat (200) @(posedge clk);
            mism = 0;
            for (n = cp_rx; n < cp.ser_rn && n < 4096; n = n + 1)
                if (cp.ser_rb[n] !== ser_pat(n)) mism = mism + 1;
            if (rc != len || cp.ser_rn != cp_rx + len || mism != 0) begin
                $display("  FAIL tx %0d bytes: rc %0d, model got %0d, %0d wrong",
                         len, rc, cp.ser_rn - cp_rx, mism);
                errors = errors + 1;
            end else begin
                $display("  ok   tx %0d bytes", len);
            end
            cp_rx = cp.ser_rn;
        end
    endtask

    task acm_tx_n;
        input integer len;
        begin
            ser_op(17, len);
            repeat (200) @(posedge clk);
            mism = 0;
            for (n = acm_rx; n < acm.ser_rn && n < 4096; n = n + 1)
                if (acm.ser_rb[n] !== ser_pat(n)) mism = mism + 1;
            if (rc != len || acm.ser_rn != acm_rx + len || mism != 0) begin
                $display("  FAIL acm tx %0d bytes: rc %0d, model got %0d, %0d wrong",
                         len, rc, acm.ser_rn - acm_rx, mism);
                errors = errors + 1;
            end else begin
                $display("  ok   acm tx %0d bytes", len);
            end
            acm_rx = acm.ser_rn;
        end
    endtask

    // A fresh device on both sides of the pattern: the harness's
    // running counts, the testbench's, and whatever the model holds.
    task cp_fresh;
        begin
            ser_op(18, 0);
            cp.ser_qh = cp.ser_qt;
            cp.ser_rn = 0;
            cp_tx = 0;
            cp_rx = 0;
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
        att_cp = 1'b0;
        att_acm = 1'b0;
        cp_tx = 0; cp_rx = 0; acm_tx = 0; acm_rx = 0;

        repeat (20) @(posedge clk);
        rst = 1'b0;
        repeat (20) @(posedge clk);

        $display("");
        $display("== usb serial cosim: real driver, real gateware ==");

        run_for(4);
        att_cp = 1'b1;
        run_for(12000);

        // -----------------------------------------------------------
        $display("");
        $display("== CP2102: enumeration ==");
        ser_op(15, 0);
        check("bound as cp210x (1 | 2<<1)", rc, 5);
        check("IFC_ENABLE in force", cp.ser_en, 1);
        check("baud", cp.ser_baud, 115200);
        check("line control 8N1", cp.ser_lctl, 16'h0800);
        check("requests sent", cp.ser_reqs, 4);
        check("  1st IFC_ENABLE", cp.ser_req_log[0], 8'h00);
        check("  2nd SET_BAUDRATE", cp.ser_req_log[1], 8'h1e);
        check("  3rd SET_LINE_CTL", cp.ser_req_log[2], 8'h03);
        check("  4th SET_MHS", cp.ser_req_log[3], 8'h07);
        check("SET_MHS requests", cp.ser_mhs_n, 1);
        check("DTR", cp.ser_dtr, 1);
        check("RTS", cp.ser_rts, 1);
        check("auto-reset glitches", cp.ser_glitch, 0);
        check("requests not understood", cp.ser_unknown, 0);
        $usbh_msc_arm(4, 0);            // lsusb into the log
        driver_step;

        // -----------------------------------------------------------
        $display("");
        $display("== CP2102: receive ==");
        ser_op(16, 0);
        check("idle read returns 0", rc, 0);
        cp_rx_n(1);
        cp_rx_n(63);
        cp_rx_n(64);
        cp_rx_n(65);
        cp_rx_n(200);
        cp_rx_n(1000);
        cp_rx_n(4000);
        ser_op(16, 0);
        check("empty after", rc, 0);

        $display("");
        $display("== CP2102: receive, short packets and NAKs ==");
        cp.ser_short = 7;
        cp_rx_n(300);
        cp.ser_short = 0;
        cp.ser_in_naks = 25;
        cp_rx_n(500);
        cp.ser_in_naks = 0;

        // -----------------------------------------------------------
        $display("");
        $display("== CP2102: send ==");
        cp_tx_n(1);
        cp_tx_n(63);
        cp_tx_n(64);
        cp_tx_n(65);
        cp_tx_n(128);
        cp_tx_n(1000);

        $display("");
        $display("== CP2102: send, the device NAKing ==");
        cp.ser_out_naks = 60;
        cp_tx_n(300);
        check("duplicate OUT", cp.ser_dup_out, 0);

        // -----------------------------------------------------------
        $display("");
        $display("== CP2102: unplug ==");
        att_cp = 1'b0;
        run_for(3000);
        ser_op(15, 0);
        check("unbound", rc, 0);
        ser_op(16, 0);
        check("read with no device", rc, -1);
        ser_op(17, 10);
        check("write with no device", rc, -1);

        $display("");
        $display("== CP2102: replug ==");
        cp_fresh;
        att_cp = 1'b1;
        run_for(12000);
        ser_op(15, 0);
        check("rebound", rc, 5);
        check("IFC_ENABLE in force again", cp.ser_en, 1);
        check("SET_MHS requests, total", cp.ser_mhs_n, 2);
        check("auto-reset glitches", cp.ser_glitch, 0);
        cp_rx_n(333);
        cp_tx_n(333);

        // -----------------------------------------------------------
        $display("");
        $display("== CP2102: an optional request refused ==");
        att_cp = 1'b0;
        run_for(3000);
        cp_fresh;
        cp.ser_stall_en = 1'b1;
        cp.ser_stall_req = 8'h03;       // SET_LINE_CTL
        m = cp.ser_reqs;
        att_cp = 1'b1;
        run_for(12000);
        ser_op(15, 0);
        check("bound anyway", rc, 5);
        check("requests sent", cp.ser_reqs - m, 4);
        check("stalled", cp.ser_stalls, 1);
        check("SET_MHS still sent", cp.ser_mhs_n, 3);
        cp_rx_n(100);
        cp_tx_n(100);

        $display("");
        $display("== CP2102: IFC_ENABLE refused ==");
        att_cp = 1'b0;
        run_for(3000);
        cp_fresh;
        cp.ser_stall_req = 8'h00;       // IFC_ENABLE
        att_cp = 1'b1;
        run_for(40000);
        ser_op(15, 0);
        check("not bound", rc, 0);
        check("UART never enabled", cp.ser_en, 0);
        $usbh_msc_arm(4, 0);            // lsusb: where it failed
        driver_step;
        cp.ser_stall_en = 1'b0;
        att_cp = 1'b0;
        run_for(3000);
        cp_fresh;
        att_cp = 1'b1;
        run_for(12000);
        ser_op(15, 0);
        check("bound after replug", rc, 5);
        cp_rx_n(64);

        // -----------------------------------------------------------
        $display("");
        $display("== one at a time: CDC-ACM plugged in beside it ==");
        att_acm = 1'b1;
        run_for(12000);
        ser_op(15, 0);
        check("still the cp210x", rc, 5);
        cp_rx_n(10);

        $display("");
        $display("== CDC-ACM alone ==");
        att_cp = 1'b0;
        att_acm = 1'b0;
        run_for(3000);
        ser_op(18, 0);
        m = acm.ser_reqs;
        att_acm = 1'b1;
        run_for(12000);
        ser_op(15, 0);
        check("bound as acm (1 | 1<<1)", rc, 3);
        check("requests sent", acm.ser_reqs - m, 2);
        check("  SET_LINE_CODING", acm.ser_req_log[m], 8'h20);
        check("  SET_CONTROL_LINE_STATE", acm.ser_req_log[m + 1], 8'h22);
        check("line coding baud", acm.ser_baud, 115200);
        check("line coding 8N1", acm.ser_lctl, 16'h0800);
        check("DTR", acm.ser_dtr, 1);
        check("RTS", acm.ser_rts, 1);
        check("requests not understood", acm.ser_unknown, 0);
        acm.ser_qh = acm.ser_qt;
        acm_tx = 0;
        acm.ser_rn = 0;
        acm_rx = 0;
        acm_rx_n(1);
        acm_rx_n(64);
        acm_rx_n(700);
        acm_tx_n(1);
        acm_tx_n(64);
        acm_tx_n(700);
        $usbh_msc_arm(4, 0);
        driver_step;

        // -----------------------------------------------------------
        $display("");
        $display("== protocol ==");
        check("data endpoint before enable", cp.ser_order_err, 0);
        check("acm: duplicate OUT", acm.ser_dup_out, 0);
        check("bus contention", xcount, 0);

        $display("");
        if (errors == 0) $display("PASS -- usb serial cosim ok");
        else $display("FAIL -- %0d check(s) failed", errors);
        $display("");
        $finish;
    end

endmodule
