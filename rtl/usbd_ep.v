/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB device for the USB-C socket, with the device itself in software:
 * a composite of a CDC-ACM console and a CDC-ECM network interface
 * (sw/apps/usbd, sw/apps/net/usbd_eth.c, docs/usb_dev.md). `USB_DEV in
 * rtl/boards.vh; the alternative to `USB_CDC, which is a fixed
 * CDC-ACM device in hardware.
 *
 * -- What is here --
 *
 * ulixxe's SIE (rtl/ext/usb_cdc/sie.v, MIT) does the wire: NRZI, bit
 * stuffing, CRCs, PIDs, data toggles, ACK/NAK/STALL. In front of it:
 *
 *   Endpoints 0-3 and 5-7, served by the CPU (sw/apps/usbd, and
 *   sw/apps/share for the card reader): a 64-byte OUT buffer and a
 *   64-byte IN buffer each. Endpoint 0 is control; the others are bulk
 *   or interrupt as EPTYPE says. This is machboot's rtl/mb_usbd.v,
 *   which runs the same way on Lakritz, with more endpoints.
 *
 *   Endpoint 4, the ECM data endpoint, served by THIS MODULE: whole
 *   Ethernet frames. OUT packets from the host are gathered into one
 *   of two 1.5 KB receive slots until a short packet ends the frame;
 *   the frame then waits there for `net`, and `net_rx_o` is high while
 *   one does. For sending, `net` writes a frame into the 1.5 KB
 *   transmit slot and
 *   its length into NTXCTL; this module answers the host's IN tokens
 *   with it, 64 bytes at a time, and a zero-length packet after a
 *   frame that is a multiple of 64. So `net` handles one frame per
 *   interrupt rather than 24 packets in real time, as with the RMII
 *   MAC's slots (rtl/ethmac_rmii.v).
 *
 * The CPU endpoints are the `usbd` driver's; the frame endpoint and
 * the N* registers are `net`'s. Neither touches the other's: `usbd`
 * enables the frame endpoint (EPTYPE bit 24) when the host selects
 * the data interface's alternate setting 1, and `net` says whether it
 * is up (NCTL bit 0), which `usbd` sees as an event and reports to
 * the host as a NETWORK_CONNECTION notification.
 *
 * -- Registers (byte offsets in the 0x6200_0000 slot, 32-bit words) --
 *
 *   0x000  MAGIC    r   "UDEV" (0x56454455)
 *   0x004  CTRL     rw  bit 0 attach (pull-up on), bits 14:8 address
 *   0x008  EVENTS   r   bit 0 bus reset, bit 1 NCTL's link changed;
 *                   w   1s clear them
 *   0x00C  FRAME    r   the last SOF's frame number
 *   0x010  EPTYPE   rw  bits 7:0 IN bulk, 15:8 OUT bulk, 23:16 IN
 *                       interrupt (bit n endpoint n; 0 is control, 4
 *                       ignored); bit 24 the frame endpoint is enabled
 *   0x014  TOGGLE   w   bits 7:0 IN, 15:8 OUT, bit n endpoint n (4 the
 *                       frame endpoint): back to DATA0
 *   0x018  IRQEN    rw  which of PEND raise irq_o
 *   0x01C  PEND     r   bits 7:0 OUT full on endpoint n, 15:8 IN free
 *                       (not armed) on endpoint n (bit 4 of each is
 *                       always 0), 16 bus reset event, 17 link event
 *   0x020  CAPS     r   bits 3:0 the highest CPU endpoint (7), 7:4 the
 *                       frame endpoint's number (4), 11:8 log2 of the
 *                       receive slots, 15:12 version (2)
 *
 *   0x100 + 16n      CPU endpoint n (0-3, 5-7)
 *     +0  OUT    r   bit 0 full, bit 1 it was a SETUP, bits 14:8 length
 *                w   anything: empty again (the next packet is ACKed)
 *     +4  IN     r   bit 8 armed; w: bits 6:0 length, bit 8 arm
 *     +8  STALL  rw  bit 0: STALL both directions (a SETUP clears it
 *                    on endpoint 0)
 *
 *   0x080  NSTAT    r   bit 0 a frame is ready, 1 transmit busy, 2 the
 *                       frame endpoint is enabled, 3 NCTL's link,
 *                       bits 11:4 frames dropped (too long; wraps),
 *                       bits 27:16 the ready frame's length
 *   0x084  NRXCTL   w   anything: the ready frame is done with
 *   0x088  NTXCTL   w   bits 11:0 length, bit 16 send (ignored while
 *                       busy), bit 17 abandon the frame being sent
 *   0x08C  NCTL     rw  bit 0 link: `net` is up
 *
 *   0x400 + 0x80n    CPU endpoint n's OUT buffer, 64 bytes, read
 *   0x440 + 0x80n    CPU endpoint n's IN buffer, 64 bytes, write
 *                    (n 0-7; 0x600-0x67F, endpoint 4's, is unused)
 *   0x800-0xDFF      the ready frame, read
 *   0x1000-0x15FF    the frame to send, write
 *
 * Three DP16KD, or two with RX_SLOTS 1. What flows one way shares a
 * memory: the receive slots and the CPU endpoints' OUT buffers
 * (written by the USB side, read by the CPU: slot 0 at word 0, slot 1
 * at 384, the OUT buffers after them, 16 words an endpoint -- 1024
 * words, or 512 with one slot), and the transmit slot and the IN
 * buffers (written by the CPU, read by the USB side: 512 words, the IN
 * buffers at 384). A slot is
 * 384 words, 24 packets: 1536 bytes, past ECM's 1514. With one slot
 * nothing is lost either: the endpoint NAKs while it is full, and the
 * host keeps the frame until it is free.
 *
 * An OUT packet to a full CPU endpoint is NAKed and not stored; a
 * SETUP is always stored (USB 2.0 8.5.3) and also disarms endpoint
 * 0's IN side. An IN token to a CPU endpoint that is not armed is
 * NAKed; a packet the host does not ACK is sent again from its start.
 * The frame endpoint NAKs OUT while both receive slots are full and IN
 * while nothing is to be sent; a frame longer than 1536 bytes is
 * dropped and counted. A retried packet (the host missed our ACK)
 * comes back with the old toggle; the SIE reports it as an error and
 * it is not stored twice.
 *
 * A bus reset clears the address, every endpoint, the frame
 * endpoint's enable and both slots.
 *
 * The SIE runs at 48 MHz with 4 samples per bit (one cycle in four),
 * the system clock, as rtl/usb_cdc_uart.v.
 */

`default_nettype none

module usbd_ep #(
	parameter RX_SLOTS = 2			// receive slots: 2, or 1 to save a DP16KD
) (
	input wire clk,			// 48 MHz
	input wire rst,

	input wire [12:0] wb_adr_i,	// byte offset in the slot
	input wire [31:0] wb_dat_i,
	output wire [31:0] wb_dat_o,
	input wire [3:0] wb_sel_i,
	input wire wb_we_i,
	input wire wb_stb_i,
	input wire wb_cyc_i,
	output reg wb_ack_o,

	output wire irq_o,		// level: (PEND & IRQEN) != 0
	output wire net_rx_o,		// level: a frame is ready

	inout wire usb_dp,
	inout wire usb_dn,
	inout wire usb_pu
);

	localparam [31:0] MAGIC = 32'h56454455;	// "UDEV"
	localparam [3:0] RXL2 = (RX_SLOTS == 2) ? 4'd1 : 4'd0;
	localparam UMW = (RX_SLOTS == 2) ? 1024 : 512;	// umem's words
	localparam [9:0] OBUF_BASE = (RX_SLOTS == 2) ? 10'd768 : 10'd384;

	// -- the 1-in-4 clock enable the SIE wants --

	reg [1:0] gate_cnt;
	reg gate;
	always @(posedge clk) begin
		if (rst) begin
			gate_cnt <= 2'd0;
			gate <= 1'b0;
		end else begin
			gate_cnt <= gate_cnt + 2'd1;
			gate <= (gate_cnt == 2'd3);
		end
	end

	// -- registers --

	reg attach;
	reg [6:0] addr;
	reg ev_reset, ev_link;
	reg [7:0] in_bulk, out_bulk, in_int;	// bit 4 unused: the frame endpoint
	reg ep4_en;
	reg [7:0] tog_in, tog_out;		// pulses, held until the next gate
	reg [17:0] irqen;

	// CPU endpoints n = 0-7 (4 unused): bit n of each
	reg [7:0] out_full, out_setup, stall, in_armed;
	reg [6:0] out_len0, out_len1, out_len2, out_len3, out_len5, out_len6, out_len7;
	reg [6:0] in_len0, in_len1, in_len2, in_len3, in_len5, in_len6, in_len7;
	reg seen_reset;

	// the frame endpoint
	reg rx_w, rx_r;				// slot being filled, slot ready
	reg [1:0] rx_n;				// slots holding a frame
	reg [10:0] fwp;				// bytes of the frame so far (x64)
	reg fbad;				// too long: drop at its end
	reg [11:0] rx_len0, rx_len1;
	reg [7:0] rx_drop;
	reg tx_busy;
	reg [11:0] tx_len;
	reg [11:0] tx_off;			// start of the packet being sent
	reg net_link;

	// -- the SIE --

	wire bus_reset;
	wire [3:0] endp;
	wire [10:0] frame;
	wire [7:0] out_data;
	wire out_valid, out_err, setup, out_ready;
	wire in_req, in_ready, in_data_ack;
	wire [2:0] ep = endp[2:0];
	wire is4 = (endp == 4'd4);		// the frame endpoint
	wire ep_ok = !endp[3] && !is4;		// a CPU endpoint

	wire sie_dp_pu, sie_tx_en, sie_dp_tx, sie_dn_tx;

	reg [6:0] in_len_ep;
	always @(*) begin
		case (ep)
		3'd0: in_len_ep = in_len0;
		3'd1: in_len_ep = in_len1;
		3'd2: in_len_ep = in_len2;
		3'd3: in_len_ep = in_len3;
		3'd5: in_len_ep = in_len5;
		3'd6: in_len_ep = in_len6;
		3'd7: in_len_ep = in_len7;
		default: in_len_ep = 7'd0;
		endcase
	end

	reg [6:0] rp;			// IN read pointer, within the packet
	reg [6:0] wp;			// OUT write pointer, within the packet
	reg rx_setup;			// this OUT transaction began with SETUP
	// Whether an OUT packet is taken is decided at its first byte and
	// kept to its end: the CPU may empty the endpoint (or `net` free a
	// slot) while the packet arrives, and the rest of it must not be
	// stored, or ACKed, as if it were the whole packet.
	reg pkt_on;
	reg pkt_drop;
	reg tx_q;			// the SIE's tx_en a cycle ago
	reg hs_drop;			// a zero-length packet's fate, as the handshake said
	wire [7:0] in_byte;

	// the frame endpoint's IN side
	wire [11:0] tx_rem = tx_len - tx_off;
	wire [6:0] tx_pkt = (tx_rem >= 12'd64) ? 7'd64 : tx_rem[6:0];
	wire rx_full = (RX_SLOTS == 2) ? (rx_n == 2'd2) : (rx_n == 2'd1);

	wire in_armed_ep = ep_ok && in_armed[ep];
	wire in_valid = is4 ? (tx_busy && ((tx_pkt == 7'd0) || (rp < tx_pkt))) :
		(in_armed_ep && ((in_len_ep == 7'd0) || (rp < in_len_ep)));
	wire in_zlp = is4 ? (tx_busy && (tx_pkt == 7'd0)) :
		(in_armed_ep && (in_len_ep == 7'd0));
	wire in_nak = is4 ? !tx_busy : !in_armed_ep;
	wire busy_now = is4 ? (!ep4_en || rx_full) :
		(ep_ok ? (out_full[ep] && !rx_setup) : 1'b1);
	wire drop_now = pkt_on ? pkt_drop : busy_now;
	wire out_nak = drop_now;
	wire eop_drop = pkt_on ? pkt_drop : hs_drop;
	wire stall_ep = ep_ok ? stall[ep] : 1'b0;

	sie #(
		.IN_CTRL_MAXPACKETSIZE(64),
		.IN_BULK_MAXPACKETSIZE(64),
		.IN_INT_MAXPACKETSIZE(64),
		.IN_ISO_MAXPACKETSIZE(8),
		.BIT_SAMPLES(4)
	) sie_i (
		.frame_o(frame),
		.clk_i(clk),
		.rstn_i(~rst),
		.clk_gate_i(gate),
		.bus_reset_o(bus_reset),
		.endp_o(endp),
		.stall_i(stall_ep),
		.out_data_o(out_data),
		.out_valid_o(out_valid),
		.out_err_o(out_err),
		.setup_o(setup),
		.out_ready_o(out_ready),
		.out_nak_i(out_nak),
		.in_req_o(in_req),
		.in_ready_o(in_ready),
		.in_data_ack_o(in_data_ack),
		.in_valid_i(in_valid),
		.in_data_i(in_byte),
		.in_zlp_i(in_zlp),
		.in_nak_i(in_nak),
		.usb_en_i(attach && seen_reset),
		.usb_detach_i(!attach),
		.addr_i(addr),
		.in_bulk_endps_i({8'h00, in_bulk[7:5], ep4_en, in_bulk[3:1], 1'b0}),
		.out_bulk_endps_i({8'h00, out_bulk[7:5], ep4_en, out_bulk[3:1], 1'b0}),
		.in_int_endps_i({8'h00, in_int[7:5], 1'b0, in_int[3:1], 1'b0}),
		.out_int_endps_i(16'h0000),
		.in_iso_endps_i(16'h0000),
		.out_iso_endps_i(16'h0000),
		.in_toggle_reset_i({8'h00, tog_in}),
		.out_toggle_reset_i({8'h00, tog_out}),
		.dp_pu_o(sie_dp_pu),
		.tx_en_o(sie_tx_en),
		.dp_tx_o(sie_dp_tx),
		.dn_tx_o(sie_dn_tx),
		.dp_rx_i(usb_dp),
		.dn_rx_i(usb_dn)
	);

	// -- line buffers, as rtl/usb_cdc_uart.v's --
	//
	// D+/D- driven only while the SIE transmits; the pull-up pin driven
	// high to announce a full-speed device and released otherwise,
	// never driven low.
	assign usb_dp = sie_tx_en ? sie_dp_tx : 1'bz;
	assign usb_dn = sie_tx_en ? sie_dn_tx : 1'bz;
	assign usb_pu = sie_dp_pu ? 1'b1 : 1'bz;

	// -- bus decode --

	wire req = wb_cyc_i && wb_stb_i && !wb_ack_o;
	wire [12:0] off = wb_adr_i;
	wire reg_area = (off[12:10] == 3'b000);		// 0x000-0x3FF
	wire cpu_buf = (off[12:10] == 3'b001);		// 0x400-0x7FF
	wire rx_area = (off[12:11] == 2'b01);		// 0x800-0xFFF
	wire tx_area = (off[12:11] == 2'b10);		// 0x1000-0x17FF
	wire cpu_in_wr = req && wb_we_i && cpu_buf && off[6];
	wire tx_wr = req && wb_we_i && tx_area;

	// -- the buffers: one memory each way --
	//
	// umem, written by the USB side a byte at a time and read by the
	// CPU: the receive slots, words 0-383 and 384-767 (a frame's bytes
	// at slot * 384 + fwp / 4 + wp / 4), and the CPU endpoints' OUT
	// buffers at OBUF_BASE + 16 * endpoint. dmem, written by the CPU and
	// read by the USB side: the transmit slot, words 0-383, and the IN
	// buffers at 384 + 16 * endpoint. One write port and one registered
	// read port each, so each is plain block RAM.

	reg [31:0] umem [0:UMW-1];
	reg [31:0] dmem [0:511];

	// the CPU's writes into dmem: the frame to send, or an IN buffer
	wire [8:0] tx_widx_cpu = off[10:2];
	wire tx_wr_ok = tx_wr && (tx_widx_cpu < 9'd384);
	wire [8:0] dm_waddr = cpu_in_wr ? { 2'b11, off[9:7], off[5:2] } : tx_widx_cpu;
	wire dm_we = cpu_in_wr || tx_wr_ok;

	always @(posedge clk) begin
		if (dm_we && wb_sel_i[0]) dmem[dm_waddr][7:0] <= wb_dat_i[7:0];
		if (dm_we && wb_sel_i[1]) dmem[dm_waddr][15:8] <= wb_dat_i[15:8];
		if (dm_we && wb_sel_i[2]) dmem[dm_waddr][23:16] <= wb_dat_i[23:16];
		if (dm_we && wb_sel_i[3]) dmem[dm_waddr][31:24] <= wb_dat_i[31:24];
	end

	// the USB side's IN read: registered, a cycle after the pointer
	// moves (the SIE takes a byte at most every 32 cycles)
	wire [8:0] dm_raddr = is4 ? { tx_off[10:6], rp[5:2] } :
		{ 2'b11, ep, rp[5:2] };
	reg [31:0] in_word;
	reg [1:0] rp_lane;
	always @(posedge clk) begin
		in_word <= dmem[dm_raddr];
		rp_lane <= rp[1:0];
	end
	assign in_byte = in_word >> { rp_lane, 3'b000 };

	// the USB side's OUT writes into umem
	wire out_byte_in = gate && out_ready && out_valid;
	wire out_byte = out_byte_in && !drop_now && (wp < 7'd64);
	wire out_byte_cpu = out_byte && ep_ok;
	wire out_byte_net = out_byte && is4;
	wire [9:0] rx_slot_base = rx_w ? 10'd384 : 10'd0;
	wire [9:0] um_waddr = out_byte_net ?
		(rx_slot_base + { 1'b0, fwp[10:6], 4'd0 } + { 6'd0, wp[5:2] }) :
		(OBUF_BASE + { 3'd0, ep, wp[5:2] });
	wire um_we = out_byte_net || out_byte_cpu;

	always @(posedge clk) begin
		if (um_we && wp[1:0] == 2'd0) umem[um_waddr][7:0] <= out_data;
		if (um_we && wp[1:0] == 2'd1) umem[um_waddr][15:8] <= out_data;
		if (um_we && wp[1:0] == 2'd2) umem[um_waddr][23:16] <= out_data;
		if (um_we && wp[1:0] == 2'd3) umem[um_waddr][31:24] <= out_data;
	end

	// the CPU's reads of umem, registered: the ready frame, or an OUT buffer
	wire [9:0] rx_rbase = rx_r ? 10'd384 : 10'd0;
	wire [9:0] um_raddr = rx_area ? (rx_rbase + { 1'b0, off[10:2] }) :
		(OBUF_BASE + { 3'd0, off[9:7], off[5:2] });
	reg [31:0] um_q;
	always @(posedge clk)
		um_q <= umem[um_raddr];

	// -- endpoint state --

	wire setup_tok = gate && out_ready && setup && !out_valid && !out_err;
	wire out_eop = gate && out_ready && !setup && !out_valid && !out_err;
	wire out_bad = gate && out_ready && out_err;
	wire in_start = gate && in_ready && in_req;
	wire in_next = gate && in_ready && !in_req && !in_data_ack;
	wire in_acked = gate && in_ready && in_data_ack;

	// CPU writes to the registers
	wire cpu_reg_wr = req && wb_we_i && reg_area;
	wire [7:0] rword = off[9:2];			// register word
	wire ep_reg = (off[9:7] == 3'b010);		// 0x100-0x17F
	wire [2:0] cpu_ep = off[6:4];
	wire wr_out = cpu_reg_wr && ep_reg && off[3:2] == 2'd0;
	wire wr_in = cpu_reg_wr && ep_reg && off[3:2] == 2'd1;
	wire wr_stall = cpu_reg_wr && ep_reg && off[3:2] == 2'd2;
	wire wr_ctrl = cpu_reg_wr && rword == 8'h01;
	wire wr_events = cpu_reg_wr && rword == 8'h02;
	wire wr_eptype = cpu_reg_wr && rword == 8'h04;
	wire wr_toggle = cpu_reg_wr && rword == 8'h05;
	wire wr_irqen = cpu_reg_wr && rword == 8'h06;
	wire wr_nrxctl = cpu_reg_wr && rword == 8'h21;
	wire wr_ntxctl = cpu_reg_wr && rword == 8'h22;
	wire wr_nctl = cpu_reg_wr && rword == 8'h23;

	// the frame endpoint's events this cycle
	wire net_eop = out_eop && is4 && !eop_drop;
	wire net_full_pkt = (wp == 7'd64);
	wire [11:0] frame_len = { 1'b0, fwp } + { 5'd0, wp };
	wire rx_commit = net_eop && !net_full_pkt && !fbad && (frame_len != 12'd0);
	wire rx_release = wr_nrxctl && (rx_n != 0);
	wire tx_go = wr_ntxctl && wb_dat_i[16] && !tx_busy &&
		(wb_dat_i[11:0] != 12'd0) && (wb_dat_i[11:0] <= 12'd1536);

	always @(posedge clk) begin
		if (rx_commit && !rx_w) rx_len0 <= frame_len;
		if (rx_commit && rx_w) rx_len1 <= frame_len;
	end

	always @(posedge clk) begin
		if (rst) begin
			attach <= 1'b0;
			addr <= 7'd0;
			ev_reset <= 1'b0;
			ev_link <= 1'b0;
			seen_reset <= 1'b0;
			in_bulk <= 8'd0;
			out_bulk <= 8'd0;
			in_int <= 8'd0;
			ep4_en <= 1'b0;
			tog_in <= 8'd0;
			tog_out <= 8'd0;
			irqen <= 18'd0;
			out_full <= 8'd0;
			out_setup <= 8'd0;
			stall <= 8'd0;
			in_armed <= 8'd0;
			out_len0 <= 7'd0; out_len1 <= 7'd0; out_len2 <= 7'd0; out_len3 <= 7'd0;
			out_len5 <= 7'd0; out_len6 <= 7'd0; out_len7 <= 7'd0;
			in_len0 <= 7'd0; in_len1 <= 7'd0; in_len2 <= 7'd0; in_len3 <= 7'd0;
			in_len5 <= 7'd0; in_len6 <= 7'd0; in_len7 <= 7'd0;
			rp <= 7'd0;
			wp <= 7'd0;
			rx_setup <= 1'b0;
			pkt_on <= 1'b0;
			pkt_drop <= 1'b0;
			tx_q <= 1'b0;
			hs_drop <= 1'b0;
			rx_w <= 1'b0;
			rx_r <= 1'b0;
			rx_n <= 2'd0;
			fwp <= 11'd0;
			fbad <= 1'b0;
			rx_drop <= 8'd0;
			tx_busy <= 1'b0;
			tx_len <= 12'd0;
			tx_off <= 12'd0;
			net_link <= 1'b0;
		end else if (bus_reset) begin
			addr <= 7'd0;
			ev_reset <= 1'b1;
			seen_reset <= 1'b1;
			out_full <= 8'd0;
			out_setup <= 8'd0;
			stall <= 8'd0;
			in_armed <= 8'd0;
			ep4_en <= 1'b0;
			rp <= 7'd0;
			wp <= 7'd0;
			rx_setup <= 1'b0;
			pkt_on <= 1'b0;
			pkt_drop <= 1'b0;
			rx_w <= 1'b0;
			rx_r <= 1'b0;
			rx_n <= 2'd0;
			fwp <= 11'd0;
			fbad <= 1'b0;
			tx_busy <= 1'b0;
			tx_off <= 12'd0;
			if (wr_ctrl) attach <= wb_dat_i[0];
		end else begin
			if (gate) begin
				tog_in <= 8'd0;
				tog_out <= 8'd0;
			end

			// -- USB side --
			if (setup_tok && ep_ok && ep == 3'd0) begin
				wp <= 7'd0;
				pkt_on <= 1'b0;
				rx_setup <= 1'b1;
				out_full[0] <= 1'b0;
				in_armed[0] <= 1'b0;
				stall[0] <= 1'b0;
			end
			// the handshake after an OUT packet starts: what the SIE was
			// told is what happens to a zero-length one
			tx_q <= sie_tx_en;
			if (sie_tx_en && !tx_q)
				hs_drop <= drop_now;
			if (out_byte_in && !pkt_on) begin
				pkt_on <= 1'b1;
				pkt_drop <= busy_now;
			end
			if (out_byte)
				wp <= wp + 7'd1;
			if (out_eop) begin
				pkt_on <= 1'b0;
				if (ep_ok && !eop_drop) begin
					out_full[ep] <= 1'b1;
					out_setup[ep] <= rx_setup;
					case (ep)
					3'd0: out_len0 <= wp;
					3'd1: out_len1 <= wp;
					3'd2: out_len2 <= wp;
					3'd3: out_len3 <= wp;
					3'd5: out_len5 <= wp;
					3'd6: out_len6 <= wp;
					3'd7: out_len7 <= wp;
					default: ;
					endcase
				end
				wp <= 7'd0;
				rx_setup <= 1'b0;
			end
			if (out_bad) begin
				wp <= 7'd0;
				rx_setup <= 1'b0;
				pkt_on <= 1'b0;
			end
			if (in_start)
				rp <= 7'd0;
			if (in_next)
				rp <= rp + 7'd1;
			if (in_acked && ep_ok)
				in_armed[ep] <= 1'b0;

			// -- the frame endpoint, receiving --
			if (net_eop) begin
				if (net_full_pkt) begin
					// a full packet: the frame goes on, unless it
					// would pass 1536 bytes, the slot
					if (fwp == 11'd1472) fbad <= 1'b1;
					else if (!fbad) fwp <= fwp + 11'd64;
				end else begin
					// a short packet ends the frame
					if (fbad) rx_drop <= rx_drop + 8'd1;
					fwp <= 11'd0;
					fbad <= 1'b0;
				end
			end
			if (rx_commit && RX_SLOTS == 2) rx_w <= !rx_w;
			if (rx_release && RX_SLOTS == 2) rx_r <= !rx_r;
			if (rx_commit && !rx_release) rx_n <= rx_n + 2'd1;
			if (!rx_commit && rx_release) rx_n <= rx_n - 2'd1;

			// -- the frame endpoint, sending --
			if (in_acked && is4 && tx_busy) begin
				if (tx_pkt == 7'd64)
					tx_off <= tx_off + 12'd64;
				else begin
					tx_busy <= 1'b0;
					tx_off <= 12'd0;
				end
			end

			// -- CPU side (after the USB side: the CPU's word wins a tie) --
			if (wr_ctrl) begin
				attach <= wb_dat_i[0];
				addr <= wb_dat_i[14:8];
			end
			if (wr_events) begin
				if (wb_dat_i[0]) ev_reset <= 1'b0;
				if (wb_dat_i[1]) ev_link <= 1'b0;
			end
			if (wr_eptype) begin
				in_bulk <= wb_dat_i[7:0];
				out_bulk <= wb_dat_i[15:8];
				in_int <= wb_dat_i[23:16];
				ep4_en <= wb_dat_i[24];
				if (!wb_dat_i[24]) begin
					// disabled: whatever was in flight is gone
					rx_w <= 1'b0;
					rx_r <= 1'b0;
					rx_n <= 2'd0;
					fwp <= 11'd0;
					fbad <= 1'b0;
					tx_busy <= 1'b0;
					tx_off <= 12'd0;
				end
			end
			if (wr_toggle) begin
				tog_in <= wb_dat_i[7:0];
				tog_out <= wb_dat_i[15:8];
			end
			if (wr_irqen)
				irqen <= wb_dat_i[17:0];
			if (wr_out && cpu_ep != 3'd4) begin
				out_full[cpu_ep] <= 1'b0;
				out_setup[cpu_ep] <= 1'b0;
			end
			if (wr_in && cpu_ep != 3'd4) begin
				in_armed[cpu_ep] <= wb_dat_i[8];
				case (cpu_ep)
				3'd0: in_len0 <= wb_dat_i[6:0];
				3'd1: in_len1 <= wb_dat_i[6:0];
				3'd2: in_len2 <= wb_dat_i[6:0];
				3'd3: in_len3 <= wb_dat_i[6:0];
				3'd5: in_len5 <= wb_dat_i[6:0];
				3'd6: in_len6 <= wb_dat_i[6:0];
				3'd7: in_len7 <= wb_dat_i[6:0];
				default: ;
				endcase
			end
			if (wr_stall && cpu_ep != 3'd4)
				stall[cpu_ep] <= wb_dat_i[0];
			if (tx_go) begin
				tx_len <= wb_dat_i[11:0];
				tx_off <= 12'd0;
				tx_busy <= 1'b1;
			end
			if (wr_ntxctl && wb_dat_i[17]) begin
				tx_busy <= 1'b0;
				tx_off <= 12'd0;
			end
			if (wr_nctl) begin
				net_link <= wb_dat_i[0];
				if (wb_dat_i[0] != net_link) ev_link <= 1'b1;
			end
		end
	end

	// -- interrupts --

	// endpoint 4's bits are the frame endpoint's, not a CPU endpoint's: 0
	wire [7:0] in_free = ~in_armed & 8'b1110_1111;
	wire [7:0] out_full_p = out_full & 8'b1110_1111;
	wire [17:0] pend = { ev_link, ev_reset, in_free, out_full_p };
	assign irq_o = |(pend & irqen);
	assign net_rx_o = (rx_n != 0);

	// -- reads --

	reg [6:0] out_len_cpu;
	always @(*) begin
		case (cpu_ep)
		3'd0: out_len_cpu = out_len0;
		3'd1: out_len_cpu = out_len1;
		3'd2: out_len_cpu = out_len2;
		3'd3: out_len_cpu = out_len3;
		3'd5: out_len_cpu = out_len5;
		3'd6: out_len_cpu = out_len6;
		3'd7: out_len_cpu = out_len7;
		default: out_len_cpu = 7'd0;
		endcase
	end

	wire [11:0] rx_len_r = rx_r ? rx_len1 : rx_len0;

	reg [31:0] rd_reg;
	always @(*) begin
		rd_reg = 32'h0;
		if (ep_reg) begin
			case (off[3:2])
			2'd0: rd_reg = { 17'h0, out_len_cpu, 6'h0, out_setup[cpu_ep], out_full[cpu_ep] };
			2'd1: rd_reg = { 23'h0, in_armed[cpu_ep], 8'h0 };
			2'd2: rd_reg = { 31'h0, stall[cpu_ep] };
			default: rd_reg = 32'h0;
			endcase
		end else begin
			case (rword)
			8'h00: rd_reg = MAGIC;
			8'h01: rd_reg = { 17'h0, addr, 7'h0, attach };
			8'h02: rd_reg = { 30'h0, ev_link, ev_reset };
			8'h03: rd_reg = { 21'h0, frame };
			8'h04: rd_reg = { 7'h0, ep4_en, in_int, out_bulk, in_bulk };
			8'h06: rd_reg = { 14'h0, irqen };
			8'h07: rd_reg = { 14'h0, pend };
			8'h08: rd_reg = { 16'h0, 4'd2, RXL2, 4'd4, 4'd7 };
			8'h20: rd_reg = { 4'h0, (rx_n != 0) ? rx_len_r : 12'd0, 4'h0,
				rx_drop, net_link, ep4_en, tx_busy, (rx_n != 0) };
			8'h23: rd_reg = { 31'h0, net_link };
			default: rd_reg = 32'h0;
			endcase
		end
	end

	reg [1:0] rd_src;		// 0 registers, 1 a CPU OUT buffer, 2 the ready frame
	reg [31:0] rd_reg_q;
	always @(posedge clk) begin
		if (rst) begin
			wb_ack_o <= 1'b0;
			rd_src <= 2'd0;
			rd_reg_q <= 32'h0;
		end else begin
			wb_ack_o <= req;
			rd_src <= rx_area ? 2'd2 : (cpu_buf && !off[6]) ? 2'd1 : 2'd0;
			rd_reg_q <= reg_area ? rd_reg : 32'h0;
		end
	end

	assign wb_dat_o = (rd_src != 2'd0) ? um_q : rd_reg_q;

endmodule

`default_nettype wire
