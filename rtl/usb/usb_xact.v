/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB transaction engine.
 *
 * Written from the USB 2.0 specification, chapter 8. Clean-room; see
 * docs/usb_host.md.
 *
 * One transaction is a token, optionally a data packet, and a
 * handshake. This module sequences those against usb_sie.v and reports
 * one status. It knows about endpoints and toggles; it knows nothing
 * about descriptors, device classes or what any of the bytes mean --
 * that is software, deliberately, and it is what lets a new device
 * class be a .c file rather than a bitstream.
 *
 *   SETUP/OUT    host: token, host: DATA0/1+payload, device: handshake
 *   IN           host: token, device: DATA0/1+payload, host: ACK
 *
 * -- auto-continue --
 *
 * With auto_cont set, a request longer than one max-packet-size is
 * split into as many transactions as it takes, incrementing the buffer
 * pointer and flipping the data toggle in hardware, and the CPU is
 * interrupted once at the end instead of once per packet.
 *
 * This is not a micro-optimisation, it is what makes mass storage
 * usable. A 512-byte sector is eight 64-byte full-speed packets. One
 * interrupt per packet on a 48 MHz PicoRV32 caps throughput around
 * 150-300 KB/s and spends most of the CPU doing it; one interrupt per
 * sector puts the ceiling near the wire and leaves the CPU free. See
 * docs/usb_host.md's throughput section.
 *
 * -- short packets are a success, not a failure --
 *
 * A bulk IN that returns fewer bytes than asked for is how a device
 * says "that is all there is", and it terminates an auto-continue
 * sequence normally. It gets its own status code rather than being
 * folded into OK because the mass-storage driver has to be able to
 * tell a short read from a complete one, and rather than being folded
 * into the error codes because it is not an error.
 */

