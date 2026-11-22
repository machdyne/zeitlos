/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The XA-XD link to the carrier's RP2040 (Sergei, Mozart), `XLINK: an
 * SPI slave with two message buffers. machboot's rtl/mb_xlink.v, the
 * same wire protocol and the same registers, so the RP2040's firmware
 * (machboot-sv) talks to either; here at 0x6400_0000, with msg_o for
 * the interrupt. What Zeitlos says over it is docs/xlink.md: netx's
 * Ethernet frames (sw/apps/net/xlink_eth.c).
 *
 * The RP2040 is the master (its SPI1: SCK on XB, MOSI on XA, MISO on XC,
 * and CS on XD as a GPIO), mode 0, MSB first. The pins are sampled at
 * the system clock through two flip-flops, so SCK must stay below about
 * a tenth of it: 6.25 MHz against 48 MHz. MISO changes just after the
 * rising edge the master sampled at (as early as the next bit may), so
 * the master has nearly a whole period to see it.
 *
 * A frame is CS low ... CS high. Its first byte is a command; while it
 * goes in, the slave sends 0x5A, so the master can tell machboot's link
 * from a pin nobody drives.
 *
 *   0x01 STATUS  the slave sends: FLAGS, STATE, TXLEN[7:0], TXLEN[10:8],
 *                RXSEQ, then zeros.
 *                  STATE bit 0: the receive buffer is full (the CPU has
 *                               not taken the last message yet)
 *                        bit 1: a reply is ready (TXLEN bytes)
 *                  FLAGS: a byte the CPU sets (docs/xlink.md)
 *                  RXSEQ: counts the messages taken in
 *   0x02 SEND    the master's bytes go into the receive buffer from 0.
 *                At CS high, if the buffer was empty when the frame
 *                began and a byte came in, it is full: RXLEN bytes,
 *                RXSEQ + 1. Otherwise nothing changes.
 *   0x03 RECV    the slave sends the reply buffer from 0. At CS high, if
 *                a reply was ready and at least TXLEN bytes went out, the
 *                reply is taken (STATE bit 1 clears).
 *
 * The CPU side (Wishbone, 32-bit words):
 *
 *   0x0000  MAGIC   r   "XLNK"
 *   0x0004  RX      r   bit 0 full, bits 26:16 length, bits 15:8 RXSEQ
 *                   w   bit 0: 1 = empty again (take the next message)
 *   0x0008  TX      r   bit 0 a reply is armed
 *                   w   bits 10:0 length (1-2048; 0 means 2048), bit 31 arm;
 *                       bit 31 clear: disarm
 *   0x000C  FLAGS   rw  bits 7:0, sent in STATUS
 *   0x0010  FRAMES  r   frames seen since reset (for "is the RP2040 there")
 *   0x1000-0x17FF   the receive buffer, read
 *   0x1800-0x1FFF   the reply buffer, write (byte lanes by sel)
 *
 * Each buffer is 512 x 32 with a write port and a read port: one block
 * RAM each.
 */

