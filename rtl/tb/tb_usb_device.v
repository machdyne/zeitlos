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
    parameter integer CLK_PPM = 0,
    // 1: a bulk-only SCSI mass storage device instead of a boot mouse.
    // Endpoint 1 is bulk IN, endpoint 2 bulk OUT, both 64 bytes, and
    // the medium is DISK_SECTORS sectors held in this model. Default
    // 0, so every existing test sees the model it always saw.
    parameter integer MSC = 0,
    // 1: a four-port full-speed HUB. Class requests, a status change
    // endpoint on endpoint 1, and per-port power/connect/enable/reset
    // state. Devices behind it are separate instances of this model on
    // the SAME wires, gated by hub_en and reset by hub_rst -- a
    // full-speed hub is a repeater, so that is electrically what they
    // see. Replaces the tb_usb_hub.v phase 0 planned.
    parameter integer HUB = 0,
    // How long the hub takes to reset a port. Real hubs take 10-20 ms;
    // shorter here only to keep simulation time down.
    parameter integer HUB_RST_NS = 20000,
    // HUB=1: ports whose device is low speed, bit per port. The hub
    // reports it in wPortStatus; nothing else about the port differs.
    parameter [3:0] PORT_LS = 4'b0000,
    // 1: a CDC-ECM USB ethernet adapter shaped like the RTL8152: two
    // configurations, the first vendor-specific (class 0xff) and the
    // second CDC-ECM -- communications interface 0 with an interrupt
    // IN on endpoint 3, data interface 1 whose alternate setting 1
    // carries bulk IN 1 and bulk OUT 2, 64 bytes each. The MAC string
    // is index 3. Frames the testbench queues with ecm_push go out on
    // bulk IN; frames the host sends are checked against the same
    // pattern (ecm_pat) and counted. Default 0.
    parameter integer ECM = 0
) (
    inout wire dp,
    inout wire dm,
    input wire attach,
    // HUB=1: port p enabled (feed a child's attach), and in reset.
    output wire [3:0] hub_en,
    output wire [3:0] hub_rst,
    // HUB=1: a device is plugged into port p.
    input wire [3:0] hub_conn,
    // Behind a hub: the hub is resetting this device's port. The reset
    // never appears on the upstream wires, so it arrives here instead.
    input wire ext_reset
);

    assign hub_en = hp_ena & hp_con;
    assign hub_rst = hp_rsting;

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
    // 128 since ECM: its configuration descriptor is 80 bytes.
    reg [7:0] cfgd [0:127];

    // -- HUB=1 --
    reg [7:0] hubd [0:8];           // hub descriptor
    reg [3:0] hp_pwr, hp_con, hp_ena, hp_rsting;
    reg [3:0] hp_c_con, hp_c_ena, hp_c_rst;
    reg [3:0] hp_seen;              // hub_conn as last sampled
    time hp_rst_until [0:3];
    integer hub_resets;             // SET_FEATURE(PORT_RESET) seen
    integer hub_powers;             // SET_FEATURE(PORT_POWER) seen
    integer hub_reports;            // change bitmaps delivered
    integer hp, hq;                 // loop variables for the hub's own
                                    // processes, never the main loop's
    reg [7:0] hub_bits;
    reg pre_ok;                     // rx_preamble: a PRE was found
    reg ls_skip;                    // rx_packet: a low-speed packet, not ours
    reg tok_mine;                   // the last token was addressed to us
    // What the current control read is serving from.
    reg [7:0] src [0:127];

    // test hooks
    integer se0_bits;
    reg bus_reset;
    integer nak_budget;
    reg stall_next;
    // Hook: STALL the status stage of every SET_ADDRESS, so the address
    // is never taken and the device stays on address 0 -- a device
    // that fails enumeration while still answering there, as a real
    // keyboard behind a real hub did.
    reg stall_set_addr;
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

    // -- mass storage (MSC = 1) --
    //
    // Bulk-only transport is three phases per command: a 31-byte CBW
    // OUT, an optional data phase, a 13-byte CSW IN. The model follows
    // it strictly, so a host that reads one phase too many or too few
    // gets a NAK or the wrong bytes, not silent success.
    localparam integer DISK_SECTORS = 16;
    reg [7:0] disk [0:DISK_SECTORS * 512 - 1];
    reg [7:0] sbuf [0:63];          // small replies: sense, capacity
    reg [7:0] csw [0:12];
    integer cfg_total;
    integer msc_state;              // 0 idle, 1 data in, 2 CSW, 3 data out
    integer msc_base;               // data-in source: disk offset, or -1
    integer msc_dlen, msc_dptr;
    integer msc_pkts;               // packets sent this data phase
    reg msc_in_tgl, msc_out_tgl;
    // Test hooks. msc_naks_mid NAKs are inserted after the FIRST data
    // packet of every data-in phase -- a device fetching the next flash
    // page, which is what real sticks do and what a single up-front
    // NAK does not exercise. Set it above the host's hardware NAK
    // budget and the host has to resume a transfer part-way through.
    integer msc_naks_mid;
    integer msc_nak_left;
    integer msc_cmds, msc_csws;
    integer msc_proto_err;          // anything out of sequence
    integer msc_dup_out;            // OUT with the wrong toggle
    integer msc_n, msc_lba, msc_want, msc_avail;    // msc_cbw scratch
    integer msc_blocks;     // READ(10)/WRITE(10) transfer length, CDB 7-8
    // Bit-error hook. While nonzero, every data-in packet goes out with
    // its CRC16 inverted and this counts down -- a transmission error
    // the host must detect, refuse to ACK, and retry. The packet is
    // resent with the SAME toggle and payload, as a real device does
    // when it sees no ACK.
    integer msc_crc_bad;
    integer msc_crc_at;             // ...starting at this packet index
    integer msc_crc_sent;           // corrupted packets actually sent
    // tx_packet's CRC corruption for the packet it is about to send.
    reg tx_crc_flip;
    // Endpoint halts, BOT 6.7.2. While set, that bulk endpoint answers
    // STALL; CLEAR_FEATURE(ENDPOINT_HALT) clears it and resets the
    // endpoint's toggle to DATA0.
    reg msc_halt_in, msc_halt_out;
    // Hook: a data-in phase with NOTHING to send (READ past the end of
    // the medium) halts the IN pipe instead of sending a zero-length
    // packet. Both are legal; many real devices do this one.
    reg msc_stall_short;
    // Hook: the next N CSWs go out with a corrupted signature.
    integer msc_bad_csw;
    // Hook: the next N host ACKs to data-in packets are "lost" -- the
    // model behaves as though it never saw them and resends the same
    // packet with the same toggle, which is what a real device does.
    integer msc_ack_lost;
    integer msc_resets;             // Bulk-Only Mass Storage Resets seen
    integer msc_clears;             // CLEAR_FEATURE(ENDPOINT_HALT) seen
    integer msc_aborted;            // commands a reset ended before their CSW
    // -- CDC-ECM (ECM = 1) --
    reg [7:0] cfge [0:127];         // configuration index 1: ECM
    integer cfge_total;
    reg [7:0] ecm_cfg;              // SET_CONFIGURATION value
    reg [7:0] ecm_alt;              // data interface alternate setting
    reg [7:0] ecm_filter;           // SET_ETHERNET_PACKET_FILTER wValue
    integer ecm_filters;            // ...requests seen
    reg ecm_in_tgl, ecm_out_tgl, ecm_int_tgl;
    // Frames queued for bulk IN: length and seed, a ring of 16.
    integer ecm_qlen [0:15];
    reg [7:0] ecm_qseed [0:15];
    integer ecm_qh, ecm_qt;
    integer ecm_ptr;                // bytes of the head frame sent
    integer ecm_sent;               // frames fully sent (ACKed)
    integer ecm_pkts;               // packets of the head frame sent
    integer ecm_naks_mid, ecm_nak_left;   // NAKs after the first packet
    integer ecm_crc_bad, ecm_crc_at;       // corrupt packets, from index
    // Frames received on bulk OUT.
    reg [7:0] ecm_rb [0:2047];
    integer ecm_rn;
    integer ecm_rx_frames, ecm_rx_bad, ecm_rx_last, ecm_rx_zlp_only;
    integer ecm_out_naks;           // NAK the next N OUT packets
    integer ecm_dup_out;
    integer ecm_bad_ep;             // data endpoint used before SET_INTERFACE
    // Notification: 0 none, 1 connected, 2 disconnected; sent once.
    integer ecm_notify;
    integer ecm_notes;              // notifications delivered
    integer ecm_q;                  // scratch for the ECM paths

    // Test pattern for a frame: byte 0 is the seed, the rest a function
    // of seed and position. rtl/tb/cosim/usbh_vpi.c has the same.
    function [7:0] ecm_pat;
        input [7:0] seed;
        input integer pos;
        begin
            if (pos == 0) ecm_pat = seed;
            else ecm_pat = (seed * 29 + pos * 7 + (pos >> 8) * 3) & 8'hff;
        end
    endfunction

    task ecm_push;
        input integer len;
        input [7:0] seed;
        begin
            ecm_qlen[ecm_qt] = len;
            ecm_qseed[ecm_qt] = seed;
            ecm_qt = (ecm_qt + 1) % 16;
        end
    endtask

    task ecm_reset;
        begin
            ecm_cfg = 8'd0;
            ecm_alt = 8'd0;
            ecm_in_tgl = 1'b0;
            ecm_out_tgl = 1'b0;
            ecm_int_tgl = 1'b0;
            ecm_ptr = 0;
            ecm_pkts = 0;
            ecm_rn = 0;
            ecm_nak_left = 0;
        end
    endtask

    // A packet received where an ACK was expected. It is the host's
    // next token (it saw an error and did not ACK), so the main loop
    // must dispatch it rather than read a new one.
    reg pkt_held;

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
                // SE0 for 2.5 us is a reset (USB 2.0 7.1.7.5, TDETRST).
                // This counted 8 of the model's OWN bit times, 667 ns at
                // full speed -- shorter than the ~1.33 us SE0 ending a
                // low-speed packet, so on a hub segment every full-speed
                // model reset itself after each packet to a PRE device.
                if (se0_bits * BIT_NS > 2500.0) begin
                    bus_reset = 1'b1;
                    dev_addr = 7'd0;
                    pending_addr = 7'd0;
                    // A bus reset returns every endpoint to its
                    // initial state: toggles to DATA0, halts cleared,
                    // bulk-only transport waiting for a CBW. This model
                    // used to reset only its address.
                    ep1_toggle = 1'b0;
                    if (msc_state != 0) msc_aborted = msc_aborted + 1;
                    msc_state = 0;
                    msc_in_tgl = 1'b0;
                    msc_out_tgl = 1'b0;
                    msc_halt_in = 1'b0;
                    msc_halt_out = 1'b0;
                    msc_nak_left = 0;
                    if (ECM) ecm_reset;
                    // A reset hub comes back with every port unpowered
                    // and nothing enabled (USB 2.0 11.10); so does one
                    // that was unplugged and plugged back in.
                    if (HUB) begin
                        hp_pwr = 4'd0; hp_con = 4'd0; hp_ena = 4'd0;
                        hp_rsting = 4'd0; hp_seen = 4'd0;
                        hp_c_con = 4'd0; hp_c_ena = 4'd0; hp_c_rst = 4'd0;
                    end
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
            // Behind a hub, a low-speed port is sent only what follows a
            // PRE; full-speed traffic to other devices on the hub is
            // never repeated to it. This model sits on the upstream
            // wires and hears all of it, so it skips any packet that
            // does not start with PRE rather than decoding it as its
            // own.
            pre_ok = 1'b0;
            while (!pre_ok) begin
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
            // The WHOLE byte, check field included, as a hub checks it.
            // This compared only the low nibble, so a PRE sent with a
            // wrong check field passed here and would be ignored by
            // any real hub -- which is exactly what usb_sie.v did.
            if (shift === {~PID_PRE, PID_PRE}) begin
                pre_ok = 1'b1;
            end else begin
                if (shift[3:0] === PID_PRE)
                    $display("[%m] ERROR: PRE with bad PID check field %02x -- a hub ignores it",
                             shift);
                // Not ours: wait for this packet's end and look again.
                wait (in_se0);
                wait (!in_se0);
            end
            end
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
            $display("[%m %0t] sync K detected", $time);