module usb_xact (
    input wire clk,
    input wire rst,

    // -- SIE, transmit --
    output reg sie_tx_go,
    output reg [3:0] sie_tx_pid,
    output reg [10:0] sie_tx_len,
    output reg [1:0] sie_tx_crc_mode,
    input wire [10:0] sie_tx_idx,
    output wire [7:0] sie_tx_byte,
    input wire sie_tx_done,
    input wire sie_tx_active,

    // -- SIE, receive --
    output reg sie_rx_en,
    input wire sie_rx_active,
    input wire [3:0] sie_rx_pid,
    input wire sie_rx_pid_err,
    input wire [7:0] sie_rx_byte,
    input wire sie_rx_byte_valid,
    input wire sie_rx_done,
    input wire sie_rx_crc_ok,
    input wire sie_rx_err,
    input wire sie_se0,

    // The selected port has lost its device. Asserted by usb_host.v
    // from the port's own debounced connect status.
    input wire port_gone,

    // -- SIE, per-transaction configuration --
    output wire sie_cfg_ls,
    output wire sie_cfg_inv,
    output wire sie_cfg_pre,

    // -- packet buffer, byte port --
    output wire [10:0] buf_adr,
    output reg buf_we,
    output reg [7:0] buf_wdat,
    input wire [7:0] buf_rdat,

    // -- request --
    input wire req_start,
    input wire [1:0] req_pid,        // 0 SETUP, 1 IN, 2 OUT
    input wire [6:0] req_addr,
    input wire [3:0] req_endp,
    input wire req_ls,
    input wire req_inv,
    input wire req_pre,
    input wire req_port,
    input wire req_toggle,
    input wire req_autocont,
    input wire [6:0] req_mps,
    input wire [10:0] req_off,
    input wire [10:0] req_len,
    input wire [3:0] req_nak_retry,
    // Too little of the frame left for a full- / low-speed transaction;
    // see the end-of-frame guard in usb_host.v.
    input wire late_fs,
    input wire late_ls,
    // Frames are running: a PRE'd request ends rather than re-issue.
    input wire pre_gate,
    // Low-speed timings in clocks, from usb_host.v's A_TUNE.
    input wire [15:0] ls_tmo,
    input wire [11:0] ls_turn,
    input wire [11:0] ls_gap,

    // -- result --
    output reg busy,
    output reg done,
    output reg [3:0] status,
    output reg [10:0] act_len,
    output reg res_toggle,
    output reg [7:0] nak_count,
    output wire port_sel
);

    localparam PID_OUT   = 4'b0001;
    localparam PID_IN    = 4'b1001;
    localparam PID_SOF   = 4'b0101;
    localparam PID_SETUP = 4'b1101;
    localparam PID_DATA0 = 4'b0011;
    localparam PID_DATA1 = 4'b1011;
    localparam PID_ACK   = 4'b0010;
    localparam PID_NAK   = 4'b1010;
    localparam PID_STALL = 4'b1110;

    localparam ST_OK      = 4'd0;
    localparam ST_NAK     = 4'd1;
    localparam ST_STALL   = 4'd2;
    localparam ST_TIMEOUT = 4'd3;
    localparam ST_CRCERR  = 4'd4;
    localparam ST_BABBLE  = 4'd5;
    localparam ST_SHORT   = 4'd6;
    localparam ST_ABORT   = 4'd7;

    localparam X_IDLE    = 4'd0;
    localparam X_TOK     = 4'd1;
    localparam X_TOK_W   = 4'd2;
    localparam X_OD      = 4'd3;
    localparam X_OD_W    = 4'd4;
    localparam X_HS_W    = 4'd5;
    localparam X_HS_RX   = 4'd6;
    localparam X_IN_W    = 4'd7;
    localparam X_IN_RX   = 4'd8;
    localparam X_TURN    = 4'd9;
    localparam X_ACK     = 4'd10;
    localparam X_ACK_W   = 4'd11;
    localparam X_EVAL    = 4'd12;
    localparam X_DONE    = 4'd13;
    localparam X_IPG     = 4'd14;
    localparam X_GAP     = 4'd15;

    reg [3:0] xs;

    // latched request
    reg [1:0] r_pid;
    reg [6:0] r_addr;
    reg [3:0] r_endp;
    reg r_ls, r_inv, r_pre, r_port;
    reg r_toggle, r_autocont;
    reg [6:0] r_mps;
    reg [10:0] r_off;
    reg [10:0] r_len;
    reg [3:0] r_nak;

    // running state across an auto-continue sequence
    reg [10:0] ptr;
    reg [10:0] remaining;
    reg [10:0] this_len;
    reg [10:0] rx_count;
    reg [3:0] nak_left;

    reg [10:0] buf_wadr;
    reg [15:0] tmo;
    reg [3:0] hs_pid;
    reg rx_bad;
    reg tok_phase;

    assign sie_cfg_ls = r_ls;
    assign sie_cfg_inv = r_inv;
    assign sie_cfg_pre = r_pre;
    assign port_sel = r_port;

    // -- token bytes --
    //
    // The 11-bit token field is {endp[3:0], addr[6:0]}, sent LSB first,
    // and usb_sie.v appends CRC5 over exactly those 11 bits. Byte 1
    // carries only three significant bits; the SIE stops after 11.
    wire [7:0] tok_b0 = {r_endp[0], r_addr};
    wire [7:0] tok_b1 = {5'b00000, r_endp[3:1]};

    // During a token the SIE's byte fetches come from here rather than
    // from the packet buffer -- a token is addressing, not payload,
    // and has no business occupying buffer space.
    assign sie_tx_byte = tok_phase ?
        ((sie_tx_idx == 11'd0) ? tok_b0 : tok_b1) : buf_rdat;

    // One byte port, shared. On transmit it follows the SIE's fetch
    // index; on receive it points at where the next byte lands. They
    // never overlap, because USB is half duplex.
    //
    // The receive address is REGISTERED alongside buf_we, and has to
    // be: buf_we and buf_wdat are non-blocking, so the write lands a
    // cycle after the byte arrives -- by which time rx_count has
    // already incremented, and a combinational ptr+rx_count would put
    // every byte one position too high and leave a hole at the start.
    assign buf_adr = (xs == X_IN_RX) ? buf_wadr : (ptr + sie_tx_idx);

    // Response timeout. The host gives up 18 bit times after its own
    // EOP, which is 72 clocks at full speed and 576 at low speed; the
    // margin here is generous because a hub adds propagation delay in
    // both directions and a preamble puts the reply at the low-speed
    // rate regardless of the hub's own speed.
    wire [15:0] tmo_limit = r_ls ? ls_tmo : 16'd200;

    // Guard for a reception that STARTS and never finishes.
    //
    // X_HS_RX and X_IN_RX used to wait on sie_rx_done with no way out.
    // The SIE ends a packet on SE0, so anything that makes rx_active
    // assert without a real packet behind it -- a glitch as the bus is
    // released, noise on an unterminated line -- left the engine
    // waiting forever, x_busy stuck high, and the driver polling a
    // pending bit that would never clear.
    //
    // Sized for the longest packet each speed can carry: a low-speed
    // 8-byte data packet is about 100 bits at 32 clocks each, and a
    // full-speed 64-byte one is about 600 bits at 4. Both fit inside
    // these with room to spare, so this can only fire on a packet that
    // is genuinely not arriving.
    wire [15:0] rx_limit = r_ls ? 16'd8000 : 16'd4000;

    // Turnaround before the host's ACK. The spec requires at least two
    // bit times of gap; a device that is still driving its EOP when we
    // start is a collision.
    // Five bit times. Four would do -- the device stops driving 2.5
    // bit times into its own EOP -- but the host has no deadline to
    // meet here and a collision is far more expensive than a clock.
    wire [11:0] turn_limit = r_ls ? ls_turn : 12'd20;

    // -- the gap BEFORE a token is set by what came before it too --
    //
    // X_GAP used turn_limit, the speed of the transaction ABOUT to
    // start. After a low-speed transaction that is not enough: the
    // device drives a full low-speed bit of J after its EOP (667 ns),
    // and behind a hub the hub is still repeating it upstream. A
    // full-speed token -- a SOF, or a request to the hub -- given only
    // five full-speed bits (417 ns) started on top of it.
    //
    // Before hubs this could not happen: a low-speed device had a root
    // port, and its wire, to itself. Behind a hub low- and full-speed
    // traffic share one segment. On hardware (hub 05e3:0608) hub
    // requests failed with CRC errors and timeouts, a low-speed
    // keyboard never enumerated, and the mouse stopped working once
    // the keyboard added traffic -- a SOF lost in the collision is a
    // keep-alive the hub never sends its low-speed ports. The hub
    // co-simulation showed the host and a device driving the wire at
    // once, a SOF straight after a low-speed ACK.
    //
    // So remember the last transaction's speed and port: after a
    // low-speed one, the next token on that port waits five LOW-speed
    // bits, whatever its own speed.
    reg last_ls;
    reg last_port;
    // The token about to go out is a re-issue inside this request -- a
    // retry or the next auto-continue packet -- rather than its first.
    // Only a re-issue is checked against the end of the frame here; the
    // first token was checked by the scheduler before it started.
    reg reissue;
    wire [11:0] gap_limit =
        (r_ls || (last_ls && last_port == r_port)) ? ls_gap : 12'd20;

    wire [10:0] chunk =
        (remaining > {4'd0, r_mps}) ? {4'd0, r_mps} : remaining;

    // -- data toggle check on IN, USB 2.0 8.6.4 --
    //
    // A device resends its last data packet when it misses the host's
    // ACK -- same payload, same DATA0/DATA1. The host has already taken
    // that packet, so the toggle is the only thing that tells the
    // resend from new data: it matches the toggle the host has moved
    // PAST, not the one it now expects. Such a packet is ACKed, so the
    // device moves on, and thrown away.
    //
    // This engine did not look, and took the resend as the next packet:
    // 64 bytes duplicated in the buffer, and the device one packet
    // behind the host for the rest of the transfer.
    //
    // in_dup_now is used while the packet arrives, to keep its bytes
    // out of the buffer; the SIE latches the PID before the first
    // payload byte. eval_dup is the same test on the handshake PID
    // kept in hs_pid, used once the packet has been ACKed.
    wire in_dup_now = (r_pid == 2'd1) &&
        (sie_rx_pid == PID_DATA0 || sie_rx_pid == PID_DATA1) &&
        ((sie_rx_pid == PID_DATA1) != r_toggle);
    wire eval_dup = (r_pid == 2'd1) &&
        (hs_pid == PID_DATA0 || hs_pid == PID_DATA1) &&
        ((hs_pid == PID_DATA1) != r_toggle);

    always @(posedge clk) begin

        sie_tx_go <= 1'b0;
        done <= 1'b0;
        buf_we <= 1'b0;

        if (rst) begin

            xs <= X_IDLE;
            busy <= 1'b0;
            status <= ST_OK;
            act_len <= 11'd0;
            last_ls <= 1'b0;
            last_port <= 1'b0;
            reissue <= 1'b0;
            nak_count <= 8'd0;
            sie_rx_en <= 1'b0;
            tok_phase <= 1'b0;

        end else if (port_gone && busy && xs != X_DONE) begin

            // The device went away mid-transaction. Abort rather than
            // sit out the response timeout: at low speed that is 1400
            // clocks of a bus the OTHER port could be using, and every
            // in-flight packet is addressed to something that is no
            // longer there. ST_ABORT tells software this was not the
            // device misbehaving.
            status <= ST_ABORT;
            sie_rx_en <= 1'b0;
            xs <= X_DONE;

        end else begin

            case (xs)

            X_IDLE: begin
                busy <= 1'b0;
                sie_rx_en <= 1'b0;
                tok_phase <= 1'b0;
                if (req_start) begin
                    r_pid <= req_pid;
                    r_addr <= req_addr;
                    r_endp <= req_endp;
                    r_ls <= req_ls;
                    r_inv <= req_inv;
                    r_pre <= req_pre;
                    r_port <= req_port;
                    r_toggle <= req_toggle;
                    r_autocont <= req_autocont;
                    r_mps <= req_mps;
                    r_off <= req_off;
                    r_len <= req_len;
                    r_nak <= req_nak_retry;
                    reissue <= 1'b0;
                    nak_left <= req_nak_retry;
                    ptr <= req_off;
                    remaining <= req_len;
                    act_len <= 11'd0;
                    nak_count <= 8'd0;
                    rx_bad <= 1'b0;
                    busy <= 1'b1;
                    tmo <= 12'd0;
                    xs <= X_GAP;
                end
            end

            // -- wait for the bus to actually be free --
            //
            // A transaction ends when the SIE samples SE0, which is
            // about a bit and a half into the other end's EOP. The
            // DEVICE keeps driving for roughly two and a half bit
            // times beyond that -- the rest of the SE0, then a J --
            // so a host that starts its next token the moment the
            // previous transaction reports done is transmitting into
            // a driver that is still on.
            //
            // At full speed the software round trip was long enough to
            // hide this. At low speed a bit is 667 ns, software is no
            // slower, and the collision is reliable: the device
            // decodes a mangled token, answers nothing, and the host
            // reports a timeout that looks like a dead device.
            //
            // Counting only while the line is not SE0 means this also
            // waits out the tail of that EOP rather than just counting
            // through it.
            //
            // EVERY token goes through here, re-issues included: a
            // retry after a NAK, after a discarded resend, and the next
            // packet of an auto-continue. Those used to jump straight
            // to X_TOK, and a retry after a NAK follows the DEVICE's
            // EOP by a few clocks -- exactly the collision described
            // above. Behind a hub it was fatal: a low-speed keyboard
            // that NAKed during enumeration had every retried PRE
            // mangled, the hub dropped it, and the retry timed out.
            // test_usb_hub reproduces it with the mouse NAKing.
            X_GAP: begin
                if (sie_se0) tmo <= 12'd0;
                else if (tmo >= gap_limit) begin
                    // Not enough of the frame left for another packet:
                    // stop here, as though NAKed, with what has been
                    // done kept in act_len. See usb_host.v.
                    // A PRE'd request (low speed behind a hub) never
                    // re-issues inside a frame either: one PRE'd
                    // transaction per frame, see usb_host.v, pre_done.
                    if (reissue && ((r_pre && pre_gate) ||
                                    (r_ls ? late_ls : late_fs))) begin
                        status <= ST_NAK;
                        xs <= X_DONE;
                    end else begin
                        xs <= X_TOK;
                    end
                end
                else tmo <= tmo + 12'd1;
            end

            X_TOK: begin
                // A zero-length request still sends one packet -- that
                // is a status stage, not nothing to do.
                this_len <= (remaining == 11'd0) ? 11'd0 : chunk;
                tok_phase <= 1'b1;
                // req_pid 3 is a start-of-frame. It reuses the token
                // path exactly: the 11-bit field that is normally
                // {endp, addr} carries the frame number instead, and
                // CRC5 covers it the same way. The scheduler in
                // usb_host.v splits the frame number across req_addr
                // and req_endp so that this needs no special case.
                sie_tx_pid <= (r_pid == 2'd0) ? PID_SETUP :
                              (r_pid == 2'd1) ? PID_IN :
                              (r_pid == 2'd3) ? PID_SOF : PID_OUT;
                sie_tx_len <= 11'd2;
                sie_tx_crc_mode <= 2'd1;
                sie_tx_go <= 1'b1;
                xs <= X_TOK_W;
            end

            X_TOK_W: begin
                if (sie_tx_done) begin
                    tok_phase <= 1'b0;
                    tmo <= 12'd0;
                    if (r_pid == 2'd3) begin
                        // A SOF is a token and nothing else. Nobody
                        // answers it and nobody is meant to.
                        status <= ST_OK;
                        xs <= X_DONE;
                    end else if (r_pid == 2'd1) begin
                        sie_rx_en <= 1'b1;
                        rx_count <= 11'd0;
                        xs <= X_IN_W;
                    end else begin
                        xs <= X_IPG;
                    end
                end
            end

            // Minimum gap between two packets the HOST sends
            // back-to-back -- the token and the data packet of a SETUP
            // or OUT. Without it the next SYNC follows the previous
            // EOP by two or three clocks, well under the two bit times
            // the spec requires, and a device is entitled to miss the
            // start of it.
            X_IPG: begin
                if (tmo >= turn_limit) xs <= X_OD;
                else tmo <= tmo + 12'd1;
            end

            // -- SETUP / OUT: send the data packet --

            X_OD: begin
                sie_tx_pid <= r_toggle ? PID_DATA1 : PID_DATA0;
                sie_tx_len <= this_len;
                sie_tx_crc_mode <= 2'd2;
                sie_tx_go <= 1'b1;
                xs <= X_OD_W;
            end

            X_OD_W: begin
                if (sie_tx_done) begin
                    sie_rx_en <= 1'b1;
                    tmo <= 12'd0;
                    xs <= X_HS_W;
                end
            end

            X_HS_W: begin
                if (sie_rx_active) begin
                    tmo <= 16'd0;
                    xs <= X_HS_RX;
                end else if (tmo >= tmo_limit) begin
                    sie_rx_en <= 1'b0;
                    status <= ST_TIMEOUT;
                    xs <= X_DONE;
                end else begin
                    tmo <= tmo + 16'd1;
                end
            end

            X_HS_RX: begin
                if (!sie_rx_done && tmo >= rx_limit) begin
                    sie_rx_en <= 1'b0;
                    status <= ST_TIMEOUT;
                    xs <= X_DONE;
                end else tmo <= tmo + 16'd1;
                if (sie_rx_done) begin
                    sie_rx_en <= 1'b0;
                    hs_pid <= sie_rx_pid;
                    if (sie_rx_pid_err) status <= ST_CRCERR;
                    else status <= ST_OK;
                    xs <= X_EVAL;
                end
            end

            // -- IN: receive the data packet --

            X_IN_W: begin
                if (sie_rx_active) begin
                    tmo <= 16'd0;
                    xs <= X_IN_RX;
                end else if (tmo >= tmo_limit) begin
                    sie_rx_en <= 1'b0;
                    status <= ST_TIMEOUT;
                    xs <= X_DONE;
                end else begin
                    tmo <= tmo + 16'd1;
                end
            end

            X_IN_RX: begin
                if (!sie_rx_done && tmo >= rx_limit) begin
                    sie_rx_en <= 1'b0;
                    status <= ST_TIMEOUT;
                    xs <= X_DONE;
                end else tmo <= tmo + 16'd1;
                // A resent packet's bytes go nowhere -- see in_dup_now.
                if (sie_rx_byte_valid && !in_dup_now) begin
                    // Babble: the device sent more than it was told it
                    // could. Take the packet no further -- writing
                    // past the caller's buffer would corrupt whatever
                    // is next in the packet buffer, and a device that
                    // does this is already misbehaving.
                    if (rx_count >= this_len) begin
                        rx_bad <= 1'b1;
                        status <= ST_BABBLE;
                    end else begin
                        buf_we <= 1'b1;
                        buf_wadr <= ptr + rx_count;
                        buf_wdat <= sie_rx_byte;
                        rx_count <= rx_count + 11'd1;
                    end
                end
                if (sie_rx_done) begin
                    sie_rx_en <= 1'b0;
                    hs_pid <= sie_rx_pid;
                    if (sie_rx_pid_err || sie_rx_err) begin
                        status <= ST_CRCERR;
                        xs <= X_EVAL;
                    end else if (sie_rx_pid == PID_NAK ||
                                 sie_rx_pid == PID_STALL) begin
                        // A device may answer an IN token with a
                        // handshake instead of data. That is not an
                        // error at this layer.
                        status <= ST_OK;
                        xs <= X_EVAL;
                    end else if (!sie_rx_crc_ok) begin
                        status <= ST_CRCERR;
                        xs <= X_EVAL;
                    end else if (rx_bad) begin
                        xs <= X_EVAL;
                    end else begin
                        status <= ST_OK;
                        tmo <= 12'd0;
                        xs <= X_TURN;
                    end
                end
            end

            X_TURN: begin
                if (tmo >= turn_limit) xs <= X_ACK;
                else tmo <= tmo + 12'd1;
            end

            X_ACK: begin
                sie_tx_pid <= PID_ACK;
                sie_tx_len <= 11'd0;
                sie_tx_crc_mode <= 2'd0;
                sie_tx_go <= 1'b1;
                xs <= X_ACK_W;
            end

            X_ACK_W: begin
                if (sie_tx_done) xs <= X_EVAL;
            end

            // -- decide what this transaction meant --

            X_EVAL: begin

                if (status != ST_OK) begin

                    xs <= X_DONE;

                end else if (r_pid != 2'd1 &&
                             (hs_pid == PID_NAK)) begin

                    // The device is not ready. Retry the SAME packet:
                    // pointer and toggle are untouched, because a NAK
                    // means the device did not accept the data and the
                    // toggle has not advanced on either side.
                    nak_count <= nak_count + 8'd1;
                    if (nak_left != 4'd0) begin
                        nak_left <= nak_left - 4'd1;
                        tmo <= 16'd0;
                        reissue <= 1'b1;
                        xs <= X_GAP;
                    end else begin
                        status <= ST_NAK;
                        xs <= X_DONE;
                    end

                end else if (r_pid == 2'd1 && hs_pid == PID_NAK) begin

                    nak_count <= nak_count + 8'd1;
                    if (nak_left != 4'd0) begin
                        nak_left <= nak_left - 4'd1;
                        tmo <= 16'd0;
                        reissue <= 1'b1;
                        xs <= X_GAP;
                    end else begin
                        status <= ST_NAK;
                        xs <= X_DONE;
                    end

                end else if (hs_pid == PID_STALL) begin

                    status <= ST_STALL;
                    xs <= X_DONE;

                end else if (eval_dup) begin

                    // A resend of the packet before, already ACKed and
                    // discarded. Nothing advances. Ask again, charged to
                    // the NAK budget: a device that never moves on must
                    // not hold the engine forever, and software treats
                    // the NAK that ends it exactly as it treats a busy
                    // device -- it resumes from act_len.
                    nak_count <= nak_count + 8'd1;
                    if (nak_left != 4'd0) begin
                        nak_left <= nak_left - 4'd1;
                        rx_count <= 11'd0;
                        rx_bad <= 1'b0;
                        tmo <= 16'd0;
                        reissue <= 1'b1;
                        xs <= X_GAP;
                    end else begin
                        status <= ST_NAK;
                        xs <= X_DONE;
                    end

                end else if (r_pid != 2'd1 && hs_pid != PID_ACK) begin

                    status <= ST_TIMEOUT;
                    xs <= X_DONE;

                end else begin

                    // Success. Advance.
                    act_len <= act_len +
                        ((r_pid == 2'd1) ? rx_count : this_len);
                    ptr <= ptr + ((r_pid == 2'd1) ? rx_count : this_len);
                    remaining <= remaining -
                        ((r_pid == 2'd1) ? rx_count : this_len);
                    r_toggle <= ~r_toggle;
                    nak_left <= r_nak;

                    if (r_pid == 2'd1 && (rx_count < {4'd0, r_mps})) begin
                        // Short packet. The device has no more to
                        // give; this ends the sequence and says so.
                        status <= ST_SHORT;
                        xs <= X_DONE;
                    end else if (!r_autocont) begin
                        xs <= X_DONE;
                    end else if ((remaining -
                            ((r_pid == 2'd1) ? rx_count : this_len))
                            == 11'd0) begin
                        xs <= X_DONE;
                    end else begin
                        rx_count <= 11'd0;
                        rx_bad <= 1'b0;
                        tmo <= 16'd0;
                        reissue <= 1'b1;
                        xs <= X_GAP;
                    end

                end

            end

            X_DONE: begin
                res_toggle <= r_toggle;
                last_ls <= r_ls;
                last_port <= r_port;
                busy <= 1'b0;
                done <= 1'b1;
                sie_rx_en <= 1'b0;
                xs <= X_IDLE;
            end

            default: xs <= X_IDLE;

            endcase

        end

    end

endmodule
