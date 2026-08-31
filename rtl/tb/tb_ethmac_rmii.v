`timescale 1ns/1ps
/*
 * ethmac_rmii_wb -- receive path.
 *
 * The question this exists to answer: how many back-to-back frames
 * can arrive before one is dropped? That number was 1 (a single
 * receive slot, "buffer still full -- drop this one"), which is what
 * made a burst of telnet traffic lose everything after the first
 * frame and wait on TCP retransmission timeouts to recover.
 *
 * Drives real RMII dibits into the real MAC at the real 2-bits-per-
 * refclk rate, with correct preamble, SFD and CRC32, then reads
 * frames back out over wishbone exactly as sw/apps/net/eth.c does:
 * poll STATUS bit 2, read RX_LEN, read the RX_BUF window, write
 * RXCTRL to pop.
 *
 * Checks, in order:
 *   1  a single frame arrives intact, right length, right bytes
 *   2  RX_SLOTS frames back-to-back all arrive, none dropped
 *   3  one more than that overflows -- and increments rx_drop_count
 *      rather than corrupting anything
 *   4  after draining, the FIFO accepts frames again (pointers wrap
 *      correctly rather than wedging a lap later)
 *   5  a bad-CRC frame is rejected as an ERROR, not a drop, and does
 *      not consume a slot
 */
module tb_ethmac_rmii;

	localparam RX_SLOTS = 4;      // must match the DUT default

	reg wb_clk = 0, wb_rst = 1;
	reg refclk = 0;
	always #10 wb_clk = ~wb_clk;   // 50MHz-ish
	always #20 refclk = ~refclk;   // 25MHz nominal RMII reference

	reg [31:0] wb_adr = 0, wb_dat = 0;
	reg wb_we = 0, wb_stb = 0, wb_cyc = 0;
	reg [3:0] wb_sel = 4'hF;
	wire [31:0] wb_dat_o;
	wire wb_ack;

	reg [1:0] eth_rxd = 0;
	reg eth_crs_dv = 0;
	wire [1:0] eth_txd;
	wire eth_tx_en, eth_rst_n, eth_int;
	wire eth_refclk_o;

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
	localparam RXBUF_BASE = 64;

	integer errors = 0;
	integer i, j;

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

	/* -- CRC32, same reflected algorithm the DUT uses -- */
	reg [31:0] crc;
	task crc_byte(input [7:0] b);
		integer k;
		begin
			crc = crc ^ {24'b0, b};
			for (k = 0; k < 8; k = k + 1)
				crc = crc[0] ? ((crc >> 1) ^ 32'hEDB88320) : (crc >> 1);
		end
	endtask

	/* -- drive one byte as 4 RMII dibits, LSB pair first -- */
	task send_byte(input [7:0] b);
		begin
			@(posedge refclk); eth_rxd <= b[1:0];
			@(posedge refclk); eth_rxd <= b[3:2];
			@(posedge refclk); eth_rxd <= b[5:4];
			@(posedge refclk); eth_rxd <= b[7:6];
		end
	endtask

	/*
	 * One frame of `len` payload bytes, payload[i] = seed + i, with a
	 * correct FCS unless corrupt is set. Carrier is raised for the
	 * frame and dropped after, which is what the DUT uses as
	 * end-of-frame.
	 */
	task send_frame(input integer len, input [7:0] seed, input corrupt);
		integer k;
		reg [7:0] b;
		begin
			crc = 32'hFFFFFFFF;
			@(posedge refclk);
			eth_crs_dv <= 1;
			for (k = 0; k < 7; k = k + 1) send_byte(8'h55);
			send_byte(8'hD5);
			for (k = 0; k < len; k = k + 1) begin
				b = seed + k[7:0];
				crc_byte(b);
				send_byte(b);
			end
			if (corrupt) crc = crc ^ 32'h00000001;
			b = ~crc[7:0];    send_byte(b);
			b = ~crc[15:8];   send_byte(b);
			b = ~crc[23:16];  send_byte(b);
			b = ~crc[31:24];  send_byte(b);
			@(posedge refclk);
			eth_crs_dv <= 0;
			eth_rxd <= 0;
			repeat (8) @(posedge refclk);   // inter-frame gap
		end
	endtask

	/* Pop one frame and verify it against what was sent. */
	task check_frame(input integer expect_len, input [7:0] seed,
	                 input [255:0] label);
		reg [31:0] d, w;
		integer k;
		reg [7:0] got, want;
		begin
			wb_read(REG_STATUS, d);
			if (!d[2]) begin
				$display("  FAIL %0s: no frame waiting", label);
				errors = errors + 1;
			end else begin
				wb_read(REG_RXLEN, d);
				if (d[10:0] !== expect_len[10:0]) begin
					$display("  FAIL %0s: len %0d, expected %0d",
						label, d[10:0], expect_len);
					errors = errors + 1;
				end
				for (k = 0; k < expect_len; k = k + 1) begin
					wb_read(RXBUF_BASE + (k >> 2), w);
					case (k[1:0])
						2'd0: got = w[7:0];
						2'd1: got = w[15:8];
						2'd2: got = w[23:16];
						2'd3: got = w[31:24];
					endcase
					want = seed + k[7:0];
					if (got !== want) begin
						$display("  FAIL %0s: byte %0d = %02h, expected %02h",
							label, k, got, want);
						errors = errors + 1;
						k = expect_len;   // one report per frame
					end
				end
				wb_write(REG_RXCTRL, 32'h1);   // pop
			end
		end
	endtask

	task read_status(output [31:0] d);
		begin wb_read(REG_STATUS, d); end
	endtask

	reg [31:0] st;
	integer drops_before, drops_after;

	initial begin
		repeat (4) @(posedge wb_clk);
		wb_rst = 0;
		repeat (200) @(posedge refclk);   // let the PHY reset stretch clear

		$display("");
		$display("=== ethmac_rmii RX, %0d slots ===", RX_SLOTS);

		/* -- 1: one frame -- */
		send_frame(64, 8'h10, 1'b0);
		repeat (20) @(posedge wb_clk);
		check_frame(64, 8'h10, "single frame");
		$display("  1  single frame received intact");

		/* -- 2: a full burst, nothing drained in between -- */
		read_status(st); drops_before = st[7:4];
		for (i = 0; i < RX_SLOTS; i = i + 1)
			send_frame(64 + i * 4, 8'h20 + i[7:0] * 8'h10, 1'b0);
		repeat (40) @(posedge wb_clk);
		read_status(st); drops_after = st[7:4];
		if (drops_after != drops_before) begin
			$display("  FAIL burst: %0d frames dropped, expected 0",
				drops_after - drops_before);
			errors = errors + 1;
		end
		for (i = 0; i < RX_SLOTS; i = i + 1)
			check_frame(64 + i * 4, 8'h20 + i[7:0] * 8'h10, "burst");
		$display("  2  %0d back-to-back frames, none dropped, all intact",
			RX_SLOTS);

		/* -- 3: one too many -- */
		read_status(st); drops_before = st[7:4];
		for (i = 0; i < RX_SLOTS + 1; i = i + 1)
			send_frame(80, 8'hA0 + i[7:0], 1'b0);
		repeat (40) @(posedge wb_clk);
		read_status(st); drops_after = st[7:4];
		if (drops_after != drops_before + 1) begin
			$display("  FAIL overflow: drop count moved by %0d, expected 1",
				drops_after - drops_before);
			errors = errors + 1;
		end else
			$display("  3  overflow drops exactly one and counts it");
		/* the slots that did land must still be the FIRST ones sent */
		for (i = 0; i < RX_SLOTS; i = i + 1)
			check_frame(80, 8'hA0 + i[7:0], "post-overflow");

		/* -- 4: the FIFO still works a lap later -- */
		send_frame(100, 8'hC0, 1'b0);
		repeat (20) @(posedge wb_clk);
		check_frame(100, 8'hC0, "after wrap");
		$display("  4  accepts frames again after wrapping");

		/* -- 5: bad CRC is an error, not a drop, and costs no slot -- */
		read_status(st); drops_before = st[7:4];
		send_frame(64, 8'hE0, 1'b1);      // corrupt
		repeat (20) @(posedge wb_clk);
		read_status(st);
		if (st[2]) begin
			$display("  FAIL bad CRC: frame was accepted");
			errors = errors + 1;
		end
		if (st[11:8] == 0) begin
			$display("  FAIL bad CRC: error counter did not move");
			errors = errors + 1;
		end
		if (st[7:4] != drops_before) begin
			$display("  FAIL bad CRC: counted as a drop, not an error");
			errors = errors + 1;
		end
		send_frame(64, 8'hF0, 1'b0);      // and the slot is still usable
		repeat (20) @(posedge wb_clk);
		check_frame(64, 8'hF0, "after bad CRC");
		$display("  5  bad CRC rejected as an error, slot not consumed");

		$display("");
		if (errors == 0)
			$display("RESULT: PASS -- RX FIFO holds %0d frames", RX_SLOTS);
		else
			$display("RESULT: FAIL -- %0d error(s)", errors);
		$display("");
		$finish;
	end

	initial begin
		#8000000;
		$display("RESULT: FAIL -- timeout");
		$finish;
	end

endmodule
