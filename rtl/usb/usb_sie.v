/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB serial interface engine (SIE) -- the bit level.
 *
 * Written from the USB 2.0 specification, chapter 7 (electrical) and
 * chapter 8 (protocol). No code from rtl/ext/usb_hid_host or
 * rtl/ext/usb_cdc -- see docs/usb_host.md's clean-room statement.
 *
 * This module knows about packets and nothing above them. It has no
 * idea what a transaction, a device or an endpoint is; usb_xact.v owns
 * that. What lives here:
 *
 *   TX   SYNC, PID, payload, CRC5 or CRC16, bit stuffing, NRZI, EOP
 *   RX   sync hunt, NRZI decode, bit unstuffing, PID check, CRC16
 *        residue check, EOP detect
 *   both a DPLL that re-centres on every line transition
 *
 * -- one clock domain --
 *
 * Everything runs on clk, which is sys_clk, which rtl/sysctl.v defines
 * as 48 MHz. That is exactly 4x the full-speed bit rate, and it is
 * also the Wishbone clock, so this core has NO clock-domain crossing
 * anywhere. Worth stating because the thing being replaced does:
 * rtl/usb_hid.v is largely synchronisers, because usb_hid_host runs at
 * 12 MHz. None of that machinery is needed here.
 *
 * -- speeds, and why lowspeed and inverted are separate inputs --
 *
 * cfg_ls picks the BIT RATE (4 clocks/bit full speed, 32 low speed).
 * cfg_inv picks the LINE POLARITY. They are deliberately not one
 * two-bit "speed" field, because the three cases do not line up:
 *
 *   FS device, direct         rate FS,  polarity normal
 *   LS device, direct         rate LS,  polarity INVERTED (the device
 *                                       pulls up D-, so idle J is
 *                                       D- high)
 *   LS device behind a hub    rate LS,  polarity NORMAL
 *
 * The third case is the one that catches people. A low-speed device
 * behind a full-speed hub sits on a bus segment the HUB drives, and
 * the hub is full-speed facing us. So the packet leaves this port with
 * full-speed polarity at the low-speed bit rate -- the one combination
 * that looks wrong and is right. Fold polarity into speed and
 * direct-attach LS works perfectly while LS-behind-a-hub silently
 * never does, which is a long week. See docs/usb_host.md.
 *
 * -- PRE --
 *
 * cfg_pre prefixes the packet with a PREamble: SYNC + PRE PID at FULL
 * speed, then the bus held idle for the hub setup interval, then the
 * real packet at the low-speed rate. A PRE packet has NO EOP -- not an
 * omission here; the spec has the host drive the bus to idle instead,
 * and the hub uses that gap to switch its downstream ports over.
 *
 * Every host-issued packet of a low-speed transaction needs its own
 * PRE, so cfg_pre stays asserted for the whole transaction and this
 * module re-sends the preamble on each tx_go. The DEVICE's reply
 * carries no PRE, which is why rate_ls stays low-speed after a
 * preamble transmit rather than reverting to full speed.
 *
 * -- CRC bit order --
 *
 * Both CRCs are reflected LFSRs fed LSB-first, initialised to all
 * ones, and transmitted as the COMPLEMENTED residual, LSB first. That
 * is not a guess: it reproduces the token bytes every USB capture
 * shows (addr 0 endp 0 -> 00 10, addr 1 endp 0 -> 01 E8) and the
 * classic GET_DESCRIPTOR(DEVICE) SETUP payload CRC of DD 94.
 *
 * On receive the CRC runs over the data field AND the received CRC
 * bytes, and a good packet leaves a constant residue -- 16'hB001 here,
 * verified against several payloads. Checking a residue rather than
 * recomputing and comparing costs nothing extra and needs no
 * buffering.
 */

