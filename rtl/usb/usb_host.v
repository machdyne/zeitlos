/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host controller -- Wishbone front end, packet buffer, scheduler.
 *
 * Clean-room from the USB 2.0 specification; see docs/usb_host.md for
 * the architecture, the register map and the reasoning behind the
 * hardware/software split.
 *
 * Phase 1 scope. The HID compatibility block (reg_usbN_*) and the
 * auto-poll table are Phase 2 and are not here yet; this module is
 * built and tested standalone against rtl/tb/tb_usb_host.v before it
 * goes anywhere near rtl/sysctl.v.
 *
 * -- the packet buffer is byte-wide on purpose --
 *
 * 2 KB, one DP16KD. An ECP5 block RAM is 18 kbit and its widest port
 * is 18 bits, so a 512x32 buffer would need TWO of them -- and
 * docs/audio.md records Lakritz sitting at 55 of 56 block RAMs. There
 * is exactly one to spend.
 *
 * So the memory is 2048x8 and the Wishbone side assembles a 32-bit
 * word from four byte accesses. That costs about eight clocks per
 * word: 512 bytes is 128 words, roughly 1024 clocks, about 21 us at
 * 48 MHz. Against the ~420 us of wire time a 512-byte sector takes at
 * full speed, that is five percent. Paying five percent of a transfer
 * that is already bounded by a 12 Mbps bus to save a block RAM on a
 * part that has one left is not a close call.
 *
 * Wishbone tolerates the extra latency without any special handling --
 * the bus is ack-driven and rtl/sysctl.v is full of slaves that take
 * their time.
 */

