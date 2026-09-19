/*
 * Zeitlos SOC -- simulation only, not synthesised.
 *
 * Behavioural USB device, for rtl/tb/tb_usb_host.v.
 *
 * Deliberately shares NO code with rtl/usb/usb_sie.v. It re-derives
 * NRZI, bit stuffing and both CRCs from the specification
 * independently, because a model built from the same source as the
 * thing it tests agrees with it about its own mistakes. The CRC
 * functions here were checked against captured USB traffic (a token to
 * address 0 endpoint 0 is 00 10; GET_DESCRIPTOR(DEVICE) carries DD 94)
 * before either side was trusted.
 *
 * -- three wiring modes, which is the whole point --
 *
 *   MODE 0   full speed, direct. Pull-up on D+, idle J is D+ high,
 *            83.3 ns bit.
 *
 *   MODE 1   low speed, direct. Pull-up on D-, so the line polarity
 *            is INVERTED, 667 ns bit.
 *
 *   MODE 2   low speed behind a full-speed hub. Pull-up on D+ and
 *            NORMAL polarity -- the local segment is full-speed and
 *            the hub drives it -- but the bit rate is still 667 ns,
 *            and every host packet is preceded by a PRE sent at the
 *            FULL-speed rate.
 *
 * Mode 2 is the one worth having a model for. It is the combination
 * that looks wrong and is right, and a host that folds polarity into
 * speed passes modes 0 and 1 and fails only here. See docs/usb_host.md.
 *
 * This is NOT a hub model: it does not enumerate as a hub, has no
 * downstream ports and no port-status endpoint. It models what a
 * low-speed device looks like FROM THE HOST when a hub is in the way,
 * which is what exercises the host's preamble path. The hub class
 * model belongs with the hub driver in phase 4.
 */