module usb_sie #(
    // Clocks per bit. At 48 MHz: full speed is 12 Mbps (4), low speed
    // is 1.5 Mbps (32). Parameters rather than literals so a board at
    // a different sys_clk can be supported without editing the body.
    parameter FS_DIV = 6'd4,
    parameter LS_DIV = 6'd32
) (
    input wire clk,
    input wire rst,

    // -- line. Synchronisation happens here, not upstream. --
    input wire dp_i,
    input wire dm_i,
    output wire dp_o,
    output wire dm_o,
    output wire oe_o,

    // -- per-transaction configuration, stable while busy --
    input wire cfg_ls,
    input wire cfg_inv,
    input wire cfg_pre,

    // -- transmit --
    // tx_len is payload BYTES after the PID. tx_crc_mode: 0 none
    // (handshake), 1 CRC5 over an 11-bit token field, 2 CRC16 over
    // tx_len bytes.
    input wire tx_go,
    input wire [3:0] tx_pid,
    input wire [10:0] tx_len,
    input wire [1:0] tx_crc_mode,
    output reg [10:0] tx_idx,
    input wire [7:0] tx_byte,
    output reg tx_done,
    output reg tx_active,

    // -- receive --
    input wire rx_en,
    output reg rx_active,
    output reg [3:0] rx_pid,
    output reg rx_pid_err,
    output reg [7:0] rx_byte,
    output reg rx_byte_valid,
    output reg rx_done,
    output reg rx_crc_ok,
    output reg rx_err,

    // -- bus observation, for usb_port.v --
    output wire [1:0] line_state,
    output wire se0
);

    localparam PID_PRE = 4'b1100;
    localparam PID_DATA0 = 4'b0011;
    localparam PID_DATA1 = 4'b1011;

    localparam CRC16_RESIDUE = 16'hB001;

    // -- TX byte-level states --
    localparam TB_IDLE    = 4'd0;
    localparam TB_PRESYNC = 4'd1;
    localparam TB_PREPID  = 4'd2;
    localparam TB_GAP     = 4'd3;
    localparam TB_SYNC    = 4'd4;
    localparam TB_PID     = 4'd5;
    localparam TB_DATA    = 4'd6;
    localparam TB_CRC     = 4'd7;
    localparam TB_EOP     = 4'd8;
    localparam TB_EOPJ    = 4'd9;

    // -- RX states --
    localparam RB_IDLE = 3'd0;
    localparam RB_SYNC = 3'd1;
    localparam RB_PID  = 3'd2;
    localparam RB_DATA = 3'd3;

    reg [5:0] phase;
    reg rate_ls;

    reg dp_s0, dp_s1, dm_s0, dm_s1;
    reg [1:0] ls_prev;

    reg tx_j;
    reg tx_se0;
    reg tx_oe;

    reg [3:0] tbs;
    reg [7:0] tx_sh;
    reg [2:0] tx_bitc;
    reg [2:0] tx_ones;
    reg [13:0] tx_bits_left;
    reg [4:0] crc5r;
    reg [15:0] crc16r;
    reg [4:0] tx_crc_left;
    reg [3:0] tx_gapc;
    reg [1:0] tx_eopc;
    reg [3:0] tx_pid_r;
    reg [10:0] tx_len_r;
    reg [1:0] tx_crc_r;

    reg [2:0] rbs;
    reg rx_prev_lvl;
    reg [2:0] rx_ones;
    reg [7:0] rx_sh;
    reg [3:0] rx_bitc;
    reg [7:0] rx_p0, rx_p1;
    reg [1:0] rx_pcnt;
    reg rx_isdata;
    reg [15:0] crc16rx;

    // ---------------------------------------------------------------
    // line state
    // ---------------------------------------------------------------
    //
    // Two flops against metastability. ls_c is the polarity-corrected
    // view: past this point the module speaks in J and K and never in
    // D+ and D-, which is what keeps the inverted case a one-line
    // concern rather than a pervasive one.

    wire [1:0] ls_raw = {dp_s1, dm_s1};
    wire [1:0] ls_c = cfg_inv ? {ls_raw[0], ls_raw[1]} : ls_raw;

    assign line_state = ls_c;
    assign se0 = (ls_c == 2'b00);

    wire rx_is_j = (ls_c == 2'b10);
    wire rx_is_k = (ls_c == 2'b01);
    wire ls_changed = (ls_c != ls_prev);

    always @(posedge clk) begin
        dp_s0 <= dp_i;
        dp_s1 <= dp_s0;
        dm_s0 <= dm_i;
        dm_s1 <= dm_s0;
        ls_prev <= ls_c;
    end

    // ---------------------------------------------------------------
    // bit clock / DPLL
    // ---------------------------------------------------------------
    //
    // While transmitting, phase free-runs -- we are the timing
    // reference. While not, any line transition resets it to zero, so
    // the sample point stays centred in the bit cell no matter how the
    // device's clock drifts against ours.
    //
    // Bit stuffing guarantees a transition at least every 7 bits and a
    // device's clock is specified to +-0.25%, so worst-case drift
    // between re-syncs is well under half a bit at 4x oversampling.
    // That is the entire argument for 48 MHz being enough to receive
    // full speed.

    wire [5:0] per = rate_ls ? (LS_DIV - 6'd1) : (FS_DIV - 6'd1);
    wire [5:0] mid = rate_ls ? (LS_DIV >> 1) : (FS_DIV >> 1);

    wire tx_tick = tx_active && (phase == per);
    wire rx_tick = !tx_active && (phase == mid);

    always @(posedge clk) begin
        if (rst) phase <= 6'd0;
        else if (!tx_active && ls_changed) phase <= 6'd0;
        else if (phase >= per) phase <= 6'd0;
        else phase <= phase + 6'd1;
    end

    // ---------------------------------------------------------------
    // transmit
    // ---------------------------------------------------------------

    // -- the drivers are REGISTERED, and that is not cosmetic --
    //
    // drv_c is combinational from two flops that change on the same
    // edge: ending an EOP clears tx_se0 and sets tx_j together. Let
    // that reach the pins directly and the pair is briefly read with
    // one updated and one not, so the line passes through K on its way
    // from SE0 to J. In hardware that is a runt pulse of a few hundred
    // picoseconds; in simulation it is a zero-width K that a receiver
    // watching for a packet start will happily latch onto, and then
    // decode the following idle as data because an idle line has no
    // transitions and therefore looks like a run of ones.
    //
    // One flop removes it. The whole transmit waveform shifts by a
    // single clock, uniformly, so every bit width and the EOP are
    // unchanged -- and the pins now change once per clock by
    // construction.
    wire [1:0] drv_c = tx_se0 ? 2'b00 : (tx_j ? 2'b10 : 2'b01);
    wire [1:0] drv = cfg_inv ? {drv_c[0], drv_c[1]} : drv_c;

    reg [1:0] drv_q;
    reg oe_q;

    always @(posedge clk) begin
        if (rst) begin
            drv_q <= 2'b10;
            oe_q <= 1'b0;
        end else begin
            drv_q <= drv;
            oe_q <= tx_oe;
        end
    end

    assign dp_o = drv_q[1];
    assign dm_o = drv_q[0];
    assign oe_o = oe_q;

    // Which states shift a bit out. The byte-level machine below only
    // has to decide where the bit comes from; stuffing and NRZI are
    // common to all of them.
    wire tx_in_shift = (tbs == TB_PRESYNC) || (tbs == TB_PREPID) ||
                       (tbs == TB_SYNC) || (tbs == TB_PID) ||
                       (tbs == TB_DATA);
    wire tx_shifting = tx_in_shift || (tbs == TB_CRC);

    wire tx_crcbit = (tx_crc_r == 2'd1) ? ~crc5r[0] : ~crc16r[0];
    wire tx_databit = (tbs == TB_CRC) ? tx_crcbit : tx_sh[0];

    // Stuffing runs over every field from SYNC onward, CRC included.
    // After six consecutive ones a zero is inserted and no data bit is
    // consumed on that tick.
    wire tx_stuff = tx_shifting && (tx_ones == 3'd6);
    wire tx_bit = tx_stuff ? 1'b0 : tx_databit;

    // The CRC sees the DATA field only, and only real data bits --
    // never stuff bits, never the CRC's own bits on the way out.
    wire tx_crc_feed = (tbs == TB_DATA) && !tx_stuff;

    // Payload bit count for this packet. A token is 11 bits, not two
    // bytes: CRC5 covers the address/endpoint field only.
    wire [13:0] tx_payload_bits =
        (tx_crc_r == 2'd1) ? 14'd11 : {1'b0, tx_len_r, 3'b000};

    always @(posedge clk) begin

        tx_done <= 1'b0;

        if (rst) begin

            tbs <= TB_IDLE;
            tx_active <= 1'b0;
            tx_oe <= 1'b0;
            tx_se0 <= 1'b0;
            tx_j <= 1'b1;
            tx_idx <= 11'd0;
            rate_ls <= 1'b0;

        end else if (tbs == TB_IDLE) begin

            tx_oe <= 1'b0;
            tx_se0 <= 1'b0;

            if (tx_go) begin
                tx_pid_r <= tx_pid;
                tx_len_r <= tx_len;
                tx_crc_r <= tx_crc_mode;
                tx_idx <= 11'd0;
                tx_active <= 1'b1;
                tx_oe <= 1'b1;
                tx_j <= 1'b1;
                tx_se0 <= 1'b0;
                tx_ones <= 3'd0;
                tx_bitc <= 3'd0;
                tx_sh <= 8'h80;
                crc5r <= 5'h1f;
                crc16r <= 16'hffff;
                // A preamble goes out at FULL speed whatever the
                // device's own speed is -- that is the point of it.
                // rate_ls only becomes cfg_ls after the gap.
                rate_ls <= cfg_pre ? 1'b0 : cfg_ls;
                tbs <= cfg_pre ? TB_PRESYNC : TB_SYNC;
`ifdef USB_TRACE
                $display("[sie %0t] tx pid=%b len=%0d crc=%0d", $time,
                         tx_pid, tx_len, tx_crc_mode);
`endif
            end

        end else if (tx_tick) begin

            // -- the bit emitter, common to every shifting state --
            if (tx_shifting) begin
                if (!tx_bit) tx_j <= ~tx_j;
                if (tx_bit) tx_ones <= tx_ones + 3'd1;
                else tx_ones <= 3'd0;
                if (tx_crc_feed) begin
                    if (tx_crc_r == 2'd1)
                        crc5r <= (crc5r >> 1) ^
                            ((tx_databit ^ crc5r[0]) ? 5'h14 : 5'h00);
                    else
                        crc16r <= (crc16r >> 1) ^
                            ((tx_databit ^ crc16r[0]) ? 16'hA001 : 16'h0000);
                end
            end

            if (!tx_stuff) begin

                case (tbs)

                TB_PRESYNC: begin
                    tx_sh <= {1'b0, tx_sh[7:1]};
                    tx_bitc <= tx_bitc + 3'd1;
                    if (tx_bitc == 3'd7) begin
                        tx_sh <= {~PID_PRE, PID_PRE};
                        tbs <= TB_PREPID;
                    end
                end

                TB_PREPID: begin
                    tx_sh <= {1'b0, tx_sh[7:1]};
                    tx_bitc <= tx_bitc + 3'd1;
                    if (tx_bitc == 3'd7) begin
                        // No EOP after a preamble. Hold idle J while
                        // the hub reconfigures its downstream ports.
                        tx_j <= 1'b1;
                        tx_gapc <= 4'd0;
                        tbs <= TB_GAP;
                    end
                end

                TB_GAP: begin
                    tx_j <= 1'b1;
                    tx_gapc <= tx_gapc + 4'd1;
                    if (tx_gapc == 4'd3) begin
                        rate_ls <= cfg_ls;
                        tx_ones <= 3'd0;
                        tx_bitc <= 3'd0;
                        tx_sh <= 8'h80;
                        tbs <= TB_SYNC;
                    end
                end

                TB_SYNC: begin
                    tx_sh <= {1'b0, tx_sh[7:1]};
                    tx_bitc <= tx_bitc + 3'd1;
                    if (tx_bitc == 3'd7) begin
                        tx_sh <= {~tx_pid_r, tx_pid_r};
                        tbs <= TB_PID;
                    end
                end

                TB_PID: begin
                    tx_sh <= {1'b0, tx_sh[7:1]};
                    tx_bitc <= tx_bitc + 3'd1;
                    if (tx_bitc == 3'd7) begin
                        tx_crc_left <= (tx_crc_r == 2'd1) ? 5'd5 : 5'd16;
                        if (tx_crc_r == 2'd0) begin
                            // Handshake: PID then EOP, nothing else.
                            tx_eopc <= 2'd0;
                            tbs <= TB_EOP;
                        end else begin
                            // tx_byte is the registered read of
                            // tx_idx, which has been 0 since tx_go --
                            // many bit times ago, so it has settled.
                            tx_sh <= tx_byte;
                            tx_idx <= 11'd1;
                            tx_bits_left <= tx_payload_bits;
                            // A zero-length data packet still has a
                            // CRC16, which is why this is not simply
                            // TB_DATA.
                            tbs <= (tx_payload_bits == 14'd0) ?
                                TB_CRC : TB_DATA;
                        end
                    end
                end

                TB_DATA: begin
                    tx_sh <= {1'b0, tx_sh[7:1]};
                    tx_bitc <= tx_bitc + 3'd1;
                    tx_bits_left <= tx_bits_left - 14'd1;
                    if (tx_bitc == 3'd7) begin
                        tx_sh <= tx_byte;
                        tx_idx <= tx_idx + 11'd1;
                    end
                    if (tx_bits_left == 14'd1) tbs <= TB_CRC;
                end

                TB_CRC: begin
                    if (tx_crc_r == 2'd1) crc5r <= {1'b0, crc5r[4:1]};
                    else crc16r <= {1'b0, crc16r[15:1]};
                    tx_crc_left <= tx_crc_left - 5'd1;
                    if (tx_crc_left == 5'd1) begin
                        tx_eopc <= 2'd0;
                        tbs <= TB_EOP;
                    end
                end

                // -- why SE0 is asserted HERE and not on entry --
                //
                // A bit's line state is established by the tick that
                // computes it and lasts until the NEXT tick. Setting
                // tx_se0 on the same edge that emitted the final CRC
                // bit therefore overwrote that bit before it was ever
                // driven: every packet went out exactly one bit short,
                // the receiver's last byte came up seven bits long and
                // was discarded as a partial, and what arrived looked
                // like a packet whose final CRC byte had simply gone
                // missing. Asserting SE0 on the first tick INSIDE this
                // state gives the last bit its full bit time.
                //
                // Three ticks here, not two: entry (drive SE0), one
                // more, then release. That is two bit times of SE0
                // measured on the wire, which is what an EOP is.
                TB_EOP: begin
                    tx_se0 <= 1'b1;
                    tx_eopc <= tx_eopc + 2'd1;
                    if (tx_eopc == 2'd2) begin
                        tx_se0 <= 1'b0;
                        tx_j <= 1'b1;
                        tbs <= TB_EOPJ;
                    end
                end

                TB_EOPJ: begin
                    tx_oe <= 1'b0;
                    tx_active <= 1'b0;
                    tx_done <= 1'b1;
                    tbs <= TB_IDLE;
                end

                default: tbs <= TB_IDLE;

                endcase

            end

        end

    end

    // ---------------------------------------------------------------
    // receive
    // ---------------------------------------------------------------
    //
    // NRZI: a transition is a zero, no transition is a one.
    //
    // Byte assembly note, because it is easy to get backwards. Bits
    // arrive LSB first and shift in at the MSB end, so after eight
    // shifts the FIRST bit sits at position 0. On the eighth bit the
    // register still holds seven bits at [7:1] and the complete byte
    // is {rx_dbit, rx_sh[7:1]} -- which makes the PID rx_sh[4:1] and
    // its check nibble {rx_dbit, rx_sh[7:5]}, not the other way round.

    wire rx_lvl = rx_is_j;
    wire rx_dbit = (rx_lvl == rx_prev_lvl);
    wire rx_unstuff = (rx_ones == 3'd6);

    wire [3:0] rx_pid_now = rx_sh[4:1];
    wire [3:0] rx_pid_chk = {rx_dbit, rx_sh[7:5]};
    wire rx_pid_isdata = (rx_pid_now == PID_DATA0) ||
                         (rx_pid_now == PID_DATA1);

    always @(posedge clk) begin

        rx_byte_valid <= 1'b0;
        rx_done <= 1'b0;

        if (rst) begin

            rbs <= RB_IDLE;
            rx_active <= 1'b0;
            rx_err <= 1'b0;
            rx_crc_ok <= 1'b0;
            rx_pid_err <= 1'b0;

        end else if (rbs == RB_IDLE) begin

            rx_active <= 1'b0;
            // A packet begins when the bus leaves idle J for K. We are
            // not transmitting, so the DPLL above has just reset phase
            // on that same transition and the first sample will land
            // in the middle of sync bit 0.
            if (rx_en && !tx_active && rx_is_k) begin
                rbs <= RB_SYNC;
                rx_active <= 1'b1;
                rx_prev_lvl <= 1'b1;
                rx_ones <= 3'd0;
                rx_bitc <= 4'd0;
                rx_err <= 1'b0;
                rx_crc_ok <= 1'b0;
                rx_pid_err <= 1'b0;
                rx_pcnt <= 2'd0;
                rx_isdata <= 1'b0;
                crc16rx <= 16'hffff;
            end

        end else if (rx_tick) begin

            rx_prev_lvl <= rx_lvl;

            if (se0) begin

                // EOP. For a data packet the CRC has been folded in
                // along with everything else, so a good packet is a
                // residue compare -- and the two bytes still sitting
                // in the pipeline below are that CRC, correctly never
                // emitted as payload.
                rx_done <= 1'b1;
                rx_crc_ok <= rx_isdata ? (crc16rx == CRC16_RESIDUE) : 1'b1;
`ifdef USB_TRACE
                $display("[sie %0t] rx pid=%b piderr=%b crcok=%b", $time,
                         rx_pid, rx_pid_err,
                         rx_isdata ? (crc16rx == CRC16_RESIDUE) : 1'b1);
`endif
                rx_active <= 1'b0;
                rbs <= RB_IDLE;

            end else if (rx_unstuff) begin

                // The stuffed bit. It must be a zero; a one here means
                // the bit stream is corrupt.
                //
                // It is NOT fed to the CRC. Stuffing happens after the
                // CRC is computed and is undone before it is checked,
                // so the residue is taken over the unstuffed stream --
                // exactly as the transmit path excludes its own stuff
                // bits. Feeding it here makes every packet that
                // happens to contain a run of six ones fail its CRC,
                // and leaves every packet that does not pass, which
                // reads as an intermittent fault rather than a
                // systematic one.
                rx_ones <= 3'd0;
                if (rx_dbit) rx_err <= 1'b1;

            end else begin

                if (rx_dbit) rx_ones <= rx_ones + 3'd1;
                else rx_ones <= 3'd0;

                case (rbs)

                RB_SYNC: begin
                    // Sync is 0000000 1. Hunting for the terminating
                    // one rather than matching all eight bits is
                    // deliberate: hubs are permitted to eat leading
                    // sync bits, so a strict match would reject
                    // packets that are perfectly legal.
                    if (rx_dbit) begin
                        rbs <= RB_PID;
                        rx_bitc <= 4'd0;
                    end
                end

                RB_PID: begin
                    rx_sh <= {rx_dbit, rx_sh[7:1]};
                    rx_bitc <= rx_bitc + 4'd1;
                    if (rx_bitc == 4'd7) begin
                        rx_pid <= rx_pid_now;
                        rx_pid_err <= (rx_pid_chk != ~rx_pid_now);
                        // Only DATA0/DATA1 carry a payload and a
                        // CRC16; handshakes are PID and then EOP.
                        if (rx_pid_isdata) begin
                            rx_isdata <= 1'b1;
                            crc16rx <= 16'hffff;
                            rx_bitc <= 4'd0;
                            rbs <= RB_DATA;
                        end
                    end
                end

                RB_DATA: begin
                    rx_sh <= {rx_dbit, rx_sh[7:1]};
                    crc16rx <= (crc16rx >> 1) ^
                        ((rx_dbit ^ crc16rx[0]) ? 16'hA001 : 16'h0000);
                    rx_bitc <= rx_bitc + 4'd1;
                    if (rx_bitc == 4'd7) begin
                        rx_bitc <= 4'd0;
                        // Two-byte delay line. The last two bytes of
                        // any data packet are its CRC, and we do not
                        // know a byte was the last one until EOP --
                        // so hold two back and let EOP discard them.
                        if (rx_pcnt == 2'd2) begin
                            rx_byte <= rx_p1;
                            rx_byte_valid <= 1'b1;
                        end else begin
                            rx_pcnt <= rx_pcnt + 2'd1;
                        end
                        rx_p1 <= rx_p0;
                        rx_p0 <= {rx_dbit, rx_sh[7:1]};
                    end
                end

                default: rbs <= RB_IDLE;

                endcase

            end

        end

    end

endmodule
