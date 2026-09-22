/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SPI flash controller: the memory-mapped flash, read as before, and a
 * small register block that erases and programs it -- in hardware, with
 * the bootloader out of reach. docs/spiflash.md.
 *
 * Replaces rtl/spiflashro.v, which only read. One controller owns the
 * four pins, so there is nothing to arbitrate: a read of the window that
 * arrives while an erase is in progress waits for the erase to finish.
 *
 * -- The map (the 0x1 nibble, byte addresses, as rtl/sysctl.v passes
 *    them: bits 27:0) --
 *
 *   0x1000_0000 - 0x1EFF_FFFF   the flash, read-only; flash offset is the
 *                               low 24 bits. A write here is acknowledged
 *                               and ignored (spiflashro left it
 *                               unacknowledged, which hung the bus).
 *   0x1F00_0000 - 0x1F00_00FF   registers, below
 *   0x1F00_0100 - 0x1F00_01FF   the page buffer: 256 bytes, as 64 words,
 *                               little-endian -- the byte at the lowest
 *                               flash address is bits 7:0, as reads are.
 *                               Write-only: it reads back as 0.
 *
 * -- Registers (0x1F00_0000 + offset) --
 *
 *   0x00 MAGIC   0x5A46_4C53 ("ZFLS")
 *   0x04 STATUS  bit 0 busy (an operation, or the ID read that follows
 *                reset or command 3); bit 1 done (the last operation completed);
 *                bit 2 refused: the address is in the locked region;
 *                bit 3 refused: not armed; bit 4 refused: the program
 *                would cross a 256-byte page, or LEN is 0 or over 256;
 *                bit 5 refused: busy; bits 15:8 the flash's status
 *                register, as last polled. Bits 5:1 clear when a
 *                command is written.
 *   0x08 ID      the JEDEC ID, { 8'h00, manufacturer, type, capacity },
 *                read at reset (EF 40 15 on a W25Q16); valid once STATUS
 *                busy is clear
 *   0x0C LOCK    the end of the locked region: LOCK_END, 0x040000
 *   0x10 ADDR    the flash address for the next command (24 bits)
 *   0x14 ARM     write 0x5A46_5752 ("ZFWR"), as a whole word, to allow ONE
 *                erase or program. Any other write here disarms.
 *   0x18 CMD     write 1: erase the 4 KB sector containing ADDR
 *                write 2: program LEN bytes from the page buffer at ADDR
 *                write 3: read the JEDEC ID again (no arming needed)
 *   0x1C LEN     bytes to program, 1 to 256
 *
 * -- Why it cannot erase the bootloader --
 *
 * There is no raw SPI path. The controller knows exactly four command
 * sequences -- read, erase a 4 KB sector, program a page, read the ID
 * -- plus the WREN and status polling they need, and sends nothing
 * else: no chip erase (C7h/60h), no status register write, no block
 * erase. Every erase and program is checked against LOCK_END BEFORE
 * any pin moves, and one in the locked region is refused with a flag
 * and never reaches the flash. Software, of any kind, with any key,
 * cannot get past that. (Another bitstream can: see docs/zboot.md
 * section 7.) LOCK_END is 0x040000 on every board -- the DFU
 * bootloader's 256 KB; on a board flashed without one it is the start
 * of Zeitlos's own gateware, which then cannot be rewritten from
 * Zeitlos either.
 *
 * -- Timing --
 *
 * The shift engine is spiflashro's, unchanged: SPI mode 0, one bit per
 * two clocks, MISO sampled on the clock SCK rises, MOSI updated with
 * it, SCK and MOSI released (tri-stated) whenever no command is on the
 * wire. Every command -- not only reads -- goes through it, so writes
 * have the timing reads already have on real boards. Between commands
 * chip select is high for GAP clocks: W25Q16 wants 50 ns before an
 * erase or program, and 8 clocks at 48 MHz is 167 ns.
 */
module spiflash_wb #(
	parameter [23:0] LOCK_END = 24'h040000,
	parameter GAP = 8
) (
	input wb_clk_i,
	input wb_rst_i,

	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input [3:0] wb_sel_i,
	input wb_we_i,
	input wb_cyc_i,
	input wb_stb_i,
	output wb_ack_o,

	output reg ss,
	output sck,
	output mosi,
	input miso
);

	localparam [31:0] MAGIC   = 32'h5A46_4C53;	// "ZFLS"
	localparam [31:0] ARM_KEY = 32'h5A46_5752;	// "ZFWR"

	localparam [7:0]
		SPI_READ  = 8'h03,
		SPI_RDSR  = 8'h05,
		SPI_WREN  = 8'h06,
		SPI_PP    = 8'h02,
		SPI_SE    = 8'h20,
		SPI_RDID  = 8'h9F;

	// -- the bus side ------------------------------------------------------

	reg ack;
	assign wb_ack_o = ack;

	wire req = wb_cyc_i && wb_stb_i && !ack;
	wire is_reg = (wb_adr_i[27:24] == 4'hF);
	wire [8:0] roff = wb_adr_i[8:0];

	// A read of the window is served by the sequencer, when it is free;
	// everything else -- registers, and window writes -- is answered at
	// once.
	wire win_read = req && !is_reg && !wb_we_i;

	// -- registers -------------------------------------------------------------

	reg [23:0] addr;
	reg [8:0] len;
	reg armed;
	reg busy;
	reg done, e_lock, e_arm, e_len, e_busy;
	reg [7:0] sr1;
	reg [23:0] id;

	// LUT RAM, not a block RAM: Lakritz uses 54 of its 56, and they are
	// the tightest resource on the board (docs/usb_host.md). ~60 LUTs
	// more than a block RAM would cost. docs/spiflash.md.
	(* ram_style = "distributed" *) reg [31:0] pbuf [0:63];

	// -- the shift engine: spiflashro's, unchanged ---------------------------

	reg [31:0] buffer;
	reg [5:0] xfer_bits;
	reg mosi_do;
	reg sck_do;
	reg drive;
	assign mosi = drive ? mosi_do : 1'bz;
	assign sck = drive ? sck_do : 1'bz;

	// -- the sequencer -----------------------------------------------------------

	localparam [4:0]
		S_IDLE     = 5'd0,
		S_R_START  = 5'd1,	// window read: spiflashro's INIT, START, CMD
		S_R_ADDR   = 5'd2,
		S_R_DATA   = 5'd3,
		S_R_END    = 5'd4,
		S_WREN     = 5'd5,	// erase / program
		S_WREN_END = 5'd6,
		S_OP_START = 5'd7,
		S_OP_ADDR  = 5'd8,
		S_OP_BYTE  = 5'd9,
		S_OP_END   = 5'd10,
		S_POLL     = 5'd11,
		S_POLL_RD  = 5'd12,
		S_POLL_END = 5'd13,
		S_ID       = 5'd14,
		S_ID_RD    = 5'd15,
		S_ID_END   = 5'd16,
		S_GAP      = 5'd17,
		S_R_CS     = 5'd18,
		S_R_CMD    = 5'd19;

	reg [4:0] state, after_gap;
	reg [7:0] gap;
	reg [1:0] op;			// 1 erase, 2 program
	reg [8:0] bidx;			// page-buffer byte index while programming
	reg id_pending;

	// The program path reads the buffer synchronously: the word for byte
	// bidx is registered a clock after bidx changes, and there are 16
	// clocks of shifting between bytes (48 before the first), so it is
	// always ready. An asynchronous read here cost a second copy of the
	// buffer in LUT RAM, or forced one into a block RAM anyway.
	reg [31:0] bword;
	wire [7:0] bbyte = bword >> {bidx[1:0], 3'b000};

	// a command register write, and whether it is allowed
	wire cmd_wr = req && is_reg && wb_we_i && roff == 9'h018 && wb_sel_i[0];
	wire [23:0] target = (wb_dat_i[1:0] == 2'd1) ? { addr[23:12], 12'h000 } : addr;
	wire locked = target < LOCK_END;
	wire bad_len = (len == 9'd0) || (len > 9'd256) || ({1'b0, addr[7:0]} + len > 9'd256);

	always @(posedge wb_clk_i) begin
		bword <= pbuf[bidx[7:2]];
		if (req && is_reg && wb_we_i && roff[8] && !busy) begin
			if (wb_sel_i[0]) pbuf[wb_adr_i[7:2]][7:0]   <= wb_dat_i[7:0];
			if (wb_sel_i[1]) pbuf[wb_adr_i[7:2]][15:8]  <= wb_dat_i[15:8];
			if (wb_sel_i[2]) pbuf[wb_adr_i[7:2]][23:16] <= wb_dat_i[23:16];
			if (wb_sel_i[3]) pbuf[wb_adr_i[7:2]][31:24] <= wb_dat_i[31:24];
		end
	end

	always @(posedge wb_clk_i) begin
		if (wb_rst_i) begin
			ack <= 0;
			ss <= 1;
			sck_do <= 0;
			mosi_do <= 0;
			drive <= 0;
			xfer_bits <= 0;
			state <= S_IDLE;
			addr <= 0;
			len <= 0;
			armed <= 0;
			busy <= 0;
			done <= 0;
			e_lock <= 0;
			e_arm <= 0;
			e_len <= 0;
			e_busy <= 0;
			sr1 <= 0;
			id <= 0;
			op <= 0;
			id_pending <= 1;		// read the JEDEC ID first thing
			wb_dat_o <= 0;
		end else begin

			// -- registers and window writes: one clock ------------------
			// ack is a one-clock pulse, as spiflashro's was
			if (ack) ack <= 0;
			if (req && (is_reg || wb_we_i) && !ack) begin
				ack <= 1;
				if (is_reg && !wb_we_i) begin
					// The page buffer is write-only: reading it back
					// cost ~600 LUTs for a debugging convenience.
					if (roff[8])
						wb_dat_o <= 32'h0;
					else case (roff[7:2])
						6'h0: wb_dat_o <= MAGIC;
						// busy covers the ID read too, from reset: wait
						// for it before trusting ID
						6'h1: wb_dat_o <= { 16'h0, sr1, 2'b0, e_busy, e_len, e_arm, e_lock, done, busy | id_pending };
						6'h2: wb_dat_o <= { 8'h00, id };
						6'h3: wb_dat_o <= { 8'h00, LOCK_END };
						6'h4: wb_dat_o <= { 8'h00, addr };
						6'h5: wb_dat_o <= { 31'h0, armed };
						6'h7: wb_dat_o <= { 23'h0, len };
						default: wb_dat_o <= 32'h0;
					endcase
				end else if (is_reg && wb_we_i && !roff[8]) begin
					case (roff[7:2])
						6'h4: if (!busy) addr <= wb_dat_i[23:0];
						6'h5: armed <= (wb_sel_i == 4'b1111 && wb_dat_i == ARM_KEY);
						6'h7: if (!busy) len <= wb_dat_i[8:0];
						default: ;
					endcase
				end
			end

			// -- a command: checked here, before anything moves ----------
			if (cmd_wr && !ack) begin
				done <= 0; e_lock <= 0; e_arm <= 0; e_len <= 0; e_busy <= 0;
				if (busy) begin
					e_busy <= 1;
				end else if (wb_dat_i[1:0] == 2'd3) begin
					id_pending <= 1;
				end else if (wb_dat_i[1:0] == 2'd1 || wb_dat_i[1:0] == 2'd2) begin
					if (!armed) e_arm <= 1;
					else if (locked) e_lock <= 1;
					else if (wb_dat_i[1:0] == 2'd2 && bad_len) e_len <= 1;
					else begin
						op <= wb_dat_i[1:0];
						busy <= 1;
					end
					armed <= 0;		// one command per arming, allowed or not
				end
			end

			// -- the shift engine --------------------------------------------
			if (xfer_bits) begin
				mosi_do <= buffer[31];
				if (sck_do) begin
					sck_do <= 0;
				end else begin
					sck_do <= 1;
					buffer <= { buffer, miso };
					xfer_bits <= xfer_bits - 1;
				end
			end else case (state)

				S_IDLE: begin
					ss <= 1;
					drive <= 0;
					sck_do <= 0;
					if (busy && op != 0) begin
						state <= S_WREN;
					end else if (id_pending) begin
						state <= S_ID;
					end else if (win_read) begin
						state <= S_R_START;
					end
				end

				// -- a window read: spiflashro's transaction, state for state
				S_R_START: begin		// its STATE_INIT
					drive <= 1;
					sck_do <= 0;
					state <= S_R_CS;
				end
				S_R_CS: begin			// its STATE_START
					ss <= 0;
					state <= S_R_CMD;
				end
				S_R_CMD: begin			// its STATE_CMD
					buffer[31:24] <= SPI_READ;
					xfer_bits <= 8;
					state <= S_R_ADDR;
				end
				S_R_ADDR: begin
					buffer[31:8] <= wb_adr_i[23:0];
					xfer_bits <= 24;
					state <= S_R_DATA;
				end
				S_R_DATA: begin
					xfer_bits <= 32;
					state <= S_R_END;
				end
				S_R_END: begin
					wb_dat_o <= { buffer[7:0], buffer[15:8], buffer[23:16], buffer[31:24] };
					ack <= 1;
					ss <= 1;
					drive <= 0;
					state <= S_IDLE;
				end

				// -- WREN, then the erase or the program ----------------------
				S_WREN: begin
					drive <= 1;
					ss <= 0;
					buffer[31:24] <= SPI_WREN;
					xfer_bits <= 8;
					state <= S_WREN_END;
				end
				S_WREN_END: begin
					ss <= 1;
					gap <= GAP;
					after_gap <= S_OP_START;
					state <= S_GAP;
				end
				S_OP_START: begin
					ss <= 0;
					buffer[31:24] <= (op == 2'd1) ? SPI_SE : SPI_PP;
					xfer_bits <= 8;
					state <= S_OP_ADDR;
				end
				S_OP_ADDR: begin
					buffer[31:8] <= (op == 2'd1) ? { addr[23:12], 12'h000 } : addr;
					xfer_bits <= 24;
					bidx <= 0;
					state <= (op == 2'd1) ? S_OP_END : S_OP_BYTE;
				end
				S_OP_BYTE: begin
					if (bidx == len) begin
						state <= S_OP_END;
					end else begin
						buffer[31:24] <= bbyte;
						xfer_bits <= 8;
						bidx <= bidx + 1;
					end
				end
				S_OP_END: begin
					ss <= 1;		// the flash starts erasing or programming now
					gap <= GAP;
					after_gap <= S_POLL;
					state <= S_GAP;
				end

				// -- poll the status register until WIP clears -------------
				S_POLL: begin
					ss <= 0;
					buffer[31:24] <= SPI_RDSR;
					xfer_bits <= 8;
					state <= S_POLL_RD;
				end
				S_POLL_RD: begin
					xfer_bits <= 8;
					state <= S_POLL_END;
				end
				S_POLL_END: begin
					ss <= 1;
					sr1 <= buffer[7:0];
					gap <= GAP;
					if (buffer[0]) begin
						after_gap <= S_POLL;		// still in progress
					end else begin
						busy <= 0;
						done <= 1;
						op <= 0;
						after_gap <= S_IDLE;
					end
					state <= S_GAP;
				end

				// -- the JEDEC ID ----------------------------------------------
				S_ID: begin
					drive <= 1;
					ss <= 0;
					buffer[31:24] <= SPI_RDID;
					xfer_bits <= 8;
					state <= S_ID_RD;
				end
				S_ID_RD: begin
					xfer_bits <= 24;
					state <= S_ID_END;
				end
				S_ID_END: begin
					ss <= 1;
					id <= buffer[23:0];
					id_pending <= 0;
					gap <= GAP;
					after_gap <= S_IDLE;
					state <= S_GAP;
				end

				// -- chip select high between commands ---------------------
				S_GAP: begin
					sck_do <= 0;
					if (gap) gap <= gap - 1;
					else begin
						if (after_gap == S_IDLE) drive <= 0;
						state <= after_gap;
					end
				end

				default: state <= S_IDLE;

			endcase
		end
	end

endmodule