module xlink (
	input clk,
	input rst,

	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input [3:0] wb_sel_i,
	input wb_we_i,
	input wb_stb_i,
	input wb_cyc_i,
	output reg wb_ack_o,

	input x_sck,
	input x_mosi,
	input x_cs_n,
	output x_miso,
	output x_miso_oe,

	output msg_o		// level: a message waits in the receive buffer
);

	// -- the pins, at the system clock -----------------------------------------

	reg [2:0] sck_s = 3'b000;
	reg [1:0] mosi_s = 2'b00;
	reg [2:0] cs_s = 3'b111;

	always @(posedge clk) begin
		sck_s <= {sck_s[1:0], x_sck};
		mosi_s <= {mosi_s[0], x_mosi};
		cs_s <= {cs_s[1:0], x_cs_n};
	end

	wire active = !cs_s[1];
	wire sck_rise = active && (sck_s[2:1] == 2'b01);
	wire cs_fall = (cs_s[2:1] == 2'b10);
	wire cs_rise = (cs_s[2:1] == 2'b01);

	// -- the buffers ---------------------------------------------------------------

	reg [31:0] rxmem [0:511];
	reg [31:0] txmem [0:511];

	// receive buffer: written by the link a byte at a time, read by the CPU
	reg rx_we = 1'b0;
	reg [10:0] rx_wa = 11'd0;
	reg [7:0] rx_wd = 8'd0;
	reg [31:0] rx_rd = 32'd0;

	always @(posedge clk) begin
		if (rx_we && rx_wa[1:0] == 2'd0) rxmem[rx_wa[10:2]][7:0] <= rx_wd;
		if (rx_we && rx_wa[1:0] == 2'd1) rxmem[rx_wa[10:2]][15:8] <= rx_wd;
		if (rx_we && rx_wa[1:0] == 2'd2) rxmem[rx_wa[10:2]][23:16] <= rx_wd;
		if (rx_we && rx_wa[1:0] == 2'd3) rxmem[rx_wa[10:2]][31:24] <= rx_wd;
		rx_rd <= rxmem[wb_adr_i[10:2]];
	end

	// reply buffer: written by the CPU, read by the link
	wire tx_cpu_we = wb_cyc_i && wb_stb_i && wb_we_i && !wb_ack_o && (wb_adr_i[12:11] == 2'b11);
	reg [10:0] tx_ra = 11'd0;
	reg [31:0] tx_rd = 32'd0;

	always @(posedge clk) begin
		if (tx_cpu_we && wb_sel_i[0]) txmem[wb_adr_i[10:2]][7:0] <= wb_dat_i[7:0];
		if (tx_cpu_we && wb_sel_i[1]) txmem[wb_adr_i[10:2]][15:8] <= wb_dat_i[15:8];
		if (tx_cpu_we && wb_sel_i[2]) txmem[wb_adr_i[10:2]][23:16] <= wb_dat_i[23:16];
		if (tx_cpu_we && wb_sel_i[3]) txmem[wb_adr_i[10:2]][31:24] <= wb_dat_i[31:24];
		tx_rd <= txmem[tx_ra[10:2]];
	end

	// -- the link ------------------------------------------------------------------

	wire cpu_reg_we = wb_cyc_i && wb_stb_i && wb_we_i && !wb_ack_o && (wb_adr_i[12] == 1'b0);

	localparam [7:0] SYNC = 8'h5A;
	localparam [7:0] C_STATUS = 8'h01;
	localparam [7:0] C_SEND = 8'h02;
	localparam [7:0] C_RECV = 8'h03;

	reg [7:0] flags = 8'd0;
	reg rx_full = 1'b0;
	reg [10:0] rx_len = 11'd0;
	reg [7:0] rx_seq = 8'd0;
	reg tx_armed = 1'b0;
	reg [10:0] tx_len = 11'd0;		// 0: 2048
	reg [15:0] frames = 16'd0;

	reg [2:0] bitn = 3'd0;			// bits of the current byte so far
	reg [7:0] sh_in = 8'd0;
	reg [7:0] sh_out = SYNC;
	reg [11:0] nbyte = 12'd0;		// whole bytes of this frame so far
	reg [7:0] cmd = 8'd0;
	reg take = 1'b0;			// SEND: the buffer was empty when the frame began
	reg give = 1'b0;			// RECV: a reply was armed when the frame began
	reg [11:0] sent = 12'd0;		// RECV: reply bytes that went out whole
	reg [1:0] lane = 2'd0;			// which byte of tx_rd the next reply byte is

	wire [7:0] in_byte = {sh_in[6:0], mosi_s[1]};
	wire [11:0] tx_full_len = (tx_len == 11'd0) ? 12'd2048 : {1'b0, tx_len};
	wire [7:0] tx_byte = (lane == 2'd0) ? tx_rd[7:0] : (lane == 2'd1) ? tx_rd[15:8] :
	                     (lane == 2'd2) ? tx_rd[23:16] : tx_rd[31:24];

	// what goes out next, once byte number nbyte+1 (counting from 0) is done
	wire [7:0] cmd_now = (nbyte == 12'd0) ? in_byte : cmd;
	reg [7:0] next_out;
	always @(*) begin
		next_out = 8'h00;
		if (cmd_now == C_STATUS) begin
			if (nbyte == 12'd0) next_out = flags;
			else if (nbyte == 12'd1) next_out = {6'd0, tx_armed, rx_full};
			else if (nbyte == 12'd2) next_out = tx_len[7:0];
			else if (nbyte == 12'd3) next_out = {5'd0, tx_len[10:8]};
			else if (nbyte == 12'd4) next_out = rx_seq;
		end else if (cmd_now == C_RECV) begin
			next_out = tx_byte;
		end
	end

	assign x_miso = sh_out[7];
	assign x_miso_oe = active;
	assign msg_o = rx_full;

	always @(posedge clk) begin
		rx_we <= 1'b0;
		if (rst) begin
			bitn <= 3'd0;
			nbyte <= 12'd0;
			sh_out <= SYNC;
			rx_full <= 1'b0;
			tx_armed <= 1'b0;
			rx_seq <= 8'd0;
			frames <= 16'd0;
		end else begin
			if (cpu_reg_we && wb_adr_i[4:2] == 3'd1 && wb_dat_i[0])
				rx_full <= 1'b0;
			if (cs_fall) begin
				bitn <= 3'd0;
				nbyte <= 12'd0;
				sh_out <= SYNC;
				cmd <= 8'd0;
				take <= !rx_full;
				give <= tx_armed;
				sent <= 12'd0;
				tx_ra <= 11'd0;		// the first reply byte, fetched now
				lane <= 2'd0;
			end else if (sck_rise) begin
				bitn <= bitn + 3'd1;
				if (bitn == 3'd7) begin
					// a whole byte
					sh_in <= in_byte;
					sh_out <= next_out;
					nbyte <= nbyte + 12'd1;
					if (nbyte == 12'd0)
						cmd <= in_byte;
					if (cmd_now == C_SEND && nbyte != 12'd0 && take && nbyte <= 12'd2048) begin
						rx_we <= 1'b1;
						rx_wa <= nbyte[10:0] - 11'd1;
						rx_wd <= in_byte;
					end
					if (cmd_now == C_RECV) begin
						// next_out was reply byte nbyte (from 0); fetch the one after
						if (nbyte != 12'd0)
							sent <= sent + 12'd1;
						tx_ra <= nbyte[10:0] + 11'd1;
						lane <= nbyte[1:0] + 2'd1;
					end
				end else begin
					sh_in <= in_byte;
					sh_out <= {sh_out[6:0], 1'b0};
				end
			end
			if (cs_rise) begin
				frames <= frames + 16'd1;
				if (cmd == C_SEND && take && nbyte > 12'd1) begin
					rx_full <= 1'b1;
					rx_len <= (nbyte > 12'd2048) ? 11'd0 : nbyte[10:0] - 11'd1;
					rx_seq <= rx_seq + 8'd1;
				end
				// RECV: the reply is taken once all of it went out (counting the
				// byte that was on its way when CS rose only if it was whole)
				if (cmd == C_RECV && give && sent >= tx_full_len)
					tx_armed <= 1'b0;
			end
			// the CPU arming a reply comes after the link taking one: a new
			// reply armed in the same cycle stays armed
			if (cpu_reg_we && wb_adr_i[4:2] == 3'd2) begin
				tx_armed <= wb_dat_i[31];
				tx_len <= wb_dat_i[10:0];
			end
			if (cpu_reg_we && wb_adr_i[4:2] == 3'd3)
				flags <= wb_dat_i[7:0];
		end
	end

	// -- the CPU's reads -------------------------------------------------------

	// The receive buffer's block RAM answers a cycle after the address:
	// a read there acks one cycle later than a register.
	wire rx_area = (wb_adr_i[12:11] == 2'b10);
	reg rd_wait = 1'b0;

	always @(posedge clk) begin
		wb_ack_o <= 1'b0;
		rd_wait <= 1'b0;
		if (wb_cyc_i && wb_stb_i && !wb_ack_o && !rst) begin
			if (rx_area && !wb_we_i && !rd_wait)
				rd_wait <= 1'b1;
			else
				wb_ack_o <= 1'b1;
		end
		if (rx_area)
			wb_dat_o <= rx_rd;
		else
			case (wb_adr_i[4:2])
			3'd0: wb_dat_o <= 32'h4B4E_4C58;	// "XLNK"
			3'd1: wb_dat_o <= {5'd0, rx_len, rx_seq, 7'd0, rx_full};
			3'd2: wb_dat_o <= {31'd0, tx_armed};
			3'd3: wb_dat_o <= {24'd0, flags};
			3'd4: wb_dat_o <= {16'd0, frames};
			default: wb_dat_o <= 32'd0;
			endcase
	end

endmodule