`endif
            #(BIT_RX / 2.0);

            // A full-speed model on a hub segment also hears the
            // replies of low-speed devices behind the hub: they go
            // upstream at low speed with no PRE. A real full-speed
            // device on another hub port never would -- a hub repeats
            // that traffic upstream only -- so skip it. At full-speed
            // rate a low-speed SYNC's first K lasts eight bit times, a
            // full-speed one's just one: sample once more, and if it is
            // still K, wait out the packet.
            ls_skip = 1'b0;
            prev_lvl = 1'b1;
            if (MODE == 0) begin
                #(BIT_RX);
                if (in_k) ls_skip = 1'b1;
                // Otherwise decoding carries on from here, one bit in:
                // the bit already passed was the SYNC's opening K.
                prev_lvl = 1'b0;
            end
            if (ls_skip) begin
                wait (in_se0);
                wait (!in_se0);
                eop = 1'b1;
            end

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
            $display("[%m %0t] rx pid=%b n=%0d b0=%02x", $time,
                     rxb[0][3:0], rxn, rxb[0]);
`endif
            // Releasing a bus reset is a line transition that can look
            // like a packet start, so the garbage decoded out of that
            // transient is expected and stays quiet. Anything
            // well-formed clears the flag, so a genuine error after
            // the first good packet is still reported.
            if (ls_skip) begin
                // nothing decoded, nothing to report
            end else if (rxb[0][7:4] !== ~rxb[0][3:0]) begin
                if (!bus_reset)
                    $display("[%m] ERROR: bad PID check field %02x",
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

            // A skipped packet is PID 0000 (reserved) with a failed CRC,
            // so nothing downstream acts on it.
            if (ls_skip) begin
                rxpid = 4'b0000;
                rx_crc_ok = 1'b0;
            end

            // Whether the data that follows is ours to judge. On a hub
            // segment every model hears every device's data; a model
            // whose receiver is off-rate from ANOTHER model's
            // transmitter fails the CRC on data that was never for it.
            // Only data after a token to this device is reported.
            if (rxpid == PID_IN || rxpid == PID_OUT || rxpid == PID_SETUP)
                tok_mine = rx_crc_ok && (rxb[1][6:0] == dev_addr);

            if (!rx_crc_ok && !bus_reset && !ls_skip &&
                (!(rxpid == PID_DATA0 || rxpid == PID_DATA1) || tok_mine)) begin
                $display("[%m] ERROR: CRC failed on PID %b, %0d bytes",
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
    // Hook: extra delay before answering, in ns. A real keyboard behind
    // a real hub answered 2.9 us after the host's EOP -- legal, but
    // later than this model's 4 bit times.
    real extra_turn_ns;
    initial extra_turn_ns = 0.0;

    task turnaround;
        begin
            #(4.0 * BIT_NS + extra_turn_ns);
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
                if (tx_crc_flip) c = ~c;
                for (q = 0; q < n; q = q + 1) emit_byte(txb[q]);
                emit_byte(c[7:0]);
                emit_byte(c[15:8]);
            end
            drv_se0 = 1'b1;
            #(2.0 * BIT_NS);
            // J BEFORE releasing SE0. The other order drove K for zero
            // time whenever the last data bit left the line in K -- a
            // glitch the host's clocked receiver never sees, but one
            // that `@(posedge in_k)` in any OTHER model on the same
            // wires takes as a packet start. Behind a hub several models
            // share the wires, and the hub model then began a bogus
            // packet, sampled idle bus, and swallowed the host's next
            // token.
            drv_j = 1'b1;
            drv_se0 = 1'b0;
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
        stall_set_addr = 1'b0;
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
        cfg_total = 34;

        msc_state = 0;
        msc_base = -1;
        msc_dlen = 0;
        msc_dptr = 0;
        msc_pkts = 0;
        msc_in_tgl = 1'b0;
        msc_out_tgl = 1'b0;
        msc_naks_mid = 0;
        msc_nak_left = 0;
        msc_crc_bad = 0;
        msc_halt_in = 1'b0;
        msc_halt_out = 1'b0;
        msc_stall_short = 1'b0;
        msc_bad_csw = 0;
        msc_ack_lost = 0;
        msc_resets = 0;
        msc_clears = 0;
        msc_aborted = 0;
        msc_crc_at = 0;
        msc_crc_sent = 0;
        tx_crc_flip = 1'b0;
        pkt_held = 1'b0;
        msc_cmds = 0;
        msc_csws = 0;
        msc_proto_err = 0;
        msc_dup_out = 0;

        hp_pwr = 4'd0; hp_con = 4'd0; hp_ena = 4'd0; hp_rsting = 4'd0;
        hp_c_con = 4'd0; hp_c_ena = 4'd0; hp_c_rst = 4'd0;
        hp_seen = 4'd0;
        hub_resets = 0;
        hub_powers = 0;
        hub_reports = 0;
        tok_mine = 1'b0;
        if (HUB) begin
            desc[4] = 8'h09;                // bDeviceClass: hub
            // config 25 bytes: one interface of class 9, one interrupt
            // IN endpoint, 1 byte, every frame.
            cfg_total = 25;
            cfgd[0]  = 8'h09; cfgd[1]  = 8'h02;
            cfgd[2]  = 8'h19; cfgd[3]  = 8'h00;
            cfgd[4]  = 8'h01; cfgd[5]  = 8'h01;
            cfgd[6]  = 8'h00; cfgd[7]  = 8'he0;
            cfgd[8]  = 8'h32;
            cfgd[9]  = 8'h09; cfgd[10] = 8'h04;
            cfgd[11] = 8'h00; cfgd[12] = 8'h00;
            cfgd[13] = 8'h01; cfgd[14] = 8'h09;
            cfgd[15] = 8'h00; cfgd[16] = 8'h00;
            cfgd[17] = 8'h00;
            cfgd[18] = 8'h07; cfgd[19] = 8'h05;
            cfgd[20] = 8'h81; cfgd[21] = 8'h03;
            cfgd[22] = 8'h01; cfgd[23] = 8'h00;
            cfgd[24] = 8'h01;
            // hub descriptor: 4 ports, per-port power switching,
            // bPwrOn2PwrGood 10 (20 ms), nothing non-removable
            hubd[0] = 8'h09; hubd[1] = 8'h29; hubd[2] = 8'h04;
            hubd[3] = 8'h01; hubd[4] = 8'h00; hubd[5] = 8'h0a;
            hubd[6] = 8'h00; hubd[7] = 8'h00; hubd[8] = 8'hff;
        end

        ecm_filters = 0; ecm_filter = 8'd0;
        ecm_qh = 0; ecm_qt = 0; ecm_sent = 0;
        ecm_naks_mid = 0; ecm_crc_bad = 0; ecm_crc_at = 0;
        ecm_rx_frames = 0; ecm_rx_bad = 0; ecm_rx_last = 0;
        ecm_rx_zlp_only = 0; ecm_out_naks = 0; ecm_dup_out = 0;
        ecm_bad_ep = 0; ecm_notify = 0; ecm_notes = 0;
        ecm_reset;
        if (ECM) begin
            desc[7] = 8'h40;                // 64, as the RTL8152
            desc[8] = 8'hda; desc[9] = 8'h0b;   // 0bda:8152
            desc[10] = 8'h52; desc[11] = 8'h81;
            desc[17] = 8'h02;               // two configurations
            // index 0 (value 1): vendor-specific, three endpoints -- what
            // Realtek's own driver uses. 39 bytes.
            cfg_total = 39;
            cfgd[0]  = 8'h09; cfgd[1]  = 8'h02; cfgd[2]  = 8'h27;
            cfgd[3]  = 8'h00; cfgd[4]  = 8'h01; cfgd[5]  = 8'h01;
            cfgd[6]  = 8'h00; cfgd[7]  = 8'ha0; cfgd[8]  = 8'h32;
            cfgd[9]  = 8'h09; cfgd[10] = 8'h04; cfgd[11] = 8'h00;
            cfgd[12] = 8'h00; cfgd[13] = 8'h03; cfgd[14] = 8'hff;
            cfgd[15] = 8'hff; cfgd[16] = 8'h00; cfgd[17] = 8'h00;
            cfgd[18] = 8'h07; cfgd[19] = 8'h05; cfgd[20] = 8'h81;
            cfgd[21] = 8'h02; cfgd[22] = 8'h40; cfgd[23] = 8'h00;
            cfgd[24] = 8'h00;
            cfgd[25] = 8'h07; cfgd[26] = 8'h05; cfgd[27] = 8'h02;
            cfgd[28] = 8'h02; cfgd[29] = 8'h40; cfgd[30] = 8'h00;
            cfgd[31] = 8'h00;
            cfgd[32] = 8'h07; cfgd[33] = 8'h05; cfgd[34] = 8'h83;
            cfgd[35] = 8'h03; cfgd[36] = 8'h02; cfgd[37] = 8'h00;
            cfgd[38] = 8'h08;
            // index 1 (value 2): CDC-ECM. 80 bytes.
            cfge_total = 80;
            cfge[0]  = 8'h09; cfge[1]  = 8'h02; cfge[2]  = 8'h50;
            cfge[3]  = 8'h00; cfge[4]  = 8'h02; cfge[5]  = 8'h02;
            cfge[6]  = 8'h00; cfge[7]  = 8'ha0; cfge[8]  = 8'h32;
            // interface 0: communications, ECM
            cfge[9]  = 8'h09; cfge[10] = 8'h04; cfge[11] = 8'h00;
            cfge[12] = 8'h00; cfge[13] = 8'h01; cfge[14] = 8'h02;
            cfge[15] = 8'h06; cfge[16] = 8'h00; cfge[17] = 8'h00;
            // header, union (0 -> 1), Ethernet networking
            cfge[18] = 8'h05; cfge[19] = 8'h24; cfge[20] = 8'h00;
            cfge[21] = 8'h10; cfge[22] = 8'h01;
            cfge[23] = 8'h05; cfge[24] = 8'h24; cfge[25] = 8'h06;
            cfge[26] = 8'h00; cfge[27] = 8'h01;
            cfge[28] = 8'h0d; cfge[29] = 8'h24; cfge[30] = 8'h0f;
            cfge[31] = 8'h03;               // iMACAddress
            cfge[32] = 8'h00; cfge[33] = 8'h00; cfge[34] = 8'h00;
            cfge[35] = 8'h00;
            cfge[36] = 8'hea; cfge[37] = 8'h05;  // wMaxSegmentSize 1514
            cfge[38] = 8'h00; cfge[39] = 8'h00; cfge[40] = 8'h00;
            // endpoint 0x83 interrupt IN, 16 bytes, 8 ms
            cfge[41] = 8'h07; cfge[42] = 8'h05; cfge[43] = 8'h83;
            cfge[44] = 8'h03; cfge[45] = 8'h10; cfge[46] = 8'h00;
            cfge[47] = 8'h08;
            // interface 1 alt 0: data, no endpoints
            cfge[48] = 8'h09; cfge[49] = 8'h04; cfge[50] = 8'h01;
            cfge[51] = 8'h00; cfge[52] = 8'h00; cfge[53] = 8'h0a;
            cfge[54] = 8'h00; cfge[55] = 8'h00; cfge[56] = 8'h00;
            // interface 1 alt 1: data, bulk IN 1 and OUT 2
            cfge[57] = 8'h09; cfge[58] = 8'h04; cfge[59] = 8'h01;
            cfge[60] = 8'h01; cfge[61] = 8'h02; cfge[62] = 8'h0a;
            cfge[63] = 8'h00; cfge[64] = 8'h00; cfge[65] = 8'h00;
            cfge[66] = 8'h07; cfge[67] = 8'h05; cfge[68] = 8'h81;
            cfge[69] = 8'h02; cfge[70] = 8'h40; cfge[71] = 8'h00;
            cfge[72] = 8'h00;
            cfge[73] = 8'h07; cfge[74] = 8'h05; cfge[75] = 8'h02;
            cfge[76] = 8'h02; cfge[77] = 8'h40; cfge[78] = 8'h00;
            cfge[79] = 8'h00;
        end

        if (MSC) begin
            // config: 32 bytes, 1 interface, 2 bulk endpoints
            cfg_total = 32;
            cfgd[0]  = 8'h09; cfgd[1]  = 8'h02;
            cfgd[2]  = 8'h20; cfgd[3]  = 8'h00;
            cfgd[4]  = 8'h01; cfgd[5]  = 8'h01;
            cfgd[6]  = 8'h00; cfgd[7]  = 8'h80;
            cfgd[8]  = 8'h32;
            // interface 0: class 8 mass storage, 6 SCSI, 0x50 BOT
            cfgd[9]  = 8'h09; cfgd[10] = 8'h04;
            cfgd[11] = 8'h00; cfgd[12] = 8'h00;
            cfgd[13] = 8'h02; cfgd[14] = 8'h08;
            cfgd[15] = 8'h06; cfgd[16] = 8'h50;
            cfgd[17] = 8'h00;
            // endpoint 0x81 bulk IN, 64
            cfgd[18] = 8'h07; cfgd[19] = 8'h05;
            cfgd[20] = 8'h81; cfgd[21] = 8'h02;
            cfgd[22] = 8'h40; cfgd[23] = 8'h00;
            cfgd[24] = 8'h00;
            // endpoint 0x02 bulk OUT, 64
            cfgd[25] = 8'h07; cfgd[26] = 8'h05;
            cfgd[27] = 8'h02; cfgd[28] = 8'h02;
            cfgd[29] = 8'h40; cfgd[30] = 8'h00;
            cfgd[31] = 8'h00;

            // The medium. Sector 0 starts like a FAT boot sector and
            // ends in the 55 AA signature, so a truncated read shows
            // the same leading bytes the hardware did. Every other
            // byte is a function of its position, mirrored in
            // rtl/tb/cosim/usbh_vpi.c, so the driver's copy can be
            // compared byte for byte.
            for (i = 0; i < DISK_SECTORS * 512; i = i + 1)
                disk[i] = (i * 7 + (i >> 9) * 13) & 8'hff;
            disk[0] = 8'heb; disk[1] = 8'h3c;
            disk[2] = 8'h90; disk[3] = 8'h6d;
            disk[510] = 8'h55; disk[511] = 8'haa;
        end
    end

    // -- HUB=1: a class request, decoded from setup[] --
    //
    // Sets in_len/src[] for the IN data stage the generic code then
    // runs; 0 for the no-data requests, whose status stage is an IN.
    task hub_request;
        begin
            hq = setup[4] - 1;              // port index, wIndex 1-based
            in_len = 0;
            if (setup[1] == 8'h06 && setup[3] == 8'h29) begin
                in_len = {setup[7], setup[6]};
                if (in_len > 9) in_len = 9;
                for (i = 0; i < 9; i = i + 1) src[i] = hubd[i];
            end else if (setup[1] == 8'h00) begin
                // GET_STATUS: the hub's is all zero; a port's is
                // wPortStatus then wPortChange.
                for (i = 0; i < 4; i = i + 1) src[i] = 8'h00;
                if ((setup[0] & 8'h1f) == 8'h03 && hq >= 0 && hq < 4) begin
                    src[0] = {3'b000, hp_rsting[hq], 2'b00,
                              hp_ena[hq], hp_con[hq]};
                    // bit 8 power, bit 9 low speed (PORT_LS)
                    src[1] = {6'd0, (PORT_LS[hq] ? 1'b1 : 1'b0), hp_pwr[hq]};
                    src[2] = {3'b000, hp_c_rst[hq], 2'b00,
                              hp_c_ena[hq], hp_c_con[hq]};
                    src[3] = 8'h00;
                end
                in_len = {setup[7], setup[6]};
                if (in_len > 4) in_len = 4;
            end else if (setup[1] == 8'h03 && (setup[0] & 8'h1f) == 8'h03 &&
                         hq >= 0 && hq < 4) begin
                // SET_FEATURE on a port
                if (setup[2] == 8'd8) begin
                    hub_powers = hub_powers + 1;
                    if (!hp_pwr[hq]) begin
                        hp_pwr[hq] = 1'b1;
                        if (hub_conn[hq] === 1'b1) begin
                            hp_con[hq] = 1'b1;
                            hp_c_con[hq] = 1'b1;
                        end
                        hp_seen[hq] = (hub_conn[hq] === 1'b1);
                    end
                end else if (setup[2] == 8'd4 && hp_con[hq]) begin
                    hub_resets = hub_resets + 1;
                    $display("[hub %0t] reset port %0d (ena %b rsting %b)",
                             $time, hq + 1, hp_ena[hq], hp_rsting[hq]);
                    hp_ena[hq] = 1'b0;
                    hp_rsting[hq] = 1'b1;
                    hp_rst_until[hq] = $time + HUB_RST_NS;
                end
            end else if (setup[1] == 8'h01 && (setup[0] & 8'h1f) == 8'h03 &&
                         hq >= 0 && hq < 4) begin
                // CLEAR_FEATURE on a port
                case (setup[2])
                8'd1:  hp_ena[hq] = 1'b0;
                8'd8:  begin hp_pwr[hq] = 1'b0; hp_con[hq] = 1'b0;
                             hp_ena[hq] = 1'b0; end
                8'd16: hp_c_con[hq] = 1'b0;
                8'd17: hp_c_ena[hq] = 1'b0;
                8'd20: hp_c_rst[hq] = 1'b0;
                default: ;
                endcase
            end
        end
    endtask

    // -- HUB=1: plugs, unplugs and reset completion, per port --
    initial begin
        #1;
        if (HUB) forever begin
            #500;
            for (hp = 0; hp < 4; hp = hp + 1) begin
                if (hp_pwr[hp] && (hub_conn[hp] === 1'b1) != hp_seen[hp]) begin
                    hp_seen[hp] = (hub_conn[hp] === 1'b1);
                    hp_con[hp] = hp_seen[hp];
                    hp_c_con[hp] = 1'b1;
                    if (!hp_seen[hp]) begin
                        hp_ena[hp] = 1'b0;
                        hp_rsting[hp] = 1'b0;
                    end
                end
                if (hp_rsting[hp] && $time >= hp_rst_until[hp]) begin
                    hp_rsting[hp] = 1'b0;
                    hp_ena[hp] = hp_con[hp];
                    hp_c_rst[hp] = 1'b1;
                end
            end
        end
    end

    // -- behind a hub: its port reset is this device's bus reset --
    always @(posedge ext_reset) begin
        dev_addr = 7'd0;
        pending_addr = 7'd0;
        ep1_toggle = 1'b0;
        if (msc_state != 0) msc_aborted = msc_aborted + 1;
        msc_state = 0;
        msc_in_tgl = 1'b0;
        msc_out_tgl = 1'b0;
        msc_halt_in = 1'b0;
        msc_halt_out = 1'b0;
        msc_nak_left = 0;
        pkt_held = 1'b0;
        if (ECM) ecm_reset;
    end

    // -- a CBW has arrived: decide the data phase and queue the CSW --
    task msc_cbw;
        begin
            msc_n = rxn - 3;
            if (msc_n != 31 || rxb[1] != 8'h55 || rxb[2] != 8'h53 ||
                rxb[3] != 8'h42 || rxb[4] != 8'h43 || msc_state != 0) begin
                $display("[msc] ERROR: bad CBW (n=%0d, state %0d)",
                         msc_n, msc_state);
                msc_proto_err = msc_proto_err + 1;
            end else begin
                msc_cmds = msc_cmds + 1;
                msc_want = {rxb[12], rxb[11], rxb[10], rxb[9]};
                msc_lba = {rxb[18], rxb[19], rxb[20], rxb[21]};
                // CSW: signature, tag echoed, residue 0, passed
                csw[0] = 8'h55; csw[1] = 8'h53;
                csw[2] = 8'h42; csw[3] = 8'h53;
                csw[4] = rxb[5]; csw[5] = rxb[6];
                csw[6] = rxb[7]; csw[7] = rxb[8];
                csw[8] = 0; csw[9] = 0; csw[10] = 0; csw[11] = 0;
                csw[12] = 0;
                for (i = 0; i < 64; i = i + 1) sbuf[i] = 8'h00;
                msc_base = -1;
                msc_avail = 0;
                case (rxb[16])
                8'h03: begin                    // REQUEST SENSE
                    sbuf[0] = 8'h70; sbuf[7] = 8'h0a;
                    msc_avail = 18;
                end
                8'h12: begin                    // INQUIRY
                    sbuf[1] = 8'h80; sbuf[4] = 8'h1f;
                    msc_avail = 36;
                end
                8'h25: begin                    // READ CAPACITY(10)
                    sbuf[3] = DISK_SECTORS - 1;
                    sbuf[6] = 8'h02;            // 512
                    msc_avail = 8;
                end
                // READ(10)/WRITE(10): the transfer length is CDB bytes
                // 7-8 (rxb[23], rxb[24]). This model used to serve one
                // sector per command whatever was asked, which a real
                // drive does not; the driver now asks for runs
                // (usbh_msc.c, MSC_MAX_SECTORS).
                8'h28: begin                    // READ(10)
                    msc_blocks = {rxb[23], rxb[24]};
                    if (msc_blocks > 0 &&
                        msc_lba + msc_blocks <= DISK_SECTORS) begin
                        msc_base = msc_lba * 512;
                        msc_avail = msc_blocks * 512;
                    end else csw[12] = 1;
                end
                8'h2a: begin                    // WRITE(10)
                    msc_blocks = {rxb[23], rxb[24]};
                    if (msc_blocks > 0 &&
                        msc_lba + msc_blocks <= DISK_SECTORS)
                        msc_base = msc_lba * 512;
                    else csw[12] = 1;
                end
                default: msc_avail = 0;             // TEST UNIT READY etc.
                endcase
                msc_dlen = (msc_want < msc_avail) ? msc_want : msc_avail;
                if (rxb[16] == 8'h2a) msc_dlen = msc_want;
                msc_dptr = 0;
                msc_pkts = 0;
                if (msc_want == 0) msc_state = 2;
                else if (rxb[13][7]) msc_state = 1;
                else msc_state = 3;
                // dCSWDataResidue: what the host asked for and did not
                // get. Only ever nonzero here for a data-in phase.
                if (rxb[13][7] && msc_want > msc_dlen) begin
                    csw[8]  = (msc_want - msc_dlen) & 8'hff;
                    csw[9]  = ((msc_want - msc_dlen) >> 8) & 8'hff;
                end
                if (msc_stall_short && rxb[13][7] && msc_want > 0 &&
                    msc_dlen == 0) begin
                    msc_halt_in = 1'b1;
                    msc_state = 2;
                end
            end
        end
    endtask

    always begin

        // A detached model must not decode, and above all must not
        // report. The testbench hangs two models off port 1 and
        // enables one at a time; without this the idle one narrates
        // its neighbour's traffic as a stream of protocol errors.
        wait (attach === 1'b1);

        if (pkt_held) pkt_held = 1'b0;
        else rx_packet;

        if (rxpid == PID_SOF) begin

            sof_count = sof_count + 1;

        end else if (rx_crc_ok && (rxb[1][6:0] == dev_addr) &&
                     (rxb[1][7] == 1'b1) && (rxb[2][2:0] == 3'd0)) begin

            // Endpoint 1, ECM: bulk IN, the head of the frame queue in
            // 64-byte packets, a zero-length packet after a frame that
            // is an exact multiple of 64.
            if (ECM && rxpid == PID_IN) begin
                turnaround;
                if (ecm_cfg != 8'd2 || ecm_alt != 8'd1) begin
                    ecm_bad_ep = ecm_bad_ep + 1;
                    tx_packet(PID_STALL, 0);
                end else if (ecm_qh == ecm_qt) begin
                    tx_packet(PID_NAK, 0);
                end else if (ecm_pkts > 0 && ecm_nak_left > 0) begin
                    ecm_nak_left = ecm_nak_left - 1;
                    tx_packet(PID_NAK, 0);
                end else begin
                    k = ecm_qlen[ecm_qh] - ecm_ptr;
                    if (k > 64) k = 64;
                    for (i = 0; i < k; i = i + 1)
                        txb[i] = ecm_pat(ecm_qseed[ecm_qh], ecm_ptr + i);
                    tx_crc_flip = (ecm_crc_bad > 0) && (ecm_pkts >= ecm_crc_at);
                    if (tx_crc_flip) ecm_crc_bad = ecm_crc_bad - 1;
                    tx_packet(ecm_in_tgl ? PID_DATA1 : PID_DATA0, k);
                    tx_crc_flip = 1'b0;
                    rx_packet;
                    if (rxpid != PID_ACK) pkt_held = 1'b1;
                    if (rxpid == PID_ACK) begin
                        ecm_in_tgl = ~ecm_in_tgl;
                        ecm_pkts = ecm_pkts + 1;
                        if (ecm_pkts == 1) ecm_nak_left = ecm_naks_mid;
                        ecm_ptr = ecm_ptr + k;
                        // Done after a short packet -- including the
                        // zero-length one that follows an exact multiple.
                        if (k < 64) begin
                            ecm_qh = (ecm_qh + 1) % 16;
                            ecm_ptr = 0;
                            ecm_pkts = 0;
                            ecm_nak_left = 0;
                            ecm_sent = ecm_sent + 1;
                        end
                    end
                end
            end else
            // Endpoint 1, mass storage: bulk IN, data phase then CSW.
            if (MSC && rxpid == PID_IN) begin
                turnaround;
                if (msc_halt_in) begin
                    tx_packet(PID_STALL, 0);
                end else if (msc_state == 1 && msc_nak_left > 0) begin
                    msc_nak_left = msc_nak_left - 1;
                    tx_packet(PID_NAK, 0);
                end else if (msc_state == 1) begin
                    k = msc_dlen - msc_dptr;
                    if (k > 64) k = 64;
                    for (i = 0; i < k; i = i + 1)
                        txb[i] = (msc_base >= 0) ?
                            disk[msc_base + msc_dptr + i] :
                            sbuf[msc_dptr + i];
                    tx_crc_flip = (msc_crc_bad > 0) &&
                                  (msc_pkts >= msc_crc_at);
                    if (tx_crc_flip) begin
                        msc_crc_bad = msc_crc_bad - 1;
                        msc_crc_sent = msc_crc_sent + 1;
                    end
                    tx_packet(msc_in_tgl ? PID_DATA1 : PID_DATA0, k);
                    tx_crc_flip = 1'b0;
                    rx_packet;
                    // No ACK: the host rejected the packet. Nothing
                    // advances, and what arrived instead is its next
                    // token.
                    if (rxpid != PID_ACK) pkt_held = 1'b1;
                    // A lost ACK: the host took the packet, the device
                    // thinks it did not, and will send it again with
                    // the same toggle.
                    if (rxpid == PID_ACK && msc_ack_lost > 0) begin
                        msc_ack_lost = msc_ack_lost - 1;
                        rxpid = PID_NAK;
                    end
                    if (rxpid == PID_ACK) begin
                        msc_in_tgl = ~msc_in_tgl;
                        msc_dptr = msc_dptr + k;
                        msc_pkts = msc_pkts + 1;
                        if (msc_pkts == 1) msc_nak_left = msc_naks_mid;
                        // A full final packet ends the phase too: BOT
                        // sends no zero-length packet when the host
                        // asked for exactly this much.
                        if (msc_dptr >= msc_dlen) msc_state = 2;
                    end
                end else if (msc_state == 2) begin
                    for (i = 0; i < 13; i = i + 1) txb[i] = csw[i];
                    if (msc_bad_csw > 0) txb[0] = csw[0] ^ 8'hff;
                    tx_packet(msc_in_tgl ? PID_DATA1 : PID_DATA0, 13);
                    rx_packet;
                    if (rxpid == PID_ACK) begin
                        if (msc_bad_csw > 0) msc_bad_csw = msc_bad_csw - 1;
                        msc_in_tgl = ~msc_in_tgl;
                        msc_csws = msc_csws + 1;
                        msc_state = 0;
                    end
                end else begin
                    // Idle, or waiting for OUT data: nothing to send.
                    tx_packet(PID_NAK, 0);
                end
            end else
            // Endpoint 1 of a hub: the status change bitmap, bit n for
            // port n with any change pending. NAK while there is none;
            // a real hub keeps reporting until the changes are cleared.
            if (HUB && rxpid == PID_IN) begin
                turnaround;
                hub_bits = {3'b000, (hp_c_con | hp_c_ena | hp_c_rst), 1'b0};
                if (hub_bits == 8'h00) begin
                    tx_packet(PID_NAK, 0);
                end else begin
                    txb[0] = hub_bits;
                    tx_packet(ep1_toggle ? PID_DATA1 : PID_DATA0, 1);
                    rx_packet;
                    if (rxpid == PID_ACK) begin
                        ep1_toggle = ~ep1_toggle;
                        hub_reports = hub_reports + 1;
                    end else pkt_held = 1'b1;
                end
            end else
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

        end else if (ECM && rx_crc_ok && rxpid == PID_IN &&
                     (rxb[1][6:0] == dev_addr) &&
                     (rxb[1][7] == 1'b1) && (rxb[2][2:0] == 3'd1)) begin

            // Endpoint 3, ECM: notifications. NETWORK_CONNECTION, 8 bytes.
            turnaround;
            if (ecm_notify == 0) begin
                tx_packet(PID_NAK, 0);
            end else begin
                txb[0] = 8'ha1; txb[1] = 8'h00;
                txb[2] = (ecm_notify == 1) ? 8'h01 : 8'h00; txb[3] = 8'h00;
                txb[4] = 8'h00; txb[5] = 8'h00; txb[6] = 8'h00; txb[7] = 8'h00;
                tx_packet(ecm_int_tgl ? PID_DATA1 : PID_DATA0, 8);
                rx_packet;
                if (rxpid == PID_ACK) begin
                    ecm_int_tgl = ~ecm_int_tgl;
                    ecm_notify = 0;
                    ecm_notes = ecm_notes + 1;
                end else pkt_held = 1'b1;
            end

        end else if (ECM && rx_crc_ok && rxpid == PID_OUT &&
                     (rxb[1][6:0] == dev_addr) &&
                     (rxb[1][7] == 1'b0) && (rxb[2][2:0] == 3'd1)) begin

            // Endpoint 2, ECM: bulk OUT. A packet shorter than 64 ends
            // the frame; the frame is checked against ecm_pat.
            rx_packet;
            if (rx_crc_ok) begin
                turnaround;
                if (ecm_cfg != 8'd2 || ecm_alt != 8'd1) begin
                    ecm_bad_ep = ecm_bad_ep + 1;
                    tx_packet(PID_STALL, 0);
                end else if (ecm_out_naks > 0) begin
                    ecm_out_naks = ecm_out_naks - 1;
                    tx_packet(PID_NAK, 0);
                end else begin
                    tx_packet(PID_ACK, 0);
                    if ((rxpid == PID_DATA1) != ecm_out_tgl) begin
                        ecm_dup_out = ecm_dup_out + 1;
                    end else begin
                        ecm_out_tgl = ~ecm_out_tgl;
                        for (i = 0; i < rxn - 3; i = i + 1)
                            if (ecm_rn + i < 2048) ecm_rb[ecm_rn + i] = rxb[1 + i];
                        ecm_rn = ecm_rn + rxn - 3;
                        if (rxn - 3 < 64) begin
                            if (ecm_rn == 0) begin
                                ecm_rx_zlp_only = ecm_rx_zlp_only + 1;
                            end else begin
                                ecm_rx_frames = ecm_rx_frames + 1;
                                ecm_rx_last = ecm_rn;
                                for (ecm_q = 1; ecm_q < ecm_rn && ecm_q < 2048; ecm_q = ecm_q + 1)
                                    if (ecm_rb[ecm_q] !== ecm_pat(ecm_rb[0], ecm_q))
                                        ecm_rx_bad = ecm_rx_bad + 1;
                            end
                            ecm_rn = 0;
                        end
                    end
                end
            end

        end else if (MSC && rx_crc_ok && rxpid == PID_OUT &&
                     (rxb[1][6:0] == dev_addr) &&
                     (rxb[1][7] == 1'b0) && (rxb[2][2:0] == 3'd1)) begin

            // Endpoint 2, mass storage: bulk OUT, a CBW or write data.
            rx_packet;
            if (rx_crc_ok && msc_halt_out) begin
                turnaround;
                tx_packet(PID_STALL, 0);
            end else if (rx_crc_ok) begin
                turnaround;
                tx_packet(PID_ACK, 0);
                if ((rxpid == PID_DATA1) != msc_out_tgl) begin
                    // A retransmission of a packet already taken. ACK
                    // it and drop it, as the spec says.
                    msc_dup_out = msc_dup_out + 1;
                end else begin
                    msc_out_tgl = ~msc_out_tgl;
                    if (msc_state == 3) begin
                        for (i = 0; i < rxn - 3; i = i + 1)
                            if (msc_base >= 0 && msc_dptr + i < msc_dlen)
                                disk[msc_base + msc_dptr + i] = rxb[1 + i];
                        msc_dptr = msc_dptr + rxn - 3;
                        if (msc_dptr >= msc_dlen) msc_state = 2;
                    end else begin
                        msc_cbw;
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
                    if (HUB && (setup[0] & 8'h60) == 8'h20) begin
                        hub_request;
                    end else if (setup[1] == 8'h06) begin
                        in_len = {setup[7], setup[6]};
                        // wValue's high byte is the descriptor TYPE.
                        if (ECM && setup[3] == 8'h02 && setup[2] == 8'h01) begin
                            if (in_len > cfge_total) in_len = cfge_total;
                            for (i = 0; i < in_len; i = i + 1)
                                src[i] = cfge[i];
                        end else if (setup[3] == 8'h02) begin
                            if (in_len > cfg_total) in_len = cfg_total;
                            for (i = 0; i < in_len; i = i + 1)
                                src[i] = cfgd[i];
                        end else if (ECM && setup[3] == 8'h03) begin
                            // string 3: the MAC, "00E04C36026B" in
                            // UTF-16LE; anything else: language 0409
                            src[0] = 8'd26; src[1] = 8'h03;
                            for (i = 0; i < 12; i = i + 1) begin
                                src[2 + i * 2] = "0";
                                src[3 + i * 2] = 8'h00;
                            end
                            src[4] = "0"; src[6] = "E"; src[8] = "0";
                            src[10] = "4"; src[12] = "C"; src[14] = "3";
                            src[16] = "6"; src[18] = "0"; src[20] = "2";
                            src[22] = "6"; src[24] = "B";
                            if (setup[2] != 8'h03) begin
                                src[0] = 8'd4;
                                src[2] = 8'h09; src[3] = 8'h04;
                            end
                            if (in_len > src[0]) in_len = src[0];
                        end else begin
                            if (in_len > 18) in_len = 18;
                            for (i = 0; i < in_len; i = i + 1)
                                src[i] = desc[i];
                        end
                    end else if (setup[1] == 8'h05) begin
                        pending_addr = setup[2][6:0];
                        in_len = 0;
                    end else if (setup[0] == 8'h02 && setup[1] == 8'h01) begin
                        // CLEAR_FEATURE(ENDPOINT_HALT): clear the halt
                        // and reset the endpoint's toggle to DATA0.
                        msc_clears = msc_clears + 1;
                        if (setup[4] == 8'h81) begin
                            msc_halt_in = 1'b0;
                            msc_in_tgl = 1'b0;
                        end
                        if (setup[4] == 8'h02) begin
                            msc_halt_out = 1'b0;
                            msc_out_tgl = 1'b0;
                        end
                        in_len = 0;
                    end else if (ECM && setup[0] == 8'h00 &&
                                 setup[1] == 8'h09) begin
                        ecm_cfg = setup[2];
                        ecm_alt = 8'd0;
                        in_len = 0;
                    end else if (ECM && setup[0] == 8'h01 &&
                                 setup[1] == 8'h0b) begin
                        // SET_INTERFACE resets the interface's endpoints
                        // to DATA0 (USB 2.0 9.1.1.5)
                        if (setup[4] == 8'h01) ecm_alt = setup[2];
                        ecm_in_tgl = 1'b0;
                        ecm_out_tgl = 1'b0;
                        in_len = 0;
                    end else if (ECM && setup[0] == 8'h21 &&
                                 setup[1] == 8'h43) begin
                        ecm_filter = setup[2];
                        ecm_filters = ecm_filters + 1;
                        in_len = 0;
                    end else if (MSC && setup[0] == 8'h21 &&
                                 setup[1] == 8'hff) begin
                        // Bulk-Only Mass Storage Reset, BOT 3.1: back
                        // to waiting for a CBW. Toggles and halts are
                        // NOT touched; the host clears those next.
                        msc_resets = msc_resets + 1;
                        if (msc_state != 0) msc_aborted = msc_aborted + 1;
                        msc_state = 0;
                        msc_nak_left = 0;
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
                end else if (stall_set_addr && setup[1] == 8'h05) begin
                    tx_packet(PID_STALL, 0);
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