`timescale 1ns/1ps

module tb_usb_device #(
    parameter MODE = 0,
    parameter [6:0] START_ADDR = 7'd0,
    parameter integer MPS0 = 8,
    // Nanoseconds a line takes to fall after the other has risen. 20
    // is about one host sample at 48 MHz, which is what hardware
    // shows. 0 disables it, so existing tests are unchanged.
    parameter integer SKEW_NS = 0,
    // Device clock error in parts per million, signed. The spec allows
    // +-2500 at full speed and +-15000 at low speed.
    parameter integer CLK_PPM = 0
) (
    inout wire dp,
    inout wire dm,
    input wire attach
);

    localparam integer INVERT = (MODE == 1) ? 1 : 0;
    localparam integer PRE = (MODE == 2) ? 1 : 0;

    localparam real FS_NS = 83.3333;
    localparam real LS_NS = 666.6667;
    // -- the device's clock is not the host's --
    //
    // USB 2.0 specifies +-0.25% at full speed and +-1.5% at low speed,
    // and cheap devices sit near the edge of that. The host's receiver
    // has to tolerate it: it aligns on SYNC and then samples on its
    // own 48 MHz grid, so any difference accumulates across the packet.
    //
    // This model ran at exactly the nominal rate, so the harness
    // measured a receiver against a transmitter that could never drift
    // -- and the receive margin was therefore never tested at all.
    // Sweep CLK_PPM to find where reception actually breaks.
    localparam real BIT_NOM = (MODE == 0) ? FS_NS : LS_NS;
    // TRANSMIT at the offset rate; RECEIVE at nominal.
    //
    // Deliberately asymmetric. The point of CLK_PPM is to measure the
    // HOST's tolerance to a device whose clock is off, and this model
    // samples incoming bits at a fixed rate with no resynchronisation
    // at all -- far less tolerant than the host's DPLL. Skewing both
    // directions measures the model, not the thing under test, and
    // reports a host limit that is really a testbench limit.
    localparam real BIT_NS = BIT_NOM * (1.0 + CLK_PPM / 1000000.0);
    localparam real BIT_RX = BIT_NOM;

    localparam [3:0] PID_OUT   = 4'b0001;
    localparam [3:0] PID_IN    = 4'b1001;
    localparam [3:0] PID_SOF   = 4'b0101;
    localparam [3:0] PID_SETUP = 4'b1101;
    localparam [3:0] PID_DATA0 = 4'b0011;
    localparam [3:0] PID_DATA1 = 4'b1011;
    localparam [3:0] PID_ACK   = 4'b0010;
    localparam [3:0] PID_NAK   = 4'b1010;
    localparam [3:0] PID_STALL = 4'b1110;
    localparam [3:0] PID_PRE   = 4'b1100;

    // -- line --

    reg drv_en;
    reg drv_j;
    reg drv_se0;

    wire [1:0] out_c = drv_se0 ? 2'b00 : (drv_j ? 2'b10 : 2'b01);
    wire [1:0] out_p_i = INVERT ? {out_c[0], out_c[1]} : out_c;

    // -- transition skew, which real devices have and this did not --
    //
    // D+ and D- do not change on the same edge on real silicon. The
    // host samples at 48 MHz, so a pair taking even 20 ns to settle
    // appears as ONE SAMPLE of an illegal bus state in the middle of
    // every J<->K transition.
    //
    // Captured on hardware inside a device's data packet:
    //
    //     KKKK X JJJJJJJ X KKK X JJJJJJJ X KKKK
    //
    // where X is SE1, and the run lengths are 7 and 3 instead of the
    // multiples of 4 a clean full-speed packet gives.
    //
    // This model drove both lines from one expression, so it could
    // never produce that, and co-simulation was blind to the entire
    // class of receive failure that stopped every full-speed device on
    // hardware. Three attempts at fixing the receiver were evaluated
    // against a test that could not reproduce the fault; two of them
    // regressed real hardware and had to be reverted.
    //
    // Asymmetric delay: a line rises immediately and falls SKEW_NS
    // late, so the overlap is both-high -- the SE1 hardware shows.
    wire [1:0] out_p;
    assign #(0, SKEW_NS) out_p = out_p_i;

    // Gated on attach as well as drv_en. The testbench hangs several
    // models off one port and enables one at a time, and a detached
    // model is still decoding what it sees -- without this gate it
    // would answer a token meant for its neighbour, because both
    // start life at address 0.
    assign dp = (drv_en && attach) ? out_p[1] : 1'bz;
    assign dm = (drv_en && attach) ? out_p[0] : 1'bz;

    // The device's 1.5k pull-up, at `pull` strength so it beats the
    // board's 15k pull-down (modelled `weak` in the testbench) and
    // loses to any real driver.
    assign (pull1, highz0) dp = (attach && !INVERT) ? 1'b1 : 1'b0;
    assign (pull1, highz0) dm = (attach && INVERT) ? 1'b1 : 1'b0;

    wire [1:0] in_raw = {dp, dm};
    wire [1:0] in_c = INVERT ? {in_raw[0], in_raw[1]} : in_raw;
    wire in_se0 = (in_c === 2'b00);
    wire in_j = (in_c === 2'b10);
    wire in_k = (in_c === 2'b01);

    // -- state --

    reg [7:0] rxb [0:79];
    integer rxn;
    reg [3:0] rxpid;
    reg rx_crc_ok;
    reg rx_timeout;

    reg [7:0] txb [0:79];

    reg [6:0] dev_addr;
    reg [6:0] pending_addr;
    reg [7:0] setup [0:7];
    reg in_toggle;
    integer in_ptr;
    integer in_len;
    reg [7:0] desc [0:17];
    // Configuration descriptor chain: config, interface, HID
    // descriptor, endpoint. 34 bytes. Needed because a HID driver
    // binds off THIS, not off the device descriptor -- and until
    // co-simulation ran a real enumeration, this model answered every
    // GET_DESCRIPTOR with the device descriptor regardless of what was
    // asked for, so the config request came back as 18 bytes of the
    // wrong structure and nothing ever bound.
    reg [7:0] cfgd [0:33];
    // What the current control read is serving from.
    reg [7:0] src [0:63];

    // test hooks
    integer se0_bits;
    reg bus_reset;
    integer nak_budget;
    reg stall_next;
    integer sof_count;
    integer setup_count;

    // A boot-protocol interrupt IN endpoint, endpoint 1. hid_have is
    // set by the testbench when there is a report to deliver; with it
    // clear the device NAKs, which is exactly what an idle mouse does
    // to every single poll and must not be mistaken for a report.
    reg [7:0] hid_r0, hid_r1, hid_r2, hid_r3;
    reg hid_have;
    reg ep1_toggle;
    integer ep1_reports;

    integer i, j, k;
    reg prev_lvl, cur_lvl, dbit;
    integer ones, bitpos, nbits;
    reg [7:0] shift;
    reg sync_done;
    reg eop;

    reg [15:0] c16;
    reg [4:0] c5;
    reg fb;

    // -- bus reset --
    //
    // SE0 held far longer than any EOP is a reset, and a real device
    // responds by returning to address 0. Modelling it matters for two
    // reasons: phase 3 hot-plug tests need it to be true, and the
    // release of a reset is a line transition that looks enough like a
    // packet start to be decoded as one. That transient is expected,
    // so the first packet after a reset is discarded silently rather
    // than reported as a protocol error -- which is what it would
    // otherwise look like in the log.
    initial begin
        se0_bits = 0;
        bus_reset = 1'b0;
        forever begin
            #(BIT_NS);
            if (in_se0) begin
                se0_bits = se0_bits + 1;
                if (se0_bits > 8) begin
                    bus_reset = 1'b1;
                    dev_addr = 7'd0;
                    pending_addr = 7'd0;
                end
            end else begin
                se0_bits = 0;
            end
        end
    end

    // ---------------------------------------------------------------
    // CRC helpers, derived independently from the spec
    // ---------------------------------------------------------------

    function [15:0] crc16f;
        input integer n;
        integer a, b;
        reg [15:0] c;
        reg f;
        begin
            c = 16'hFFFF;
            for (a = 0; a < n; a = a + 1)
                for (b = 0; b < 8; b = b + 1) begin
                    f = txb[a][b] ^ c[0];
                    c = c >> 1;
                    if (f) c = c ^ 16'hA001;
                end
            crc16f = ~c;
        end
    endfunction

    function [15:0] crc16rx;
        input integer n;
        integer a, b;
        reg [15:0] c;
        reg f;
        begin
            c = 16'hFFFF;
            for (a = 1; a < n; a = a + 1)
                for (b = 0; b < 8; b = b + 1) begin
                    f = rxb[a][b] ^ c[0];
                    c = c >> 1;
                    if (f) c = c ^ 16'hA001;
                end
            crc16rx = c;
        end
    endfunction

    function [4:0] crc5rx;
        input dummy;
        integer b;
        reg [15:0] fld;
        reg [4:0] c;
        reg f;
        begin
            fld = {rxb[2], rxb[1]};
            c = 5'h1F;
            for (b = 0; b < 11; b = b + 1) begin
                f = fld[b] ^ c[0];
                c = c >> 1;
                if (f) c = c ^ 5'h14;
            end
            crc5rx = ~c;
        end
    endfunction

    // ---------------------------------------------------------------
    // receive
    // ---------------------------------------------------------------

    task rx_preamble;
        begin
            // A preamble is SYNC + PRE PID at the FULL-speed rate and
            // has NO EOP -- the host holds the bus idle afterwards
            // instead. So this decodes exactly eight PID bits and
            // stops, rather than looking for a packet end that is
            // never coming.
            @(posedge in_k);
            #(FS_NS / 2.0);
            prev_lvl = 1'b1;
            sync_done = 1'b0;
            ones = 0;
            bitpos = 0;
            shift = 8'd0;
            nbits = 0;
            while (nbits < 8) begin
                cur_lvl = in_j;
                dbit = (cur_lvl == prev_lvl);
                prev_lvl = cur_lvl;
                if (ones == 6) begin
                    ones = 0;
                end else begin
                    if (dbit) ones = ones + 1;
                    else ones = 0;
                    if (!sync_done) begin
                        if (dbit) sync_done = 1'b1;
                    end else begin
                        shift = {dbit, shift[7:1]};
                        nbits = nbits + 1;
                    end
                end
                #(FS_NS);
            end
            if (shift[3:0] !== PID_PRE)
                $display("[dev] ERROR: expected PRE, got PID %b",
                         shift[3:0]);
        end
    endtask

    task rx_packet;
        begin
            rxn = 0;
            rx_crc_ok = 1'b0;
            rx_timeout = 1'b0;
            eop = 1'b0;

            if (PRE) rx_preamble;

            // Wait for the bus to leave idle. After a preamble the gap
            // holds J, so this is the same wait either way.
            @(posedge in_k);
`ifdef USB_TRACE
            $display("[dev %0t] sync K detected", $time);
