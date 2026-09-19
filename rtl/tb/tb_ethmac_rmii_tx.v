`timescale 1ns/1ps
/*
 * ethmac_rmii_wb -- transmit path, and TX-to-RX loopback.
 *
 * Written when the TX buffer moved from an asynchronous read (LUT RAM)
 * to a registered one (block RAM) -- see "BLOCK RAM" in
 * rtl/ethmac_rmii.v's header. tb_ethmac_rmii.v covers receive only, so
 * the transmit engine had no test at all, and a one-cycle-early
 * prefetch is exactly the kind of change that is wrong by one byte
 * rather than wrong everywhere.
 *
 * Loads frames into TX_BUF over wishbone exactly as
 * sw/apps/net/rmii_eth.c does (whole words, then TX_LEN, then
 * TX_CTRL, then poll tx_busy), captures eth_txd/eth_tx_en dibit by
 * dibit, and checks the wire:
 *
 *   1  7 x 0x55 preamble, then SFD 0xD5
 *   2  every data byte, in order -- at lengths chosen to end in each
 *      of the four byte lanes of a word
 *   3  the FCS equals an independent CRC32 of the data (reflected
 *      0xEDB88320, init and final invert 0xFFFFFFFF -- zlib's)
 *   4  tx_en drops right after the FCS, and the engine honours the
 *      12-byte inter-frame gap before a second frame
 *   5  a byte-lane write into TX_BUF changes exactly that byte on
 *      the wire, and STATUS[15:12] reports log2 of the RX slot count
 *
 * Then the loopback: eth_txd/eth_tx_en are wired into eth_rxd/
 * eth_crs_dv, and a transmitted frame has to come back through the
 * receive FIFO with the right length and contents -- which checks the
 * TX FCS against the RX residual check, two implementations that were
 * written separately.
 *
 * Checks 1-4 and 6 pass against the pre-BRAM RTL too (asynchronous
 * read), which is what makes them evidence that the two are the same
 * machine rather than a test written to fit the new one. Check 5
 * covers the two deliberate interface changes: TX_BUF no longer reads
 * back, and STATUS now carries the slot count.
 *
 *   iverilog -g2005 -o /tmp/tbtx rtl/tb/tb_ethmac_rmii_tx.v rtl/ethmac_rmii.v
 *   vvp /tmp/tbtx
 */
module tb_ethmac_rmii_tx;

	reg wb_clk = 0, wb_rst = 1;
	reg refclk = 0;
	always #10.4 wb_clk = ~wb_clk;   // ~48MHz, the real system clock
	always #10   refclk = ~refclk;   // 50MHz RMII reference, unrelated

	reg [31:0] wb_adr = 0, wb_dat = 0;
	reg wb_we = 0, wb_stb = 0, wb_cyc = 0;
	reg [3:0] wb_sel = 4'hF;
	wire [31:0] wb_dat_o;
	wire wb_ack;

	wire [1:0] eth_txd;
	wire eth_tx_en, eth_rst_n, eth_int;

	// Loopback switch: 0 = RX idle, 1 = TX looped into RX.
	reg loop = 0;
	wire [1:0] eth_rxd = loop ? eth_txd : 2'b00;
	wire eth_crs_dv = loop ? eth_tx_en : 1'b0;

	ethmac_rmii_wb dut(
		.wb_clk_i(wb_clk), .wb_rst_i(wb_rst),
		.wb_adr_i(wb_adr), .wb_dat_i(wb_dat), .wb_dat_o(wb_dat_o),
		.wb_we_i(wb_we), .wb_sel_i(wb_sel), .wb_stb_i(wb_stb),
		.wb_cyc_i(wb_cyc), .wb_ack_o(wb_ack),
		.eth_refclk(refclk),
		.eth_rxd(eth_rxd), .eth_txd(eth_txd), .eth_tx_en(eth_tx_en),
		.eth_crs_dv(eth_crs_dv), .eth_rst_n(eth_rst_n),
		.eth_int_o(eth_int));

	localparam REG_STATUS = 0, REG_RXLEN = 1, REG_RXCTRL = 2;
	localparam REG_TXLEN = 3, REG_TXCTRL = 4;
	localparam RXBUF_BASE = 64, TXBUF_BASE = 640;

	// Follows the DUT: build with -DETH_RX_SLOTS=n to test another size.
`ifdef ETH_RX_SLOTS
	localparam RX_SLOTS = `ETH_RX_SLOTS;
`else
	localparam RX_SLOTS = 4;