module usb_host #(
    parameter PORTS = 2,
    parameter T_MS = 32'd48000,
    // The frame interval, separately from the port timers above.
    // Identical on hardware; a testbench shortens T_MS so a 100 ms
    // attach debounce does not cost five million cycles per plug
    // event, and must NOT shorten this with it -- frames arriving
    // every two microseconds would leave the engine permanently busy
    // emitting SOF and starve every transaction under test.
    parameter T_FRAME = 32'd48000,
    parameter DEBOUNCE_MS = 32'd100,
    parameter RESET_MS = 32'd10,
    parameter RECOVERY_MS = 32'd10,
    parameter FS_DIV = 6'd4,
    parameter LS_DIV = 6'd32,
    // Pointer sensitivity, passed straight through to
    // usb_hid_compat.v. Set per board from rtl/boards.vh's
    // USB_HID_SENS_SHIFT, exactly as rtl/usb_hid.v takes it today.
    parameter SENS_SHIFT = 0,
    parameter POLL_SLOTS = 4
) (
    input wire wb_clk_i,
    input wire wb_rst_i,
    input wire [10:0] wb_adr_i,
    input wire [31:0] wb_dat_i,
    output reg [31:0] wb_dat_o,
    input wire wb_we_i,
    input wire [3:0] wb_sel_i,
    input wire wb_stb_i,
    output reg wb_ack_o,
    input wire wb_cyc_i,

    inout wire [PORTS-1:0] usb_dp,
    inout wire [PORTS-1:0] usb_dm,

    // -- HID compatibility blocks --
    // Two of them, exposed separately, because that is how rtl/sysctl.v
    // wires the two usb_hid_wb instances today: one cursor pair per
    // block and one interrupt per block. Keeping the shape identical
    // is what makes this a drop-in.
    output wire [9:0] curs_x0,
    output wire [9:0] curs_y0,
    output wire [9:0] curs_x1,
    output wire [9:0] curs_y1,
    output wire [1:0] typ0,
    output wire [1:0] typ1,
    output wire hid0_int_o,
    output wire hid1_int_o,

    // Transmitter active, brought out for rtl/probe.v to trigger on.
    // A hierarchical reference from sysctl.v does NOT work -- yosys
    // implicitly declares the name as a fresh undriven wire and only
    // warns, so the probe armed and never fired.
    output wire tx_active_o,
    // Which port that transmission is going to, so a probe watching
    // one port's pins can ignore traffic aimed at the other.
    output wire tx_port_o,

    output wire int_o
);

    localparam MAGIC = 12'h05b;

    // Word addresses inside the 0xC nibble. Byte 0xc000_0100 is word
    // 0x40 because rtl/sysctl.v hands slaves wbm_adr_sel[27:2].
    localparam A_CTRL     = 11'h040;
    localparam A_PORTSTAT = 11'h041;
    localparam A_IRQSTAT  = 11'h042;
    localparam A_IRQEN    = 11'h043;
    localparam A_XACT_A   = 11'h044;
    localparam A_XACT_B   = 11'h045;
    localparam A_XACT_S   = 11'h046;
    localparam A_CONFIG   = 11'h047;
    // Bring-up counters. Not part of the programming model -- they
    // answer "is anything happening on the wire at all", which is the
    // first question when a port that looks healthy never enumerates,
    // and which nothing else in this register map can answer.
    localparam A_DEBUG0   = 11'h048;
    localparam A_DEBUG1   = 11'h049;

    // Auto-poll table: 0xc000_0200 is word 0x080, two words per slot.
    localparam A_POLL     = 11'h080;

    localparam W_IDLE  = 3'd0;
    localparam W_BADR  = 3'd1;
    localparam W_BWAIT = 3'd2;
    localparam W_BCAP  = 3'd3;
    localparam W_ACK   = 3'd4;
    localparam W_DEC   = 3'd5;

    localparam PID_SOF = 4'b0101;

    integer i;

    reg [31:0] ctrl;
    reg [31:0] irqen;
    reg [5:0] irqstat;

    reg [31:0] xact_a;
    reg [31:0] xact_b;

    reg [10:0] frame;
    reg [19:0] frame_div;
    reg [PORTS-1:0] sof_pending;

    reg [2:0] ws;
    reg [1:0] bcnt;
    reg [10:0] wb_badr;
    reg wb_bwe;
    reg [7:0] wb_bwdat;
    reg [7:0] wb_brdat;
    reg [31:0] wb_bacc;

    reg sw_req;
    reg sched_start;
    reg sched_is_sof;
    // Latched at schedule time: is the transaction now going out an
    // IN token? rtl/probe.v triggers on this so a capture lands on the
    // transaction whose REPLY we want to see, rather than on the
    // SETUP, whose reply is a 16-bit ACK that already decodes fine.
    reg tx_is_in;
    reg [1:0] sched_pid;
    reg [6:0] sched_addr;
    reg [3:0] sched_endp;
    reg sched_ls;
    reg sched_inv;
    reg sched_pre;
    reg sched_port;
    reg sched_toggle;
    reg sched_autocont;
    reg [6:0] sched_mps;
    reg [10:0] sched_off;
    reg [10:0] sched_len;
    reg [3:0] sched_nak;

    reg [3:0] res_status;
    reg [10:0] res_len;
    reg res_tgl;
    reg [7:0] res_naks;

    // -- no_rw_check is what makes this ONE block RAM --
    //
    // Two synchronous read/write ports over one array is the shape a
    // DP16KD has, but yosys still refuses it without this attribute,
    // and silently: it reports no error, maps the whole 2 KB into
    // flip-flops and LUTs, and the design goes from ~2000 LUT4 to over
    // 56000. On a 12F that is not a regression, it is the end of the
    // project, so it is worth knowing why.
    //
    // Verilog array semantics DEFINE what port B reads when port A
    // writes the same address in the same cycle -- both see the old
    // value, because the assignments are non-blocking. The hardware
    // does not define it at all. Rather than emit a block RAM that
    // might disagree with the simulation, yosys declines to use one.
    //
    // The attribute says the design does not depend on that case, and
    // here it does not -- but only because of a contract with
    // software, so the contract is written down: SOFTWARE MUST NOT
    // TOUCH THE PACKET BUFFER WHILE A TRANSACTION IS RUNNING. Poll
    // XACT_S until the pending bit clears first. That was already the
    // only sensible way to use this block; it is now load-bearing.
    //
    // Within each port the case cannot arise anyway: the engine reads
    // while transmitting and writes while receiving, never both, and
    // the Wishbone walk below does one or the other per access.
    reg [31:0] poll_a [0:3];
    reg [31:0] poll_b [0:3];
    reg [7:0] poll_ctr [0:3];
    reg [3:0] poll_pending;
    reg [1:0] poll_slot;
    reg sched_is_poll;

    reg [15:0] dbg_tx;        // packets this host has transmitted
    reg [15:0] dbg_rx;        // packets that STARTED arriving
    reg [7:0] dbg_rx_ok;      // ...and ended with a good CRC and PID
    reg [7:0] dbg_rx_bad;     // ...and did not
    reg sie_rx_active_q;

    reg [7:0] cap0, cap1, cap2, cap3, cap4, cap5, cap6, cap7;
    reg [3:0] cap_idx;

    reg [1:0] hid_in_mode;
    reg hid0_valid;
    reg hid1_valid;

    reg [1:0] typ_wval;
    reg typ0_we;
    reg typ1_we;

    wire [31:0] hid0_info, hid0_keys, hid0_mouse, hid0_cursor, hid0_pad;
    wire [31:0] hid1_info, hid1_keys, hid1_mouse, hid1_cursor, hid1_pad;

    (* no_rw_check *)
    reg [7:0] pbuf [0:2047];
    reg [7:0] xb_rdat;

    wire [PORTS-1:0] p_drive_se0;
    wire [PORTS-1:0] p_connected;
    wire [PORTS-1:0] p_enabled;
    wire [PORTS-1:0] p_lowspeed;
    wire [PORTS-1:0] p_resetting;
    wire [PORTS-1:0] p_change;
    wire [PORTS-1:0] p_ka;
    wire [PORTS-1:0] p_change_ack;

    // Pin drive, split into a plain enable and a plain value so the
    // tristate below can be written in the ONE form yosys recognises.
    // See the assign statements for why that matters.
    wire [PORTS-1:0] pin_oe;
    wire [PORTS-1:0] pin_dp;
    wire [PORTS-1:0] pin_dm;

    // Raw line state for PORTSTAT bits [7:6] of each port byte,
    // synchronised because it crosses from a pad into the bus read
    // path. Costs four flops per port and answers the one question
    // that is otherwise unanswerable from software: the reported
    // SPEED is derived from which line the device pulls up, and when
    // that derivation is wrong every failure downstream presents as a
    // protocol error instead of as a wiring problem.
    reg [PORTS-1:0] dp_s0, dp_s1, dm_s0, dm_s1;

    // A packet "starts" on the rising edge of rx_active -- the SIE
    // saw the bus leave idle for K while it was listening. That is
    // deliberately the earliest possible point: it separates "the
    // device answered and we mangled it" from "the device never
    // answered", and those have nothing in common.
    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            dbg_tx <= 16'd0;
            dbg_rx <= 16'd0;
            dbg_rx_ok <= 8'd0;
            dbg_rx_bad <= 8'd0;
            sie_rx_active_q <= 1'b0;
        end else begin
            sie_rx_active_q <= sie_rx_active;
            if (sie_tx_done) dbg_tx <= dbg_tx + 16'd1;
            if (sie_rx_active && !sie_rx_active_q) dbg_rx <= dbg_rx + 16'd1;
            if (sie_rx_done) begin
                if (sie_rx_crc_ok && !sie_rx_pid_err && !sie_rx_err)
                    dbg_rx_ok <= dbg_rx_ok + 8'd1;
                else
                    dbg_rx_bad <= dbg_rx_bad + 8'd1;
            end
        end
    end

    always @(posedge wb_clk_i) begin
        dp_s0 <= usb_dp;
        dp_s1 <= dp_s0;
        dm_s0 <= usb_dm;
        dm_s1 <= dm_s0;
    end

    // A write of 1 to an IRQSTAT change bit acknowledges the port that
    // raised it. irqstat bit 1 is port 0, bit 2 is port 1.
    wire irq_w1c = wb_wr_stb && (adr_q == A_IRQSTAT);

    wire sie_dp_o, sie_dm_o, sie_oe;
    wire [10:0] sie_tx_idx;
    wire [7:0] sie_tx_byte;
    wire sie_tx_done, sie_tx_active;
    wire sie_rx_active, sie_rx_pid_err, sie_rx_byte_valid;
    wire sie_rx_done, sie_rx_crc_ok, sie_rx_err;
    wire [3:0] sie_rx_pid;
    wire [7:0] sie_rx_byte;
    wire [1:0] sie_line_state;
    wire sie_se0;

    wire x_tx_go, x_rx_en;
    wire [3:0] x_tx_pid;
    wire [10:0] x_tx_len;
    wire [1:0] x_tx_crc_mode;
    wire x_cfg_ls, x_cfg_inv, x_cfg_pre;
    wire [10:0] xb_adr;
    wire xb_we;
    wire [7:0] xb_wdat;
    wire x_busy, x_done;
    wire [3:0] x_status;
    wire [10:0] x_act_len;
    wire x_res_toggle;
    wire [7:0] x_naks;
    wire x_port_sel;

    // -- busy has to cover the launch latency, not just the engine --
    //
    // Writing the start bit sets sw_req; the scheduler picks it up a
    // cycle later as sched_start; the engine raises its own busy a
    // cycle after that. Reporting only the engine's busy leaves a
    // three-cycle window in which a transaction has been accepted and
    // nothing says so -- and the natural driver loop is "write start,
    // then poll until not busy", which lands squarely in it, reads the
    // PREVIOUS transaction's status as if it were this one's, and
    // fires the next request on top of one already running.
    //
    // The three terms are contiguous by construction, so a request is
    // outstanding from the write to the last cycle of the engine.
    wire xact_pending = x_busy || sw_req || sched_start;
    wire ctl_frame_en = ctrl[1];

    // -- CTRL fields --
    // -- XACT_A fields, per docs/usb_host.md --
    wire [6:0] xa_addr = xact_a[6:0];
    wire [3:0] xa_endp = xact_a[10:7];
    wire [1:0] xa_pid = xact_a[12:11];
    wire xa_ls = xact_a[13];
    wire xa_inv = xact_a[14];
    wire xa_pre = xact_a[15];
    wire xa_port = xact_a[16];
    wire xa_toggle = xact_a[17];
    wire xa_autocont = xact_a[18];
    wire [6:0] xa_mps = xact_a[25:19];

    wire [10:0] xb_off = xact_b[10:0];
    wire [10:0] xb_len = xact_b[21:11];
    wire [3:0] xb_nak = xact_b[25:22];

    // -- the bus inputs are REGISTERED before anything decodes them --
    //
    // wb_adr_i used to be decoded combinationally in sixteen places
    // across six always blocks, so the top-level wbm_adr net -- which
    // the arbiter's address mux drives, and which every slave on the
    // bus already loads -- picked up that many more sinks when this
    // block was added.
    //
    // That cost real frequency: the whole SoC went from 52.5 MHz with
    // `USB_HID to 45.3 MHz with `USB_HOST, below the 48 MHz it runs
    // at. The failing path was 17.8 ns of routing against 4.2 ns of
    // logic -- congestion, not depth -- so the fix is to stop being a
    // big distributed load, not to shorten a chain.
    //
    // Latching once here presents eleven flop inputs instead, and the
    // decode below works from the copy.
    //
    // It costs one wait state on a register access. Nothing notices:
    // the driver polls, and a buffer access already takes six cycles
    // for its four-byte walk.
    //
    // wb_is_buf stays on the LIVE address because W_IDLE has to
    // choose a path in the cycle the strobe arrives, before the latch
    // has happened.
    wire wb_is_buf = (wb_adr_i[10:9] == 2'b10);

    reg [10:0] adr_q;
    reg [31:0] dat_q;
    reg [3:0] sel_q;
    reg we_q;

    wire wb_is_poll = (adr_q[10:3] == 8'h10);
    // The HID compat window: block 0 at words 0x00-0x04, block 1 at
    // 0x08-0x0c. rtl/sysctl.v's existing decode splits the two on
    // address bit 5, which is word-address bit 3.
    wire wb_is_hid = (adr_q[10:4] == 7'd0);
    wire [2:0] hid_reg = adr_q[2:0];
    wire hid_blk = adr_q[3];
    wire wb_sel_cyc = wb_cyc_i && wb_stb_i;

    // THE cycle on which a register write takes effect.
    //
    // Everything that acts on a write -- the poll table, the compat
    // blocks, the XACT_B start bit, the IRQSTAT write-1-to-clear --
    // must key off this and nothing else. They used to test
    // "wb_sel_cyc && !wb_ack_o", which was exactly one cycle while
    // the decode was combinational. Registering the address made the
    // ack land a cycle later, so that condition became true TWICE and
    // every one of those writes happened twice. Co-simulation caught
    // it instantly as devices enumerating at address 4 and 5.
    wire wb_wr_stb = (ws == W_DEC) && we_q;

    assign int_o = |(irqstat & irqen[5:0]);

    // ---------------------------------------------------------------
    // packet buffer -- true dual port, byte wide
    // ---------------------------------------------------------------
    //
    // Two single-port-shaped always blocks over one array is the
    // pattern yosys turns into a DP16KD. Writing it any other way
    // tends to produce distributed RAM, which on this design would be
    // 2048 bytes of LUTs and is not survivable.

    always @(posedge wb_clk_i) begin
        if (xb_we) pbuf[xb_adr] <= xb_wdat;
        xb_rdat <= pbuf[xb_adr];
    end

    always @(posedge wb_clk_i) begin
        if (wb_bwe) pbuf[wb_badr] <= wb_bwdat;
        wb_brdat <= pbuf[wb_badr];
    end

    // ---------------------------------------------------------------
    // ports
    // ---------------------------------------------------------------

    genvar gi;
    generate
        for (gi = 0; gi < PORTS; gi = gi + 1) begin : ports

            usb_port #(
                .T_MS(T_MS),
                .DEBOUNCE_MS(DEBOUNCE_MS),
                .RESET_MS(RESET_MS),
                .RECOVERY_MS(RECOVERY_MS)
            ) port_i (
                .clk(wb_clk_i),
                .rst(wb_rst_i),
                .dp_i(usb_dp[gi]),
                .dm_i(usb_dm[gi]),
                .drive_se0(p_drive_se0[gi]),
                .ctl_enable(ctrl[8 + gi*8]),
                .ctl_reset(ctrl[9 + gi*8]),
                .ctl_suspend(ctrl[10 + gi*8]),
                .ctl_keepalive(p_ka[gi]),
                .st_connected(p_connected[gi]),
                .st_enabled(p_enabled[gi]),
                .st_lowspeed(p_lowspeed[gi]),
                .st_resetting(p_resetting[gi]),
                .st_change(p_change[gi]),
                // Clearing the interrupt bit clears the port's own
                // change flag with it. Without this the flag stays
                // asserted, re-sets irqstat the cycle after the
                // handler clears it, and IRQ 9 -- which is a LEVEL --
                // storms. Two halves of one acknowledgement.
                .change_ack(p_change_ack[gi])
            );

            // Pin mux. A port driving a bus reset outranks the SIE --
            // it is a 10 ms assertion and nothing else may talk over
            // it. Otherwise the SIE drives whichever single port the
            // current transaction named, and every other port is
            // released, which is the correct idle: the DEVICE's 1.5k
            // pull-up holds the line, not us.
            assign pin_oe[gi] = p_drive_se0[gi] ||
                (sie_oe && (x_port_sel == (gi != 0)));
            assign pin_dp[gi] = p_drive_se0[gi] ? 1'b0 : sie_dp_o;
            assign pin_dm[gi] = p_drive_se0[gi] ? 1'b0 : sie_dm_o;

            // -- this MUST stay a flat "enable ? value : 1'bz" --
            //
            // It used to be a nested ternary that put the 1'bz in the
            // innermost else. That is correct Verilog and simulates
            // perfectly, and yosys infers NO TRISTATE FROM IT AT ALL:
            // it recognises the simple form and quietly resolves
            // anything else, so both pins came out permanently driven.
            //
            // On hardware that is worse than it sounds. The port sat
            // at a constant J, which made every port report a device
            // attached whether or not one was, and drove the bus hard
            // enough that a real device's reply never reached the
            // receiver -- so every transfer timed out and it looked
            // like two dead devices rather than one bad assign.
            //
            // rtl/ext/usb_hid_host uses exactly this flat form on the
            // same pins, which is why that core works here.
            //
            // Check it, do not assume it:
            //   yosys -p "read_verilog rtl/usb/*.v; hierarchy -top \
            //     usb_host; proc; opt_clean; select -assert-count \
            //     4 t:\$tribuf"
            assign usb_dp[gi] = pin_oe[gi] ? pin_dp[gi] : 1'bz;
            assign usb_dm[gi] = pin_oe[gi] ? pin_dm[gi] : 1'bz;

        end
    endgenerate

    // The SIE sees whichever port the transaction engine selected.
    wire sie_dp_i = usb_dp[x_port_sel];
    wire sie_dm_i = usb_dm[x_port_sel];

    // ---------------------------------------------------------------
    // SIE and transaction engine
    // ---------------------------------------------------------------

    usb_sie #(
        .FS_DIV(FS_DIV),
        .LS_DIV(LS_DIV)
    ) sie_i (
        .clk(wb_clk_i),
        .rst(wb_rst_i),
        .dp_i(sie_dp_i),
        .dm_i(sie_dm_i),
        .dp_o(sie_dp_o),
        .dm_o(sie_dm_o),
        .oe_o(sie_oe),
        .cfg_ls(x_cfg_ls),
        .cfg_inv(x_cfg_inv),
        .cfg_pre(x_cfg_pre),
        .tx_go(x_tx_go),
        .tx_pid(x_tx_pid),
        .tx_len(x_tx_len),
        .tx_crc_mode(x_tx_crc_mode),
        .tx_idx(sie_tx_idx),
        .tx_byte(sie_tx_byte),
        .tx_done(sie_tx_done),
        .tx_active(sie_tx_active),
        .rx_en(x_rx_en),
        .rx_active(sie_rx_active),
        .rx_pid(sie_rx_pid),
        .rx_pid_err(sie_rx_pid_err),
        .rx_byte(sie_rx_byte),
        .rx_byte_valid(sie_rx_byte_valid),
        .rx_done(sie_rx_done),
        .rx_crc_ok(sie_rx_crc_ok),
        .rx_err(sie_rx_err),
        .line_state(sie_line_state),
        .se0(sie_se0)
    );

    usb_xact xact_i (
        .clk(wb_clk_i),
        .rst(wb_rst_i),
        .sie_tx_go(x_tx_go),
        .sie_tx_pid(x_tx_pid),
        .sie_tx_len(x_tx_len),
        .sie_tx_crc_mode(x_tx_crc_mode),
        .sie_tx_idx(sie_tx_idx),
        .sie_tx_byte(sie_tx_byte),
        .sie_tx_done(sie_tx_done),
        .sie_tx_active(sie_tx_active),
        .sie_rx_en(x_rx_en),
        .sie_rx_active(sie_rx_active),
        .sie_rx_pid(sie_rx_pid),
        .sie_rx_pid_err(sie_rx_pid_err),
        .sie_rx_byte(sie_rx_byte),
        .sie_rx_byte_valid(sie_rx_byte_valid),
        .sie_rx_done(sie_rx_done),
        .sie_rx_crc_ok(sie_rx_crc_ok),
        .sie_rx_err(sie_rx_err),
        .sie_se0(sie_se0),
        .port_gone(!p_connected[x_port_sel]),
        .sie_cfg_ls(x_cfg_ls),
        .sie_cfg_inv(x_cfg_inv),
        .sie_cfg_pre(x_cfg_pre),
        .buf_adr(xb_adr),
        .buf_we(xb_we),
        .buf_wdat(xb_wdat),
        .buf_rdat(xb_rdat),
        .req_start(sched_start),
        .req_pid(sched_pid),
        .req_addr(sched_addr),
        .req_endp(sched_endp),
        .req_ls(sched_ls),
        .req_inv(sched_inv),
        .req_pre(sched_pre),
        .req_port(sched_port),
        .req_toggle(sched_toggle),
        .req_autocont(sched_autocont),
        .req_mps(sched_mps),
        .req_off(sched_off),
        .req_len(sched_len),
        .req_nak_retry(sched_nak),
        .busy(x_busy),
        .done(x_done),
        .status(x_status),
        .act_len(x_act_len),
        .res_toggle(x_res_toggle),
        .nak_count(x_naks),
        .port_sel(x_port_sel)
    );

    // ---------------------------------------------------------------
    // HID compatibility blocks
    // ---------------------------------------------------------------
    //
    // Two instances, matching the two usb_hid_wb instances they
    // replace. Which PHYSICAL port a report came from is not what
    // picks the block -- the auto-poll slot names its target. That
    // matters the moment a hub is involved, because a mouse and a
    // keyboard can both be behind port 0 and software still expects
    // them at reg_usb0_* and reg_usb1_*.

    usb_hid_compat #(.SENS_SHIFT(SENS_SHIFT)) hid0_i (
        .clk(wb_clk_i), .rst(wb_rst_i),
        .in_valid(hid0_valid), .in_mode(hid_in_mode),
        .in_b0(cap0), .in_b1(cap1), .in_b2(cap2), .in_b3(cap3),
        .in_b4(cap4), .in_b5(cap5), .in_b6(cap6), .in_b7(cap7),
        .typ_we(typ0_we), .typ_i(typ_wval),
        .reg_info(hid0_info), .reg_keys(hid0_keys),
        .reg_mouse(hid0_mouse), .reg_cursor(hid0_cursor),
        .reg_pad(hid0_pad),
        .curs_x(curs_x0), .curs_y(curs_y0), .typ(typ0),
        .int_o(hid0_int_o)
    );

    usb_hid_compat #(.SENS_SHIFT(SENS_SHIFT)) hid1_i (
        .clk(wb_clk_i), .rst(wb_rst_i),
        .in_valid(hid1_valid), .in_mode(hid_in_mode),
        .in_b0(cap0), .in_b1(cap1), .in_b2(cap2), .in_b3(cap3),
        .in_b4(cap4), .in_b5(cap5), .in_b6(cap6), .in_b7(cap7),
        .typ_we(typ1_we), .typ_i(typ_wval),
        .reg_info(hid1_info), .reg_keys(hid1_keys),
        .reg_mouse(hid1_mouse), .reg_cursor(hid1_cursor),
        .reg_pad(hid1_pad),
        .curs_x(curs_x1), .curs_y(curs_y1), .typ(typ1),
        .int_o(hid1_int_o)
    );

    // ---------------------------------------------------------------
    // frame timer and scheduler
    // ---------------------------------------------------------------
    //
    // A full-speed device that stops seeing SOF decides the bus is
    // suspended, so this is not optional decoration. Low-speed ports
    // get a keepalive EOP instead, which is what a low-speed segment
    // uses in place of a frame marker -- a low-speed device would not
    // understand a SOF token and is not supposed to see one.
    //
    // SOF outranks a software transaction when both are waiting. A
    // frame marker that slips is a protocol error; a control transfer
    // that waits 50 us is not.

    // -- the keepalive must wait for the bus, exactly as SOF does --
    //
    // A low-speed keepalive is an EOP driven by usb_port.v, and the
    // pin mux gives a port's own drive priority over the SIE. Derive
    // it from the frame timer alone and it lands in the MIDDLE of
    // whatever packet the SIE is sending -- the host's own data
    // packet, truncated by two bit times of SE0 it did not ask for.
    //
    // Invisible with one device attached, because the only traffic on
    // a low-speed port is transactions to that port and the frame
    // boundary rarely fell inside one. It appears the moment a second
    // device is present: a control transfer that takes hundreds of
    // microseconds at 1.5 Mbps is wide enough for a 1 ms frame
    // boundary to land inside it fairly often, and the symptom is a
    // device that enumerates and then fails partway.
    //
    // The SOF path already had this right -- the scheduler only issues
    // one when the engine is idle. This is the same rule.
    // Enabled OR still in reset recovery. A port that has just been
    // reset stays idle for 10 ms before it reports enabled, which is
    // longer than the 3 ms a low-speed device waits before suspending.
    wire [PORTS-1:0] p_ka_ok = p_enabled | p_resetting;

    // Same rule for full speed, where the keepalive's job is done by
    // an actual SOF packet.
    //
    // The SOF branches below were gated on p_enabled, which is not set
    // until reset recovery ENDS -- so a full-speed device got 10 ms of
    // idle bus immediately after its reset, suspended, and ignored the
    // first SETUP. Identical mechanism to the low-speed keepalive bug,
    // and identical symptom: no response at all while the lines read a
    // perfectly healthy J.
    //
    // Excludes the reset phase itself, where the port drives SE0 and a
    // SOF would just be stamped out.
    wire [PORTS-1:0] p_sof_ok =
        p_enabled | (p_resetting & ~p_drive_se0);

    assign p_ka = sof_pending & p_lowspeed & p_ka_ok &
                  {PORTS{~x_busy}};

    wire frame_tick = ctl_frame_en && (frame_div >= (T_FRAME - 32'd1));

    // Lowest pending slot first. A fixed priority is fine here: a slot
    // that misses its frame is serviced in the next one, and a boot
    // device's interval is 8 to 10 frames, so a one-frame slip is
    // invisible. Round-robin would cost a counter to solve a problem
    // this design does not have.
    wire [1:0] poll_next =
        poll_pending[0] ? 2'd0 :
        poll_pending[1] ? 2'd1 :
        poll_pending[2] ? 2'd2 : 2'd3;

    wire [2:0] poll_mode = poll_b[poll_slot][13:11];

    generate
        for (gi = 0; gi < PORTS; gi = gi + 1) begin : chack
            assign p_change_ack[gi] = irq_w1c && dat_q[1 + gi];
        end
    endgenerate

    always @(posedge wb_clk_i) begin

        sched_start <= 1'b0;
        hid0_valid <= 1'b0;
        hid1_valid <= 1'b0;
        typ0_we <= 1'b0;
        typ1_we <= 1'b0;

        if (wb_rst_i) begin

            frame <= 11'd0;
            frame_div <= 20'd0;
            sof_pending <= {PORTS{1'b0}};
            sw_req <= 1'b0;
            poll_pending <= 4'd0;
            sched_is_poll <= 1'b0;
            sched_is_sof <= 1'b0;
            tx_is_in <= 1'b0;
            cap_idx <= 4'd0;
            // The result registers MUST come out of reset defined.
            // Software reads XACT_S before its first transaction --
            // "poll until not pending" does exactly that -- and
            // without this it reads whatever these flops powered up
            // holding, then acts on it. In simulation that is X and
            // the pending bit reads back as a zero that is not a zero;
            // on silicon it is a stable random value, which is worse,
            // because it will look like it works.
            res_status <= 4'd0;
            res_len <= 11'd0;
            res_tgl <= 1'b0;
            res_naks <= 8'd0;
            irqstat <= 6'd0;
            for (i = 0; i < 4; i = i + 1) begin
                poll_a[i] <= 32'd0;
                poll_b[i] <= 32'd0;
                poll_ctr[i] <= 8'd0;
            end

        end else begin

            if (ctl_frame_en) begin
                if (frame_tick) begin
                    frame_div <= 20'd0;
                    frame <= frame + 11'd1;
                    sof_pending <= p_ka_ok;
                end else begin
                    frame_div <= frame_div + 20'd1;
                end
            end

            // A keepalive needs no transaction, so those ports clear
            // themselves the moment the pulse is taken.
            //
            // Per bit, NOT as a whole-vector assignment: the frame
            // boundary above sets the whole vector, and a second
            // vector assignment here would win outright and no port
            // would ever see a SOF. Assigning single bits lets the
            // later statement override only what it names, which is
            // the intent.
            for (i = 0; i < PORTS; i = i + 1)
                if (p_ka[i]) sof_pending[i] <= 1'b0;

            if (!x_busy && !sched_start) begin

                if (sof_pending[0] && !p_lowspeed[0] && p_sof_ok[0]) begin

                    sof_pending[0] <= 1'b0;
                    sched_is_sof <= 1'b1;
                    sched_is_poll <= 1'b0;
                    sched_pid <= 2'd3;
                    sched_addr <= frame[6:0];
                    sched_endp <= frame[10:7];
                    sched_ls <= 1'b0;
                    sched_inv <= 1'b0;
                    sched_pre <= 1'b0;
                    sched_port <= 1'b0;
                    sched_len <= 11'd0;
                    sched_autocont <= 1'b0;
                    sched_start <= 1'b1;

                end else if ((PORTS > 1) && sof_pending[PORTS-1] &&
                             !p_lowspeed[PORTS-1] && p_sof_ok[PORTS-1]) begin

                    sof_pending[PORTS-1] <= 1'b0;
                    sched_is_sof <= 1'b1;
                    sched_is_poll <= 1'b0;
                    sched_pid <= 2'd3;
                    sched_addr <= frame[6:0];
                    sched_endp <= frame[10:7];
                    sched_ls <= 1'b0;
                    sched_inv <= 1'b0;
                    sched_pre <= 1'b0;
                    sched_port <= 1'b1;
                    sched_len <= 11'd0;
                    sched_autocont <= 1'b0;
                    sched_start <= 1'b1;

                end else if (sw_req) begin

                    sw_req <= 1'b0;
                    sched_is_sof <= 1'b0;
                    sched_is_poll <= 1'b0;
                    sched_pid <= xa_pid;
                    tx_is_in <= (xa_pid == 2'd1);
                    sched_addr <= xa_addr;
                    sched_endp <= xa_endp;
                    sched_ls <= xa_ls;
                    sched_inv <= xa_inv;
                    sched_pre <= xa_pre;
                    sched_port <= xa_port;
                    sched_toggle <= xa_toggle;
                    sched_autocont <= xa_autocont;
                    sched_mps <= xa_mps;
                    sched_off <= xb_off;
                    sched_len <= xb_len;
                    sched_nak <= xb_nak;
                    sched_start <= 1'b1;

                end else if (ctrl[2] && (poll_pending != 4'd0)) begin

                    // An auto-poll slot is due. Software set this up
                    // once, at enumeration, and is not involved again
                    // -- which is the entire point. A report becomes a
                    // cursor position with no interrupt, no ISR entry
                    // and no exposure to whatever else is holding the
                    // CPU. See docs/usb_host.md on why the cursor is
                    // the one thing that stays in gateware.
                    // Both flags are set on EVERY scheduler branch,
                    // not just the one they belong to. They are what
                    // decides where a completed transaction's result
                    // goes, and a stale one sends it somewhere it does
                    // not belong: leaving sched_is_poll set through a
                    // SOF meant the SOF completed with status OK and
                    // was delivered to the compat block as a mouse
                    // report built from whatever was last captured.
                    poll_slot <= poll_next;
                    poll_pending[poll_next] <= 1'b0;
                    sched_is_sof <= 1'b0;
                    sched_is_poll <= 1'b1;
                    sched_pid <= 2'd1;
                    sched_addr <= poll_a[poll_next][6:0];
                    sched_endp <= poll_a[poll_next][10:7];
                    sched_ls <= poll_a[poll_next][11];
                    sched_inv <= poll_a[poll_next][12];
                    sched_pre <= poll_a[poll_next][13];
                    sched_port <= poll_a[poll_next][14];
                    sched_mps <= poll_a[poll_next][21:15];
                    sched_toggle <= poll_a[poll_next][31];
                    sched_autocont <= 1'b0;
                    sched_off <= poll_b[poll_next][10:0];
                    sched_len <= {4'd0, poll_a[poll_next][21:15]};
                    // A poll must never stall the scheduler waiting on
                    // a device that has nothing to say. An idle mouse
                    // NAKs every single poll, and that is normal.
                    sched_nak <= 4'd0;
                    cap_idx <= 4'd0;
                    sched_start <= 1'b1;

                end

            end

            // Snoop the payload on its way past rather than reading it
            // back out of the packet buffer afterwards. A boot report
            // is at most eight bytes and the alternative is a second
            // pass over a memory the CPU may want.
            if (sched_is_poll && x_busy && sie_rx_byte_valid) begin
                case (cap_idx)
                4'd0: cap0 <= sie_rx_byte;
                4'd1: cap1 <= sie_rx_byte;
                4'd2: cap2 <= sie_rx_byte;
                4'd3: cap3 <= sie_rx_byte;
                4'd4: cap4 <= sie_rx_byte;
                4'd5: cap5 <= sie_rx_byte;
                4'd6: cap6 <= sie_rx_byte;
                4'd7: cap7 <= sie_rx_byte;
                default: ;
                endcase
                if (cap_idx != 4'd8) cap_idx <= cap_idx + 4'd1;
            end

            if (x_done && sched_is_poll) begin
                poll_a[poll_slot][31] <= x_res_toggle;
                poll_b[poll_slot][19:17] <= x_status[2:0];
                if (x_status == 4'd0 || x_status == 4'd6) begin
                    // A real report. A NAK is not one -- it is the
                    // device saying nothing changed -- and must not
                    // reach the compat block, or an idle mouse would
                    // generate an interrupt a hundred times a second
                    // and a keyboard would re-deliver its last report
                    // forever.
                    if (poll_mode == 3'd1) begin
                        poll_b[poll_slot][16] <= 1'b1;
                        irqstat[3] <= 1'b1;
                    end else if (poll_mode != 3'd0) begin
                        hid_in_mode <= poll_mode[1:0];
                        if (poll_b[poll_slot][15:14] == 2'd0)
                            hid0_valid <= 1'b1;
                        else
                            hid1_valid <= 1'b1;
                    end
                end else if (x_status != 4'd1) begin
                    irqstat[4] <= 1'b1;
                end
            end

            // Frame-driven interval counters. bInterval is in frames,
            // so this is where a poll becomes due.
            if (ctl_frame_en && frame_tick) begin
                for (i = 0; i < 4; i = i + 1) begin
                    if (poll_a[i][30]) begin
                        if (poll_ctr[i] >= poll_a[i][29:22]) begin
                            poll_ctr[i] <= 8'd0;
                            poll_pending[i] <= 1'b1;
                        end else begin
                            poll_ctr[i] <= poll_ctr[i] + 8'd1;
                        end
                    end
                end
            end

            if (wb_wr_stb && wb_is_poll) begin
                if (adr_q[0]) poll_b[adr_q[2:1]] <= dat_q;
                else poll_a[adr_q[2:1]] <= dat_q;
            end

            if (wb_wr_stb && wb_is_hid && (hid_reg == 3'd0)) begin
                // The kernel assigns the device type; nothing infers
                // it any more. Writing the info register is how, and
                // it is the only bit of that register that is
                // writable -- see docs/usb_host.md.
                typ_wval <= dat_q[25:24];
                typ0_we <= !hid_blk;
                typ1_we <= hid_blk;
            end

            // A completed SOF is not a result software asked for and
            // must not overwrite the status of the transaction it is
            // waiting on.
            if (x_done && !sched_is_sof) begin
                res_status <= x_status;
                res_len <= x_act_len;
                res_tgl <= x_res_toggle;
                res_naks <= x_naks;
                irqstat[0] <= 1'b1;
            end

            if (p_change[0]) irqstat[1] <= 1'b1;
            if ((PORTS > 1) && p_change[PORTS-1]) irqstat[2] <= 1'b1;

            // -- and !wb_ack_o is load-bearing --
            //
            // wb_cyc/wb_stb stay asserted for the whole bus cycle, so
            // without this the start bit is detected on two
            // consecutive clocks. The scheduler clears sw_req on the
            // second of them and this statement, being later in the
            // same block, sets it straight back -- so every
            // transaction software asked for ran exactly twice, and
            // the second one overwrote the first one's result just as
            // the driver went to read it.
            if (wb_wr_stb && (adr_q == A_XACT_B) && dat_q[31])
                sw_req <= 1'b1;

            if (wb_wr_stb && (adr_q == A_IRQSTAT))
                irqstat <= irqstat & ~dat_q[5:0];

        end

    end

    // ---------------------------------------------------------------
    // Wishbone
    // ---------------------------------------------------------------

    always @(posedge wb_clk_i) begin

        wb_ack_o <= 1'b0;
        wb_bwe <= 1'b0;

        if (wb_rst_i) begin

            ws <= W_IDLE;
            ctrl <= 32'd0;
            irqen <= 32'd0;

        end else begin

            // -- the port reset bits are SELF-CLEARING --
            //
            // docs/usb_host.md specified them that way and this did
            // not implement it, which co-simulation found in its first
            // run. The driver's natural sequence is "assert reset,
            // then wait for the port to report enabled" -- and
            // usb_port.v re-enters reset from P_ENABLED whenever
            // ctl_reset is high, so the port cycled between reset and
            // recovery forever and the driver waited for an enabled
            // that could never arrive.
            //
            // Neither side is wrong on its own, which is why neither
            // side's own tests caught it: the RTL's testbench pulsed
            // the bit by hand, and the driver was never run against
            // anything.
            //
            // Clearing here, before the write case below, means a
            // write that sets the bit wins for exactly one cycle --
            // long enough for usb_port.v to sample it once, which is
            // all a reset request needs.
            if (ctrl[9]) ctrl[9] <= 1'b0;
            if (ctrl[17]) ctrl[17] <= 1'b0;

            case (ws)

            W_IDLE: begin
                if (wb_sel_cyc && !wb_ack_o) begin

                    // Latch everything the decode needs, once.
                    adr_q <= wb_adr_i;
                    dat_q <= wb_dat_i;
                    sel_q <= wb_sel_i;
                    we_q <= wb_we_i;

                    if (wb_is_buf) begin

                        bcnt <= 2'd0;
                        wb_bacc <= 32'd0;
                        ws <= W_BADR;

                    end else begin

                        ws <= W_DEC;

                    end

                end
            end

            // Register access, one cycle after the strobe, working
            // entirely from the latched copies.
            W_DEC: begin
                begin
                    begin

                        if (we_q) begin
                            case (adr_q)
                            A_CTRL:  ctrl <= dat_q;
                            A_IRQEN: irqen <= dat_q;
                            A_XACT_A: xact_a <= dat_q;
                            A_XACT_B: xact_b <= dat_q;
                            default: ;
                            endcase
                        end

                        if (wb_is_hid) begin
                            case (hid_reg)
                            3'd0: wb_dat_o <= hid_blk ? hid1_info : hid0_info;
                            3'd1: wb_dat_o <= hid_blk ? hid1_keys : hid0_keys;
                            3'd2: wb_dat_o <= hid_blk ? hid1_mouse : hid0_mouse;
                            3'd3: wb_dat_o <= hid_blk ? hid1_cursor : hid0_cursor;
                            3'd4: wb_dat_o <= hid_blk ? hid1_pad : hid0_pad;
                            default: wb_dat_o <= 32'd0;
                            endcase
                        end else if (wb_is_poll) begin
                            wb_dat_o <= adr_q[0] ? poll_b[adr_q[2:1]]
                                                 : poll_a[adr_q[2:1]];
                        end else
                        case (adr_q)
                        A_CTRL: wb_dat_o <= ctrl;
                        // Bit 5 of each byte is overcurrent, wired to
                        // zero: these ports are D+/D-, 22R and a 15k
                        // pull-down and nothing else, so there is no
                        // sense hardware to report from. The field is
                        // allocated anyway so a board that gains a
                        // load switch does not move everything else.
                        // See docs/usb_host.md's board notes.
                        A_PORTSTAT: wb_dat_o <= {
                            5'd0, frame,
                            dp_s1[PORTS-1], dm_s1[PORTS-1],
                            1'b0, p_change[PORTS-1],
                            p_resetting[PORTS-1], p_lowspeed[PORTS-1],
                            p_enabled[PORTS-1], p_connected[PORTS-1],
                            dp_s1[0], dm_s1[0],
                            1'b0, p_change[0],
                            p_resetting[0], p_lowspeed[0],
                            p_enabled[0], p_connected[0] };
                        A_IRQSTAT: wb_dat_o <= {26'd0, irqstat};
                        A_IRQEN: wb_dat_o <= irqen;
                        A_XACT_A: wb_dat_o <= xact_a;
                        A_XACT_B: wb_dat_o <= xact_b;
                        A_XACT_S: wb_dat_o <= {
                            7'd0, res_naks, xact_pending, res_tgl,
                            res_status, res_len };
                        A_DEBUG0: wb_dat_o <= {dbg_rx, dbg_tx};
                        A_DEBUG1: wb_dat_o <= {
                            16'd0, dbg_rx_bad, dbg_rx_ok };
                        A_CONFIG: wb_dat_o <= {
                            MAGIC, 4'd2, POLL_SLOTS[3:0],
                            PORTS[3:0], 8'd2 };
                        // Everything else in the block acks and reads
                        // zero. Not optional: an unacked address on
                        // this bus hangs picorv32_wb forever, so a
                        // stale pointer must land on a zero read
                        // rather than a dead machine. rtl/sysctl.v
                        // learned this the hard way on the icache
                        // window.
                        default: wb_dat_o <= 32'd0;
                        endcase

                        wb_ack_o <= 1'b1;
                        ws <= W_IDLE;

                    end

                end
            end

            // -- the four-byte buffer walk --

            W_BADR: begin
                wb_badr <= {adr_q[8:0], bcnt};
                if (we_q) begin
                    if (sel_q[bcnt]) begin
                        wb_bwe <= 1'b1;
                        wb_bwdat <= dat_q[{bcnt, 3'b000} +: 8];
                    end
                    ws <= W_BCAP;
                end else begin
                    ws <= W_BWAIT;
                end
            end

            W_BWAIT: ws <= W_BCAP;

            W_BCAP: begin
                if (!we_q)
                    wb_bacc[{bcnt, 3'b000} +: 8] <= wb_brdat;
                bcnt <= bcnt + 2'd1;
                if (bcnt == 2'd3) ws <= W_ACK;
                else ws <= W_BADR;
            end

            W_ACK: begin
                wb_dat_o <= wb_bacc;
                wb_ack_o <= 1'b1;
                ws <= W_IDLE;
            end

            default: ws <= W_IDLE;

            endcase

        end

    end

    // Deliberately NOT a plain sie_tx_active.
    //
    // Narrowed again to IN transactions only. Excluding SOFs was not
    // enough: a capture triggered on the SETUP shows the token, our
    // DATA0 and the device's ACK -- all of which decode perfectly --
    // and stops before the IN whose reply is the packet that actually
    // fails its CRC.
    //
    // At full speed a SOF goes out every frame, so "the transmitter
    // became active" is almost always a SOF -- and rtl/probe.v, armed
    // and triggering on the first edge it sees, captured one every
    // time: 34 bit times of token followed by the rest of the window
    // full of idle J. Excluding SOFs makes the trigger land on a real
    // transaction, which is the only thing worth a capture.
    assign tx_active_o = sie_tx_active & ~sched_is_sof & tx_is_in;
    assign tx_port_o = x_port_sel;

endmodule