`endif
            #(BIT_RX / 2.0);

            prev_lvl = 1'b1;
            sync_done = 1'b0;
            ones = 0;
            bitpos = 0;
            shift = 8'd0;

            while (!eop) begin
                if (in_se0) begin
                    eop = 1'b1;
                end else begin
                    cur_lvl = in_j;
                    dbit = (cur_lvl == prev_lvl);
                    prev_lvl = cur_lvl;
                    if (ones == 6) begin
                        ones = 0;
                    end else begin
                        if (dbit) ones = ones + 1;
                        else ones = 0;
                        if (!sync_done) begin
                            if (dbit) sync_done = 1'b1;
                        end else begin
                            shift = {dbit, shift[7:1]};
                            bitpos = bitpos + 1;
                            if (bitpos == 8) begin
                                rxb[rxn] = shift;
                                rxn = rxn + 1;
                                bitpos = 0;
                            end
                        end
                    end
                    #(BIT_RX);
                end
            end

            rxpid = rxb[0][3:0];
`ifdef USB_TRACE
            $display("[dev %0t] rx pid=%b n=%0d b0=%02x", $time,
                     rxb[0][3:0], rxn, rxb[0]);
`endif
            // Releasing a bus reset is a line transition that can look
            // like a packet start, so the garbage decoded out of that
            // transient is expected and stays quiet. Anything
            // well-formed clears the flag, so a genuine error after
            // the first good packet is still reported.
            if (rxb[0][7:4] !== ~rxb[0][3:0]) begin
                if (!bus_reset)
                    $display("[dev] ERROR: bad PID check field %02x",
                             rxb[0]);
            end else begin
                bus_reset = 1'b0;
            end

            if (rxpid == PID_DATA0 || rxpid == PID_DATA1)
                rx_crc_ok = (crc16rx(rxn) == 16'hB001);
            else if (rxpid == PID_IN || rxpid == PID_OUT ||
                     rxpid == PID_SETUP || rxpid == PID_SOF)
                rx_crc_ok = ((rxb[2][7:3]) == crc5rx(1'b0));
            else
                rx_crc_ok = 1'b1;

            if (!rx_crc_ok && !bus_reset) begin
                $display("[dev] ERROR: CRC failed on PID %b, %0d bytes",
                         rxpid, rxn);
                for (j = 0; j < rxn; j = j + 1)
                    $write(" %02x", rxb[j]);
                $write("\n");
            end
        end
    endtask

    // ---------------------------------------------------------------
    // transmit
    // ---------------------------------------------------------------

    task emit_bit;
        input v;
        begin
            if (ones == 6) begin
                drv_j = ~drv_j;
                #(BIT_NS);
                ones = 0;
            end
            if (!v) drv_j = ~drv_j;
            #(BIT_NS);
            if (v) ones = ones + 1;
            else ones = 0;
        end
    endtask

    task emit_byte;
        input [7:0] b;
        integer q;
        begin
            for (q = 0; q < 8; q = q + 1) emit_bit(b[q]);
        end
    endtask

    // Turnaround before answering. The device notices the EOP about
    // half a bit into its SE0, but the host keeps driving for another
    // 2.5 bit times -- two of SE0 and one of J. Answering two bit
    // times after detection therefore collides with the host's own
    // driver and puts x on the bus. Four is clear of it and still well
    // inside the 7.5 bit times a device is allowed to take.
    task turnaround;
        begin
            #(4.0 * BIT_NS);
        end
    endtask

    task tx_packet;
        input [3:0] pid;
        input integer n;
        integer q;
        reg [15:0] c;
        begin
            drv_en = 1'b1;
            drv_se0 = 1'b0;
            drv_j = 1'b1;
            ones = 0;
            emit_byte(8'h80);
            emit_byte({~pid, pid});
            if (pid == PID_DATA0 || pid == PID_DATA1) begin
                c = crc16f(n);
                for (q = 0; q < n; q = q + 1) emit_byte(txb[q]);
                emit_byte(c[7:0]);
                emit_byte(c[15:8]);
            end
            drv_se0 = 1'b1;
            #(2.0 * BIT_NS);
            drv_se0 = 1'b0;
            drv_j = 1'b1;
            #(BIT_NS);
            drv_en = 1'b0;
        end
    endtask

    // ---------------------------------------------------------------
    // device behaviour
    // ---------------------------------------------------------------

    initial begin
        drv_en = 1'b0;
        drv_j = 1'b1;
        drv_se0 = 1'b0;
        dev_addr = START_ADDR;
        pending_addr = START_ADDR;
        in_toggle = 1'b1;
        in_ptr = 0;
        in_len = 0;
        nak_budget = 0;
        stall_next = 1'b0;
        sof_count = 0;
        setup_count = 0;
        hid_have = 1'b0;
        hid_r0 = 8'h00; hid_r1 = 8'h00; hid_r2 = 8'h00; hid_r3 = 8'h00;
        ep1_toggle = 1'b0;
        ep1_reports = 0;

        // An 18-byte device descriptor with bMaxPacketSize0 = 8, so a
        // GET_DESCRIPTOR needs three IN transactions and the last one
        // is short. That exercises auto-continue and short-packet
        // termination rather than just a single-packet happy path.
        desc[0]  = 8'h12; desc[1]  = 8'h01;
        desc[2]  = 8'h00; desc[3]  = 8'h02;
        desc[4]  = 8'h00; desc[5]  = 8'h00;
        desc[6]  = 8'h00; desc[7]  = MPS0[7:0];
        desc[8]  = 8'hd8; desc[9]  = 8'h16;
        desc[10] = 8'h34; desc[11] = 8'h12;
        desc[12] = 8'h00; desc[13] = 8'h01;
        desc[14] = 8'h01; desc[15] = 8'h02;
        desc[16] = 8'h03; desc[17] = 8'h01;

        // config: 34 bytes total, 1 interface
        cfgd[0]  = 8'h09; cfgd[1]  = 8'h02;
        cfgd[2]  = 8'h22; cfgd[3]  = 8'h00;
        cfgd[4]  = 8'h01; cfgd[5]  = 8'h01;
        cfgd[6]  = 8'h00; cfgd[7]  = 8'ha0;
        cfgd[8]  = 8'h32;
        // interface 0: class 3 HID, subclass 1 boot, protocol 2 mouse
        cfgd[9]  = 8'h09; cfgd[10] = 8'h04;
        cfgd[11] = 8'h00; cfgd[12] = 8'h00;
        cfgd[13] = 8'h01; cfgd[14] = 8'h03;
        cfgd[15] = 8'h01; cfgd[16] = 8'h02;
        cfgd[17] = 8'h00;
        // HID descriptor -- present because real devices have one and
        // the driver's descriptor walk has to step over it correctly
        cfgd[18] = 8'h09; cfgd[19] = 8'h21;
        cfgd[20] = 8'h11; cfgd[21] = 8'h01;
        cfgd[22] = 8'h00; cfgd[23] = 8'h01;
        cfgd[24] = 8'h22; cfgd[25] = 8'h34;
        cfgd[26] = 8'h00;
        // endpoint 1 IN, interrupt, 4 bytes, 10 ms
        cfgd[27] = 8'h07; cfgd[28] = 8'h05;
        cfgd[29] = 8'h81; cfgd[30] = 8'h03;
        cfgd[31] = 8'h04; cfgd[32] = 8'h00;
        cfgd[33] = 8'h0a;
    end

    always begin

        // A detached model must not decode, and above all must not
        // report. The testbench hangs two models off port 1 and
        // enables one at a time; without this the idle one narrates
        // its neighbour's traffic as a stream of protocol errors.
        wait (attach === 1'b1);

        rx_packet;

        if (rxpid == PID_SOF) begin

            sof_count = sof_count + 1;

        end else if (rx_crc_ok && (rxb[1][6:0] == dev_addr) &&
                     (rxb[1][7] == 1'b1) && (rxb[2][2:0] == 3'd0)) begin

            // Endpoint 1: the boot-protocol interrupt IN.
            if (rxpid == PID_IN) begin
                turnaround;
                if (!hid_have) begin
                    tx_packet(PID_NAK, 0);
                end else begin
                    txb[0] = hid_r0;
                    txb[1] = hid_r1;
                    txb[2] = hid_r2;
                    txb[3] = hid_r3;
                    tx_packet(ep1_toggle ? PID_DATA1 : PID_DATA0, 4);
                    rx_packet;
                    if (rxpid == PID_ACK) begin
                        ep1_toggle = ~ep1_toggle;
                        hid_have = 1'b0;
                        ep1_reports = ep1_reports + 1;
                    end
                end
            end

        end else if (rx_crc_ok &&
                     (rxb[1][6:0] == dev_addr) &&
                     (rxb[1][7] == 1'b0) && (rxb[2][2:0] == 3'd0)) begin

            case (rxpid)

            PID_SETUP: begin
                rx_packet;
                if (rx_crc_ok && rxn >= 9) begin
                    for (i = 0; i < 8; i = i + 1) setup[i] = rxb[i + 1];
                    setup_count = setup_count + 1;
                    in_toggle = 1'b1;
                    in_ptr = 0;
                    // bRequest 6 is GET_DESCRIPTOR, 5 is SET_ADDRESS.
                    if (setup[1] == 8'h06) begin
                        in_len = {setup[7], setup[6]};
                        // wValue's high byte is the descriptor TYPE.
                        if (setup[3] == 8'h02) begin
                            if (in_len > 34) in_len = 34;
                            for (i = 0; i < in_len; i = i + 1)
                                src[i] = cfgd[i];
                        end else begin
                            if (in_len > 18) in_len = 18;
                            for (i = 0; i < in_len; i = i + 1)
                                src[i] = desc[i];
                        end
                    end else if (setup[1] == 8'h05) begin
                        pending_addr = setup[2][6:0];
                        in_len = 0;
                    end else begin
                        in_len = 0;
                    end
                    turnaround;
                    tx_packet(PID_ACK, 0);
                end
            end

            PID_IN: begin
                turnaround;
                if (nak_budget > 0) begin
                    nak_budget = nak_budget - 1;
                    tx_packet(PID_NAK, 0);
                end else if (stall_next) begin
                    stall_next = 1'b0;
                    tx_packet(PID_STALL, 0);
                end else begin
                    k = in_len - in_ptr;
                    if (k > MPS0) k = MPS0;
                    if (k < 0) k = 0;
                    for (i = 0; i < k; i = i + 1)
                        txb[i] = src[in_ptr + i];
                    tx_packet(in_toggle ? PID_DATA1 : PID_DATA0, k);
                    // The host's ACK follows; swallow it so the next
                    // rx_packet sees a token rather than a handshake.
                    rx_packet;
                    if (rxpid == PID_ACK) begin
                        in_ptr = in_ptr + k;
                        in_toggle = ~in_toggle;
                        // A control transfer with NO data stage has an
                        // IN status stage, not an OUT one -- so this
                        // is where SET_ADDRESS takes effect, and the
                        // OUT branch below is only the status stage of
                        // a control READ.
                        //
                        // This model had it only in the OUT branch,
                        // because until co-simulation drove it with a
                        // real enumeration sequence the only control
                        // transfer it had ever seen was
                        // GET_DESCRIPTOR. The host would move to the
                        // new address and the device would still be
                        // answering on zero.
                        if (setup[1] == 8'h05 && in_len == 0)
                            dev_addr = pending_addr;
                    end
                end
            end

            PID_OUT: begin
                // Status stage of a control read, or the data stage of
                // a control write. Either way: take it and ACK.
                rx_packet;
                turnaround;
                tx_packet(PID_ACK, 0);
                // SET_ADDRESS takes effect only after its status
                // stage completes, which is why this is here and not
                // where the request was decoded.
                if (setup[1] == 8'h05) dev_addr = pending_addr;
            end

            default: ;

            endcase

        end

    end

endmodule