`endif
	localparam [3:0] RX_SLOTS_LOG2 = (RX_SLOTS <= 2) ? 1 : (RX_SLOTS <= 4) ? 2 :
	                                 (RX_SLOTS <= 8) ? 3 : 4;

	integer errors = 0;
	integer i, n;

	task check(input [8*48-1:0] what, input ok);
		begin
			if (!ok) begin
				$display("  FAIL: %0s", what);
				errors = errors + 1;
			end
		end
	endtask

	task wb_read(input [31:0] a, output [31:0] d);
		begin
			@(posedge wb_clk);
			wb_adr <= a; wb_we <= 0; wb_cyc <= 1; wb_stb <= 1;
			@(posedge wb_clk);
			while (!wb_ack) @(posedge wb_clk);
			d = wb_dat_o;
			wb_cyc <= 0; wb_stb <= 0;
			@(posedge wb_clk);
		end
	endtask

	task wb_write(input [31:0] a, input [31:0] d);
		begin
			@(posedge wb_clk);
			wb_adr <= a; wb_dat <= d; wb_we <= 1; wb_cyc <= 1; wb_stb <= 1;
			@(posedge wb_clk);
			while (!wb_ack) @(posedge wb_clk);
			wb_cyc <= 0; wb_stb <= 0; wb_we <= 0;
			@(posedge wb_clk);
		end
	endtask

	// -- the frame under test --
	reg [7:0] frame [0:2047];
	integer frame_len;

	function [7:0] pattern(input integer seed, input integer k);
		begin
			pattern = (k * 8'd37 + seed * 8'd11 + (k >> 3)) & 8'hFF;
		end
	endfunction

	function [31:0] crc32_byte(input [31:0] c, input [7:0] b);
		integer bit_i;
		reg [31:0] r;
		begin
			r = c;
			for (bit_i = 0; bit_i < 8; bit_i = bit_i + 1) begin
				if (r[0] ^ b[bit_i])
					r = (r >> 1) ^ 32'hEDB88320;
				else
					r = r >> 1;
			end
			crc32_byte = r;
		end
	endfunction

	task make_frame(input integer seed, input integer len);
		begin
			frame_len = len;
			for (i = 0; i < len; i = i + 1)
				frame[i] = pattern(seed, i);
		end
	endtask

	task load_and_send;
		reg [31:0] w;
		reg [31:0] st;
		begin
			for (i = 0; i < frame_len; i = i + 4) begin
				w = { frame[i+3], frame[i+2], frame[i+1], frame[i] };
				wb_write(TXBUF_BASE + i/4, w);
			end
			wb_write(REG_TXLEN, frame_len);
			wb_write(REG_TXCTRL, 1);
		end
	endtask

	task wait_tx_idle;
		reg [31:0] st;
		integer guard;
		begin
			guard = 0;
			// tx_busy crosses two synchronisers after the toggle does;
			// give it time to rise before trusting a zero.
			repeat (20) @(posedge wb_clk);
			wb_read(REG_STATUS, st);
			while (st[3] && guard < 100000) begin
				wb_read(REG_STATUS, st);
				guard = guard + 1;
			end
			check("tx_busy clears", !st[3]);
		end
	endtask

	// -- wire capture --
	//
	// Assembles dibits back into bytes on eth_refclk exactly as a PHY
	// would: first dibit is the byte's LSBs.
	reg [7:0] cap [0:2100];
	integer cap_n;
	integer cap_frames;
	integer idle_run;          // refclk cycles since tx_en last fell
	integer gap_before_last;   // idle_run at the start of the last frame
	reg [7:0] cap_shift;
	reg [1:0] cap_dibit;
	reg tx_en_d;

	initial begin
		cap_n = 0; cap_frames = 0; idle_run = 1000; gap_before_last = 0;
		cap_dibit = 0; tx_en_d = 0; cap_shift = 0;
	end

	always @(posedge refclk) begin
		tx_en_d <= eth_tx_en;
		if (eth_tx_en) begin
			if (!tx_en_d) begin
				cap_n = 0;
				cap_dibit = 0;
				gap_before_last = idle_run;
			end
			cap_shift = { eth_txd, cap_shift[7:2] };
			if (cap_dibit == 2'd3) begin
				cap[cap_n] = cap_shift;
				cap_n = cap_n + 1;
			end
			cap_dibit = cap_dibit + 1;
			idle_run = 0;
		end else begin
			if (tx_en_d) cap_frames = cap_frames + 1;
			idle_run = idle_run + 1;
		end
	end

	task check_wire;
		reg [31:0] crc;
		reg ok;
		integer bad_at;
		begin
			// 8 bytes of preamble + SFD, the frame, 4 of FCS
			check("captured length", cap_n == 8 + frame_len + 4);
			if (cap_n != 8 + frame_len + 4)
				$display("        got %0d bytes, want %0d", cap_n, 8 + frame_len + 4);

			ok = 1;
			for (i = 0; i < 7; i = i + 1)
				if (cap[i] != 8'h55) ok = 0;
			check("preamble 7 x 0x55", ok);
			check("SFD 0xD5", cap[7] == 8'hD5);

			ok = 1; bad_at = -1;
			for (i = 0; i < frame_len; i = i + 1)
				if (cap[8 + i] != frame[i] && ok) begin
					ok = 0; bad_at = i;
				end
			check("data bytes", ok);
			if (!ok)
				$display("        first mismatch at byte %0d: wire %02x, frame %02x",
					bad_at, cap[8 + bad_at], frame[bad_at]);

			crc = 32'hFFFFFFFF;
			for (i = 0; i < frame_len; i = i + 1)
				crc = crc32_byte(crc, frame[i]);
			crc = ~crc;
			check("FCS = CRC32 of data",
				{ cap[8+frame_len+3], cap[8+frame_len+2],
				  cap[8+frame_len+1], cap[8+frame_len] } == crc);
		end
	endtask

	task send_and_check(input integer seed, input integer len);
		integer before;
		begin
			make_frame(seed, len);
			before = cap_frames;
			load_and_send;
			wait_tx_idle;
			// tx_en has fallen by the time tx_busy clears (the IFG
			// runs with tx_en low), so the capture is complete.
			check("one frame on the wire", cap_frames == before + 1);
			check_wire;
		end
	endtask

	reg [31:0] rd;
	integer k, lens_i;
	integer lens [0:5];

	initial begin
		lens[0] = 60;   lens[1] = 61;   lens[2] = 62;
		lens[3] = 63;   lens[4] = 1514; lens[5] = 64;

		repeat (10) @(posedge wb_clk);
		wb_rst <= 0;
		repeat (10) @(posedge wb_clk);

		$display("\n=== ethmac_rmii TX ===");

		// 1-3: one frame at each tail alignment, plus a full MTU
		for (lens_i = 0; lens_i < 6; lens_i = lens_i + 1) begin
			n = errors;
			send_and_check(lens_i + 1, lens[lens_i]);
			if (errors == n)
				$display("  %0d-byte frame: preamble, SFD, data, FCS correct", lens[lens_i]);
		end

		// 4: two frames loaded back to back -- the second TX_CTRL is
		// written as soon as tx_busy clears, and must still see the
		// full inter-frame gap on the wire.
		n = errors;
		make_frame(9, 70);
		load_and_send;
		wait_tx_idle;
		make_frame(10, 66);
		load_and_send;
		wait_tx_idle;
		check_wire;
		check("inter-frame gap >= 48 refclk", gap_before_last >= 48);
		if (errors == n)
			$display("  back-to-back frames, gap %0d refclk cycles (>= 48)", gap_before_last);

		// 5: byte-lane writes into TX_BUF, checked on the wire (TX_BUF
		// is write-only from the CPU now -- reads return zero)
		n = errors;
		make_frame(33, 60);
		load_and_send;
		wait_tx_idle;
		@(posedge wb_clk);
		wb_sel <= 4'b0100;
		wb_write(TXBUF_BASE + 1, 32'h00AA0000);   // byte 6 only
		wb_sel <= 4'hF;
		frame[6] = 8'hAA;
		wb_write(REG_TXCTRL, 1);                   // resend, same TX_LEN
		wait_tx_idle;
		check_wire;
		wb_read(TXBUF_BASE + 1, rd);
		check("TX_BUF reads zero", rd == 0);
		// a register read between two RAM reads must not return RAM data
		wb_read(RXBUF_BASE, rd);
		wb_read(REG_RXLEN, rd);
		check("register read after RAM read", rd == 0);
		wb_read(REG_STATUS, rd);
		check("STATUS[15:12] = log2(RX_SLOTS)", rd[15:12] == RX_SLOTS_LOG2);
		if (errors == n)
			$display("  byte-lane TX_BUF write lands on the wire; STATUS reports %0d slots", RX_SLOTS);

		// 6: loopback -- TX into RX
		n = errors;
		loop <= 1;
		repeat (10) @(posedge refclk);
		make_frame(21, 97);
		load_and_send;
		wait_tx_idle;
		repeat (200) @(posedge wb_clk);
		wb_read(REG_STATUS, rd);
		check("loopback: rx_ready", rd[2]);
		check("loopback: no CRC errors", rd[11:8] == 0);
		wb_read(REG_RXLEN, rd);
		check("loopback: RX_LEN", rd == 97);
		for (k = 0; k < 97; k = k + 4) begin
			wb_read(RXBUF_BASE + k/4, rd);
			for (i = 0; i < 4; i = i + 1)
				if (k + i < 97 && rd[8*i +: 8] != frame[k + i]) begin
					$display("  FAIL: loopback byte %0d: %02x != %02x",
						k + i, rd[8*i +: 8], frame[k + i]);
					errors = errors + 1;
				end
		end
		wb_write(REG_RXCTRL, 1);
		repeat (20) @(posedge wb_clk);
		wb_read(REG_STATUS, rd);
		check("loopback: popped", !rd[2]);
		if (errors == n)
			$display("  loopback: TX FCS passes the RX residual check, frame intact");

		if (errors == 0)
			$display("\nRESULT: PASS\n");
		else
			$display("\nRESULT: FAIL -- %0d error(s)\n", errors);
		$finish;
	end

	initial begin
		#50_000_000;
		$display("\nRESULT: FAIL -- timeout\n");
		$finish;
	end

endmodule
