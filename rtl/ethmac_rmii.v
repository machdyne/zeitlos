/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * RMII Ethernet MAC (LAN8720A PHY). mozart_ml1 only -- an optional
 * alternative to rtl/spibb_eth.v (SPI ENC28J60) for boards that have
 * an RMII PHY but no SPI Ethernet MAC.
 *
 * No MDIO/MDC on this board -- see boards/mozart_ml1.lpf's PULLMODE=UP
 * on rx_data/crs_dv. Those pull-ups set the LAN8720A's strap-configured
 * mode at the moment eth_rst_n is released (PHY address / auto-negotiation
 * mode), which is the only configuration this MAC will ever be able to
 * do -- there's no MDIO bus to read link/speed/duplex back afterward.
 * This MAC assumes whatever the PHY auto-negotiates to is 100M full
 * duplex (the common case against a modern switch) and never checks.
 *
 * PHASE 1: pin plumbing + a status register (crs_dv + a slow ETH_REFCLK
 * heartbeat), no RX/TX datapath. Kept below, unchanged.
 *
 * PHASE 2 (this version): RX datapath. Continuously watches eth_rxd/
 * eth_crs_dv for the SFD byte (0xD5) to lock onto byte alignment
 * (rather than counting exactly 7 preamble bytes, which real PHYs
 * don't always deliver cleanly), assembles bytes, runs a bit-serial
 * CRC32 across every byte from the destination address through the
 * received FCS, and on carrier-drop checks the running CRC against
 * the fixed IEEE 802.3 residual (0xDEBB20E3 -- the value a correct
 * CRC32 implementation converges to when run continuously across
 * frame-data-plus-its-own-FCS, no final invert; verified against
 * Python's zlib.crc32 on synthetic frames while writing this, not
 * just transcribed from memory -- see tb/ethmac_rmii_tb.v). A single
 * receive buffer holds one frame at a time; a new frame arriving
 * before software has popped the previous one is dropped and
 * counted, not double-buffered (deliberately simple for a first
 * version -- see the register map below).
 *
 * PHASE 3 (this version): TX datapath. Software writes a frame into
 * TX_BUF, latches its length via TX_LEN, then triggers TX_CTRL.
 * The TX engine emits 7 bytes of preamble + SFD, shifts the frame
 * data out while accumulating the same bit-serial CRC32 used for RX
 * (over the same bits, in the same order they go out), then appends
 * the FCS -- computed as the ONE'S COMPLEMENT of the running CRC
 * register after the last data bit (matching standard Ethernet FCS
 * generation, i.e. the same value Python's zlib.crc32() would give
 * for the same bytes -- verified against it in the TX/RX loopback
 * testbench, tb/ethmac_rmii_loopback_tb.v, not just derived on
 * paper). A minimum 12-byte (48 eth_refclk cycle) inter-frame gap
 * follows before the engine reports itself idle again.
 *
 * CLOCK DOMAINS: wb_clk_i (the system bus, ~48MHz) and eth_refclk
 * (RMII's shared 50MHz reference clock) are independent. The RX
 * engine below runs entirely in the eth_refclk domain (eth_crs_dv/
 * eth_rxd are already synchronous to eth_refclk -- the PHY generates
 * them off the exact same clock -- so no synchronizer is needed on
 * the way in). Crossing back out to software happens in two places,
 * both explained where they occur: the ready/status bits (a real
 * 2-flop synchronizer) and the RX_LEN/RX_BUF contents (a "quasi-
 * static" argument -- see the comment above rx_len_sync).
 *
 * REGISTER MAP (word-addressed, all offsets from 0x6000_0000):
 *   0x00 STATUS (read-only):
 *     bit0  = crs_dv, synchronized (Phase 1, unchanged -- raw pin
 *             state, not frame-aware)
 *     bit1  = ETH_REFCLK heartbeat, synchronized (Phase 1, unchanged)
 *     bit2  = rx_ready -- a frame is waiting in the RX buffer
 *     bit3  = tx_busy -- a transmission is in progress; don't touch
 *             TX_BUF/TX_LEN/TX_CTRL again until this clears
 *     bits[7:4]  = rx_drop_count -- frames dropped because the
 *                  buffer was still full (software too slow to
 *                  drain), saturating 4-bit counter
 *     bits[11:8] = rx_err_count -- frames dropped for bad CRC or
 *                  under minimum length, saturating 4-bit counter
 *   0x04 RX_LEN (read-only): byte length of the frame currently in
 *     the RX buffer, NOT including the 4-byte FCS (same convention
 *     as sw/apps/net/enc28j60.c's enc28j60_recv()) -- 0 if rx_ready
 *     is clear
 *   0x08 RX_CTRL (write-only): write any value to release the RX
 *     buffer back to hardware once you've finished reading it
 *   0x0C TX_LEN (write-only): frame length to send, NOT including
 *     FCS (hardware appends it) -- write this BEFORE TX_CTRL
 *   0x10 TX_CTRL (write-only): write any value to start transmitting
 *     TX_LEN bytes from TX_BUF. Only meaningful while tx_busy (STATUS
 *     bit3) is clear -- write TX_BUF, then TX_LEN, then TX_CTRL, then
 *     poll tx_busy before touching any of the three again. Software's
 *     job to obey that order; hardware doesn't defend against it.
 *   0x100-0x8FC RX_BUF: the received frame, one word every 4 bytes,
 *     valid only while rx_ready is set and only up to RX_LEN bytes
 *     (RX_LEN+4 bytes are actually present -- the trailing FCS is
 *     still there, just not counted in RX_LEN)
 *   0xA00-0x11FC TX_BUF: the frame to send, one word every 4 bytes.
 *     Write TX_LEN bytes here (do NOT write an FCS -- hardware
 *     generates and appends it)
 */

module ethmac_rmii_wb #()
(
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output reg wb_ack_o,
	input wb_cyc_i,

	input eth_refclk,
	input [1:0] eth_rxd,
	output [1:0] eth_txd,
	output eth_tx_en,
	input eth_crs_dv,
	output eth_rst_n
);

	localparam REG_STATUS  = 0;
	localparam REG_RXLEN   = 1;
	localparam REG_RXCTRL  = 2;
	localparam REG_TXLEN   = 3;
	localparam REG_TXCTRL  = 4;
	localparam RXBUF_BASE  = 64;   // word offset -- byte offset 0x100
	localparam RXBUF_WORDS = 512;  // 2048 bytes -- max standard frame is 1518
	localparam TXBUF_BASE  = 640;  // word offset -- byte offset 0xA00
	localparam TXBUF_WORDS = 512;  // 2048 bytes

	// Ethernet FCS residual: running the bit-serial CRC32 update below
	// (init 0xFFFFFFFF, reflected poly 0xEDB88320, no final invert)
	// continuously across a frame's bytes AND its own trailing FCS
	// converges to this fixed value for any valid frame, regardless
	// of contents -- see this file's header comment.
	localparam [31:0] CRC32_RESIDUAL = 32'hDEBB20E3;

	// ===========================================================
	// PHASE 1: PHY reset stretch, CRS_DV/ETH_REFCLK status sync
	// (unchanged from Phase 1 -- see header comment there)
	// ===========================================================

	reg [21:0] phy_rst_counter = 0;
	reg phy_rst_done = 0;

	always @(posedge wb_clk_i) begin
		if (wb_rst_i) begin
			phy_rst_counter <= 0;
			phy_rst_done <= 0;
		end else if (!phy_rst_done) begin
			phy_rst_counter <= phy_rst_counter + 1;
			if (&phy_rst_counter) phy_rst_done <= 1;
		end
	end

	assign eth_rst_n = phy_rst_done;

	reg crs_dv_meta = 0;
	reg crs_dv_sync = 0;

	always @(posedge wb_clk_i) begin
		crs_dv_meta <= eth_crs_dv;
		crs_dv_sync <= crs_dv_meta;
	end

	reg [23:0] refclk_counter = 0;

	always @(posedge eth_refclk) begin
		refclk_counter <= refclk_counter + 1;
	end

	reg refclk_hb_meta = 0;
	reg refclk_hb_sync = 0;

	always @(posedge wb_clk_i) begin
		refclk_hb_meta <= refclk_counter[23];
		refclk_hb_sync <= refclk_hb_meta;
	end

	// ===========================================================
	// PHASE 2: RX datapath
	// ===========================================================

	// bit-serial CRC32 update, reflected/little-endian form (matches
	// the bit order RMII delivers -- LSB of each byte first, which is
	// also standard Ethernet FCS bit order). two of these chained
	// per eth_refclk cycle handle both bits of a dibit.
	function [31:0] crc32_update;
		input [31:0] crc_in;
		input din;
		begin
			if (crc_in[0] ^ din)
				crc32_update = (crc_in >> 1) ^ 32'hEDB88320;
			else
				crc32_update = crc_in >> 1;
		end
	endfunction

	// RX packet buffer -- one frame at a time. word-addressed for the
	// CPU read side (below), byte-lane-written from the RX engine
	// (this section). only ever written from this always block
	// (single driver), only ever read (never written) from the CPU
	// side's always block further down.
	reg [31:0] rxbuf [0:RXBUF_WORDS-1];

	// continuously-updating byte assembler. RMII delivers 2 bits per
	// eth_refclk cycle; d0 (eth_rxd, first cycle of a byte) ends up
	// as the byte's LSB after 4 cycles of "shift new dibit into the
	// top, old bits move down" -- new_byte here is exactly what
	// rx_shift will hold after this cycle's dibit is incorporated.
	reg [7:0] rx_shift = 0;
	wire [7:0] new_byte = { eth_rxd, rx_shift[7:2] };

	reg sfd_found = 0;
	reg [1:0] dibit_cnt = 0;
	reg [10:0] byte_cnt = 0;       // bytes received this frame (0..2047)
	reg [31:0] rx_crc = 32'hFFFFFFFF;

	// handoff to software: single-slot semaphore, plus a toggle-based
	// CDC handshake for the "pop" request coming from wb_clk_i (see
	// pop_toggle below).
	reg rx_avail = 0;
	reg [10:0] rx_len = 0;
	reg [3:0] rx_drop_count = 0;   // dropped: buffer still full
	reg [3:0] rx_err_count = 0;    // dropped: bad CRC / too short

	reg pop_toggle_meta = 0;
	reg pop_toggle_sync = 0;
	reg pop_toggle_sync_d = 0;

	wire [31:0] crc_after_bit0 = crc32_update(rx_crc, eth_rxd[0]);
	wire [31:0] crc_after_bit1 = crc32_update(crc_after_bit0, eth_rxd[1]);

	always @(posedge eth_refclk) begin

		// cross the wb_clk_i-domain pop request in -- a plain 2-flop
		// synchronizer on the toggle bit, then edge-detect it here.
		// safe for a single bit that only ever changes once per
		// software read (nowhere near every eth_refclk cycle).
		pop_toggle_meta <= pop_toggle;
		pop_toggle_sync <= pop_toggle_meta;
		pop_toggle_sync_d <= pop_toggle_sync;
		if (pop_toggle_sync != pop_toggle_sync_d) rx_avail <= 1'b0;

		if (!eth_crs_dv) begin

			// carrier just dropped (or was never present) -- if we'd
			// locked onto a frame, this is the end of it. sfd_found/
			// byte_cnt/rx_crc below are all read at their PRE-edge
			// (i.e. final, as-of-last-active-cycle) values here,
			// same non-blocking-assignment trick used throughout:
			// the clears below don't take effect until next edge.
			if (sfd_found) begin
				if (rx_avail) begin
					// buffer still full -- software hasn't popped
					// the previous frame yet. drop this one.
					if (rx_drop_count != 4'hF) rx_drop_count <= rx_drop_count + 1'b1;
				end else if (byte_cnt >= 11'd64 && rx_crc == CRC32_RESIDUAL) begin
					// byte_cnt includes the 4-byte FCS (it has to, for
					// the CRC check above) but software wants the
					// same convention as enc28j60_recv()/eth.c: length
					// of the actual frame, FCS not included. The FCS
					// bytes are still sitting in rxbuf past this
					// length -- harmless, software just never reads
					// that far.
					rx_len <= byte_cnt - 11'd4;
					rx_avail <= 1'b1;
				end else begin
					if (rx_err_count != 4'hF) rx_err_count <= rx_err_count + 1'b1;
				end
			end

			sfd_found <= 0;
			dibit_cnt <= 0;
			byte_cnt <= 0;
			rx_crc <= 32'hFFFFFFFF;

		end else if (!sfd_found) begin

			// hunting for the SFD, continuously -- not gated by any
			// dibit counter, since we don't know/trust the preamble's
			// exact length or alignment. the instant the last 8 bits
			// received equal 0xD5, we're locked to a byte boundary.
			rx_shift <= new_byte;
			if (new_byte == 8'hD5) begin
				sfd_found <= 1;
				dibit_cnt <= 0;
				byte_cnt <= 0;
				rx_crc <= 32'hFFFFFFFF;
			end

		end else begin

			// locked on, receiving frame data. CRC runs every cycle
			// (2 bits/cycle) regardless of byte alignment; the byte
			// assembler and buffer write only fire once every 4
			// cycles, when dibit_cnt (pre-edge) reads 3.
			rx_shift <= new_byte;
			rx_crc <= crc_after_bit1;

			if (dibit_cnt == 2'b11) begin
				dibit_cnt <= 0;
				if (!rx_avail) begin
					case (byte_cnt[1:0])
						2'b00: rxbuf[byte_cnt[10:2]][7:0]   <= new_byte;
						2'b01: rxbuf[byte_cnt[10:2]][15:8]  <= new_byte;
						2'b10: rxbuf[byte_cnt[10:2]][23:16] <= new_byte;
						2'b11: rxbuf[byte_cnt[10:2]][31:24] <= new_byte;
					endcase
					if (byte_cnt != 11'd2047) byte_cnt <= byte_cnt + 1'b1;
				end
			end else begin
				dibit_cnt <= dibit_cnt + 1'b1;
			end

		end
	end

	// ===========================================================
	// PHASE 3: TX datapath
	// ===========================================================

	// TX packet buffer -- one frame at a time, mirror image of rxbuf:
	// only ever WRITTEN from the wb_clk_i side (CPU), only ever READ
	// from this section (eth_refclk side, TX engine). single writer.
	reg [31:0] txbuf [0:TXBUF_WORDS-1];

	localparam TX_IDLE     = 3'd0;
	localparam TX_PREAMBLE = 3'd1;
	localparam TX_DATA     = 3'd2;
	localparam TX_FCS      = 3'd3;
	localparam TX_IFG      = 3'd4;

	reg [2:0] tx_state = TX_IDLE;
	reg [7:0] tx_shift = 0;         // current byte, shifted out 2 bits/cycle, LSB first
	reg [1:0] tx_dibit = 0;         // 0..3 within the current byte
	reg [3:0] tx_preamble_idx = 0;  // 0..6 = 0x55, 7 = SFD (0xD5)
	reg [10:0] tx_byte_idx = 0;     // index into txbuf during TX_DATA
	reg [10:0] tx_len = 0;          // latched frame length (excl. FCS) for this transmission
	reg [31:0] tx_crc = 32'hFFFFFFFF;
	reg [31:0] tx_fcs_latched = 0;
	reg [1:0] tx_fcs_idx = 0;       // 0..3 within the FCS
	reg [5:0] tx_ifg_cnt = 0;
	reg tx_busy = 0;

	assign eth_tx_en = (tx_state == TX_PREAMBLE) || (tx_state == TX_DATA) || (tx_state == TX_FCS);
	assign eth_txd = eth_tx_en ? tx_shift[1:0] : 2'b00;

	// lookahead for the byte AFTER the one currently being sent --
	// needed because we have to load tx_shift with the next byte on
	// the same edge we advance tx_byte_idx (can't wait a cycle to
	// read txbuf[new tx_byte_idx], that value isn't there yet this
	// same edge). same idea as new_byte on the RX side, just for a
	// read instead of a write.
	wire [10:0] tx_next_byte_idx = tx_byte_idx + 11'd1;
	reg [7:0] tx_next_data_byte;
	always @(*) begin
		case (tx_next_byte_idx[1:0])
			2'b00: tx_next_data_byte = txbuf[tx_next_byte_idx[10:2]][7:0];
			2'b01: tx_next_data_byte = txbuf[tx_next_byte_idx[10:2]][15:8];
			2'b10: tx_next_data_byte = txbuf[tx_next_byte_idx[10:2]][23:16];
			2'b11: tx_next_data_byte = txbuf[tx_next_byte_idx[10:2]][31:24];
		endcase
	end

	// CRC over the bits actually being shifted out this cycle
	// (tx_shift[0] first, then tx_shift[1] -- transmission order,
	// same as the RX side's eth_rxd[0] then eth_rxd[1]).
	wire [31:0] txcrc_after_bit0 = crc32_update(tx_crc, tx_shift[0]);
	wire [31:0] txcrc_after_bit1 = crc32_update(txcrc_after_bit0, tx_shift[1]);

	// the actual FCS to transmit: the ONE'S COMPLEMENT of the CRC
	// register after the last data bit -- this is what makes it a
	// standard, real Ethernet FCS (matches zlib.crc32()) rather than
	// the RX side's residual-check trick, which only works because
	// it's comparing against a fixed constant, not producing bytes
	// that have to be independently valid on the wire.
	wire [31:0] tx_fcs_next = ~txcrc_after_bit1;

	reg tx_start_toggle_meta = 0;
	reg tx_start_toggle_sync = 0;
	reg tx_start_toggle_sync_d = 0;

	always @(posedge eth_refclk) begin

		// cross the wb_clk_i-domain start request in -- same
		// toggle-plus-edge-detect pattern as the RX side's pop.
		tx_start_toggle_meta <= tx_start_toggle;
		tx_start_toggle_sync <= tx_start_toggle_meta;
		tx_start_toggle_sync_d <= tx_start_toggle_sync;

		case (tx_state)

			TX_IDLE: begin
				tx_busy <= 1'b0;
				tx_dibit <= 0;
				tx_preamble_idx <= 0;
				if (tx_start_toggle_sync != tx_start_toggle_sync_d) begin
					tx_busy <= 1'b1;
					tx_len <= tx_len_reg;  // latch -- see tx_len_reg's comment below
					tx_shift <= 8'h55;
					tx_state <= TX_PREAMBLE;
				end
			end

			TX_PREAMBLE: begin
				tx_shift <= tx_shift >> 2;
				if (tx_dibit == 2'b11) begin
					tx_dibit <= 0;
					if (tx_preamble_idx == 4'd7) begin
						// that was the SFD -- start of frame data
						tx_state <= TX_DATA;
						tx_byte_idx <= 0;
						tx_crc <= 32'hFFFFFFFF;
						tx_shift <= txbuf[0][7:0];
					end else begin
						tx_preamble_idx <= tx_preamble_idx + 1'b1;
						tx_shift <= (tx_preamble_idx == 4'd6) ? 8'hD5 : 8'h55;
					end
				end else begin
					tx_dibit <= tx_dibit + 1'b1;
				end
			end

			TX_DATA: begin
				tx_shift <= tx_shift >> 2;
				tx_crc <= txcrc_after_bit1;
				if (tx_dibit == 2'b11) begin
					tx_dibit <= 0;
					if (tx_byte_idx == tx_len - 11'd1) begin
						// last data byte just went out -- FCS next
						tx_state <= TX_FCS;
						tx_fcs_idx <= 0;
						tx_fcs_latched <= tx_fcs_next;
						tx_shift <= tx_fcs_next[7:0];
					end else begin
						tx_byte_idx <= tx_next_byte_idx;
						tx_shift <= tx_next_data_byte;
					end
				end else begin
					tx_dibit <= tx_dibit + 1'b1;
				end
			end

			TX_FCS: begin
				tx_shift <= tx_shift >> 2;
				if (tx_dibit == 2'b11) begin
					tx_dibit <= 0;
					if (tx_fcs_idx == 2'b11) begin
						tx_state <= TX_IFG;
						tx_ifg_cnt <= 0;
					end else begin
						tx_fcs_idx <= tx_fcs_idx + 1'b1;
						case (tx_fcs_idx + 1'b1)
							2'b01: tx_shift <= tx_fcs_latched[15:8];
							2'b10: tx_shift <= tx_fcs_latched[23:16];
							default: tx_shift <= tx_fcs_latched[31:24]; // 2'b11
						endcase
					end
				end else begin
					tx_dibit <= tx_dibit + 1'b1;
				end
			end

			TX_IFG: begin
				// minimum inter-frame gap: 12 byte-times (48 dibit
				// cycles) of silence before the next transmission,
				// per IEEE 802.3 -- otherwise a fast poll-and-resend
				// loop from software could run frames together with
				// no gap for the link partner to resynchronize.
				if (tx_ifg_cnt == 6'd47) begin
					tx_state <= TX_IDLE;
				end else begin
					tx_ifg_cnt <= tx_ifg_cnt + 1'b1;
				end
			end

			default: tx_state <= TX_IDLE;

		endcase
	end

	// ===========================================================
	// WISHBONE SLAVE (wb_clk_i domain)
	// ===========================================================

	// tx_busy crosses via a real 2-flop synchronizer, same as
	// crs_dv/refclk_hb/rx_avail.
	reg tx_busy_meta = 0;
	reg tx_busy_sync = 0;

	always @(posedge wb_clk_i) begin
		tx_busy_meta <= tx_busy;
		tx_busy_sync <= tx_busy_meta;
	end

	// tx_len_reg crosses WITHOUT a bit-by-bit synchronizer -- same
	// "quasi-static" reasoning as rx_len_sync above, just the other
	// direction: software writes TX_LEN, then (a separate, in-order,
	// unposted wishbone write, so it's fully complete first) writes
	// TX_CTRL. TX_CTRL's toggle then has to cross the CDC handshake
	// (several eth_refclk cycles) before TX_IDLE's edge-detect even
	// fires and reads tx_len_reg -- by which point it's long since
	// settled and nothing is still writing it.
	reg [10:0] tx_len_reg = 0;
	reg tx_start_toggle = 0;

	// rx_avail crosses via a real 2-flop synchronizer, same as
	// crs_dv/refclk_hb above.
	reg rx_ready_meta = 0;
	reg rx_ready_sync = 0;

	always @(posedge wb_clk_i) begin
		rx_ready_meta <= rx_avail;
		rx_ready_sync <= rx_ready_meta;
	end

	// rx_len crosses WITHOUT a bit-by-bit synchronizer -- normally
	// unsafe for a multi-bit value, but safe here because rx_len is
	// quasi-static: it's written in the eth_refclk domain on the
	// exact same edge rx_avail is set, and never changes again until
	// software pops the buffer and a whole new frame completes. By
	// the time rx_ready_sync above has propagated (a handful of
	// eth_refclk + wb_clk_i cycles after that edge), rx_len has long
	// since settled and nothing is still writing it. Software must
	// still only trust this value while rx_ready_sync (STATUS bit 2)
	// reads 1 -- same rule as the buffer contents below.
	reg [10:0] rx_len_sync = 0;

	always @(posedge wb_clk_i) begin
		rx_len_sync <= rx_len;
	end

	// pop request: a toggle, not a pulse -- avoids the request being
	// missed if it lands on the "wrong" eth_refclk edge relative to a
	// single-cycle pulse. cleared by nothing; it just keeps toggling.
	reg pop_toggle = 0;

	always @(posedge wb_clk_i) begin

		wb_ack_o <= 1'b0;

		if (wb_rst_i) begin
			pop_toggle <= 1'b0;
			tx_start_toggle <= 1'b0;
			tx_len_reg <= 0;
		end else if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin

			if (wb_we_i) begin
				if (wb_adr_i == REG_RXCTRL) begin
					pop_toggle <= ~pop_toggle;
				end else if (wb_adr_i == REG_TXLEN) begin
					tx_len_reg <= wb_dat_i[10:0];
				end else if (wb_adr_i == REG_TXCTRL) begin
					tx_start_toggle <= ~tx_start_toggle;
				end else if (wb_adr_i >= TXBUF_BASE && wb_adr_i < (TXBUF_BASE + TXBUF_WORDS)) begin
					if (wb_sel_i[0]) txbuf[wb_adr_i - TXBUF_BASE][7:0]   <= wb_dat_i[7:0];
					if (wb_sel_i[1]) txbuf[wb_adr_i - TXBUF_BASE][15:8]  <= wb_dat_i[15:8];
					if (wb_sel_i[2]) txbuf[wb_adr_i - TXBUF_BASE][23:16] <= wb_dat_i[23:16];
					if (wb_sel_i[3]) txbuf[wb_adr_i - TXBUF_BASE][31:24] <= wb_dat_i[31:24];
				end
			end else begin
				if (wb_adr_i == REG_STATUS)
					wb_dat_o <= { 20'b0, rx_err_count, rx_drop_count,
						tx_busy_sync, rx_ready_sync, refclk_hb_sync, crs_dv_sync };
				else if (wb_adr_i == REG_RXLEN)
					wb_dat_o <= { 21'b0, rx_len_sync };
				else if (wb_adr_i >= RXBUF_BASE && wb_adr_i < (RXBUF_BASE + RXBUF_WORDS))
					wb_dat_o <= rxbuf[wb_adr_i - RXBUF_BASE];
				else if (wb_adr_i >= TXBUF_BASE && wb_adr_i < (TXBUF_BASE + TXBUF_WORDS))
					wb_dat_o <= txbuf[wb_adr_i - TXBUF_BASE];
				else
					wb_dat_o <= 32'h0;
			end

			wb_ack_o <= 1'b1;

		end
	end

endmodule
