/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for the GPIO stream engines (rtl/gpio_stream.v) inside
 * rtl/gpio.v.
 *
 *   make tb_gpio_stream
 *
 * Two complete GPIO blocks, A and B, each with two ports and two
 * engines, on SEPARATE clocks: B runs PPM parts per million fast (or
 * slow, with a negative PPM), the way two boards with their own 48 MHz
 * oscillators do. The wires between them add a random 0-JIT ns of
 * delay to every edge. Nothing in the zlink or UART receivers knows
 * the other side's clock, so this is the test that they really recover
 * it rather than relying on both ends being the same simulation clock.
 *
 * Pins, chosen to be scattered on purpose -- any role on any pin is
 * the feature being tested:
 *
 *   A 0.0 -> A 0.1   jumper: raw and UART loopback on one board
 *   A 1.2 -> B 0.3   A's TX to B's RX (zlink, UART)
 *   B 0.4 -> A 1.6   B's TX to A's RX
 *   A 0.4..0.7       SPI: SCK, MOSI, MISO, CS to a device model
 *
 * What it covers:
 *   1. CONFIG reports the engines and modes; an unbuilt engine reads 0;
 *      SLOCK is a per-engine test-and-set.
 *   2. Ownership: a claimed pin ignores DIR/OUT, IN still reads it, and
 *      the pin is ordinary GPIO again when MODE goes back to 0.
 *   3. FIFO plumbing: STX/STX4/SRX/SRX4, levels, TX overflow, flush,
 *      and the K-in-word flag.
 *   4. Raw: 48 Mbit/s through the jumper, both bit orders, trigger.
 *   5. Raw repeat: the TX FIFO loops.
 *   6. UART A -> B and B -> A at 115200, 31250 (MIDI) and 3 Mbaud, and
 *      a framing error counted.
 *   7. SPI against a device model, modes 0-3, MSB and LSB first, full
 *      duplex and write-only.
 *   8. zlink A <-> B at 12 Mbit/s and at 3 Mbit/s, full duplex, with
 *      data and K symbols, across the clock offset and the jitter.
 *   9. zlink recovery: a fault on the wire mid-burst damages that burst,
 *      and the link re-aligns on the next comma and is clean again.
 *  10. Engine 1 is independent of engine 0.
 */

`timescale 1ns / 1ps

`ifndef PPM
`define PPM 300
`endif
`ifndef JIT
`define JIT 3
`endif

module tb_gpio_stream;

	localparam NPORTS = 2;
	localparam ENG = 2;

	// -- clocks: A at 48 MHz, B off by PPM --
	reg clka = 0, clkb = 0;
	real ta = 1000.0 / 48.0 / 2.0;
	real tb = (1000.0 / 48.0 / 2.0) * (1.0 - `PPM * 1.0e-6);
	always #(ta) clka = ~clka;
	always #(tb) clkb = ~clkb;

	reg rst = 1;

	integer errors = 0;
	integer i, j, n;

	// ------------------------------------------------------------------
	// two GPIO blocks
	// ------------------------------------------------------------------

	reg [31:0] adra = 0, data = 0, adrb = 0, datb = 0;
	reg wea = 0, stba = 0, web = 0, stbb = 0;
	wire [31:0] qa, qb;
	wire acka, ackb;
	wire [63:0] dira, outa, ina, dirb, outb, inb;
	wire ledA, ledB;
	wire [7:0] ledsA, ledsB;

	gpio_wb #(.NPORTS(NPORTS), .ENGINES(ENG)) A (
		.wb_clk_i(clka), .wb_rst_i(rst), .wb_adr_i(adra), .wb_dat_i(data),
		.wb_dat_o(qa), .wb_we_i(wea), .wb_sel_i(4'hF), .wb_stb_i(stba),
		.wb_ack_o(acka), .wb_cyc_i(stba), .led(ledA), .leds(ledsA),
		.gpio_dir_o(dira), .gpio_out_o(outa), .gpio_in_i(ina));

	gpio_wb #(.NPORTS(NPORTS), .ENGINES(ENG)) B (
		.wb_clk_i(clkb), .wb_rst_i(rst), .wb_adr_i(adrb), .wb_dat_i(datb),
		.wb_dat_o(qb), .wb_we_i(web), .wb_sel_i(4'hF), .wb_stb_i(stbb),
		.wb_ack_o(ackb), .wb_cyc_i(stbb), .led(ledB), .leds(ledsB),
		.gpio_dir_o(dirb), .gpio_out_o(outb), .gpio_in_i(inb));

	// ------------------------------------------------------------------
	// pads: driver, far-end driver, pull-up (as rtl/tb/tb_gpio.v)
	// ------------------------------------------------------------------

	wire [15:0] pa, pb;
	reg [15:0] xa_dir = 0, xa_out = 0, xb_dir = 0, xb_out = 0;

	genvar g;
	generate
		for (g = 0; g < 16; g = g + 1) begin : pads
			assign pa[g] = dira[g] ? outa[g] : 1'bz;
			assign pa[g] = xa_dir[g] ? xa_out[g] : 1'bz;
			pullup(pa[g]);
			assign pb[g] = dirb[g] ? outb[g] : 1'bz;
			assign pb[g] = xb_dir[g] ? xb_out[g] : 1'bz;
			pullup(pb[g]);
		end
	endgenerate
	assign ina = { 48'd0, pa };
	assign inb = { 48'd0, pb };

	// wires with jitter: every edge arrives 0..JIT ns late
	reg [31:0] seed = 32'd12345;
	integer dly;

	// A 0.0 -> A 0.1 (a short jumper: no jitter)
	always @(*) begin xa_dir[1] = jumper; xa_out[1] = pa[0]; end
	reg jumper = 1;

	// A 1.2 -> B 0.3
	reg ab = 1;
	always @(pa[10]) begin
		dly = $unsigned($random(seed)) % (`JIT + 1);
		ab <= #(dly) pa[10];
	end
	reg glitch = 0;
	always @(*) begin xb_dir[3] = 1'b1; xb_out[3] = ab ^ glitch; end

	// B 0.4 -> A 1.6
	reg ba = 1;
	always @(pb[4]) begin
		dly = $unsigned($random(seed)) % (`JIT + 1);
		ba <= #(dly) pb[4];
	end
	always @(*) begin xa_dir[14] = 1'b1; xa_out[14] = ba; end

	// ------------------------------------------------------------------
	// SPI device on A 0.4 (SCK), 0.5 (MOSI), 0.6 (MISO), 0.7 (CS)
	//
	// Mode set by the test. It shifts out resp[k] while it receives
	// got[k], with a 6 ns clock-to-out, which is ordinary for a part
	// at 3.3V.
	// ------------------------------------------------------------------

	reg dev_cpol = 0, dev_cpha = 0, dev_lsb = 0;
	reg [7:0] resp [0:63];
	reg [7:0] got [0:63];
	integer dk = 0;
	integer dbits = 0;
	reg [7:0] dsr_in = 0, dsr_out = 0;
	reg miso = 1;

	wire sck = pa[4], mosi = pa[5], cs = pa[7];
	always @(*) begin xa_dir[6] = !cs; xa_out[6] = miso; end

	// the bit the device puts on MISO next
	function nextbit;
		input [7:0] v;
		input lsbf;
		nextbit = lsbf ? v[0] : v[7];
	endfunction

	always @(negedge cs) begin
		dk = 0; dbits = 0;
		dsr_out = resp[0];
		// CPHA 0: the first bit is out before the first edge
		if (!dev_cpha) miso <= #6 nextbit(dsr_out, dev_lsb);
	end

	// leading edge: idle->active; trailing: active->idle
	wire lead = dev_cpol ? !sck : sck;
	always @(lead) begin
		if (!cs) begin
			if (lead == 1'b1) begin
				// leading edge
				if (!dev_cpha) dev_sample;
				else dev_shift;
			end else begin
				if (!dev_cpha) dev_shift;
				else dev_sample;
			end
		end
	end

	task dev_sample;
	begin
		dsr_in = dev_lsb ? { mosi, dsr_in[7:1] } : { dsr_in[6:0], mosi };
		dbits = dbits + 1;
		if (dbits == 8) begin
			got[dk] = dsr_in;
			dk = dk + 1;
			dbits = 0;
			dsr_out = resp[dk];
		end
	end
	endtask

	// CPHA 0: shift after the sample (on the trailing edge), but not
	// past the last bit of a byte, where the next byte's first bit is
	// already loaded by dev_sample. CPHA 1: present a bit on each
	// leading edge.
	reg [7:0] cur;
	task dev_shift;
	begin
		if (!dev_cpha) begin
			if (dbits != 0) begin
				dsr_out = dev_lsb ? { 1'b0, dsr_out[7:1] } : { dsr_out[6:0], 1'b0 };
				miso <= #6 nextbit(dsr_out, dev_lsb);
			end else
				miso <= #6 nextbit(dsr_out, dev_lsb);
		end else begin
			miso <= #6 nextbit(dsr_out, dev_lsb);
			dsr_out = dev_lsb ? { 1'b0, dsr_out[7:1] } : { dsr_out[6:0], 1'b0 };
		end
	end
	endtask

	// ------------------------------------------------------------------
	// bus tasks
	// ------------------------------------------------------------------

	reg [31:0] r;

	task wa(input [31:0] a, input [31:0] d);
	begin
		@(posedge clka); adra <= a >> 2; data <= d; wea <= 1; stba <= 1;
		@(posedge clka); while (!acka) @(posedge clka);
		stba <= 0; wea <= 0;
	end
	endtask

	task ra(input [31:0] a);
	begin
		@(posedge clka); adra <= a >> 2; wea <= 0; stba <= 1;
		@(posedge clka); while (!acka) @(posedge clka);
		r = qa; stba <= 0;
	end
	endtask

	task wb(input [31:0] a, input [31:0] d);
	begin
		@(posedge clkb); adrb <= a >> 2; datb <= d; web <= 1; stbb <= 1;
		@(posedge clkb); while (!ackb) @(posedge clkb);
		stbb <= 0; web <= 0;
	end
	endtask

	task rb(input [31:0] a);
	begin
		@(posedge clkb); adrb <= a >> 2; web <= 0; stbb <= 1;
		@(posedge clkb); while (!ackb) @(posedge clkb);
		r = qb; stbb <= 0;
	end
	endtask

	task check(input [255:0] what, input [31:0] got_, input [31:0] want);
	begin
		if (got_ !== want) begin
			$display("FAIL %0s: got %08x want %08x", what, got_, want);
			errors = errors + 1;
		end
	end
	endtask

	// ------------------------------------------------------------------
	// register map (docs/gpio.md)
	// ------------------------------------------------------------------

	localparam CONFIG = 32'h0c;
	localparam PORT0 = 32'h1000, PORT1 = 32'h1020;
	localparam DIR = 0, OUT = 4, IN = 8;
	function [31:0] E(input integer e, input integer reg_);
		E = 32'h2000 + e * 32'h40 + reg_ * 4;
	endfunction
	localparam SCTL = 0, SPINS = 1, SRATE = 2, SSTAT = 3, STX = 4,
	           STX4 = 5, SRX = 6, SRX4 = 7, SERR = 8, SFLUSH = 9;

	localparam M_ZLINK = 1, M_SPI = 2, M_UART = 3, M_RAW = 4;
	localparam B_LSB = 1 << 3, B_CPHA = 1 << 4, B_CPOL = 1 << 5,
	           B_TXOD = 1 << 8, B_RXEN = 1 << 9, B_LOOP = 1 << 10,
	           B_RPT = 1 << 11, B_TRIG = 1 << 12, B_CS = 1 << 14,
	           B_IDLE = 1 << 15;

	// role byte: enable, port, pin
	function [7:0] role(input integer port, input integer pin);
		role = 8'h80 | (port << 3) | pin;
	endfunction

	// ------------------------------------------------------------------
	// helpers
	// ------------------------------------------------------------------

	task wait_a(input integer clocks);
		begin repeat (clocks) @(posedge clka); end
	endtask

	// drain A's RX FIFO into buf_a; stop at `want` entries or timeout
	reg [8:0] buf_a [0:4095];
	reg [8:0] buf_b [0:4095];
	integer na, nb;

	task drain_a(input integer e, input integer want, input integer timeout);
		integer t;
	begin
		t = 0;
		while (na < want && t < timeout) begin
			ra(E(e, SRX));
			if (r[9]) begin buf_a[na] = r[8:0]; na = na + 1; end
			else t = t + 1;
		end
	end
	endtask

	task drain_b(input integer e, input integer want, input integer timeout);
		integer t;
	begin
		t = 0;
		while (nb < want && t < timeout) begin
			rb(E(e, SRX));
			if (r[9]) begin buf_b[nb] = r[8:0]; nb = nb + 1; end
			else t = t + 1;
		end
	end
	endtask

	reg [8:0] sent [0:4095];
	reg [8:0] sentb [0:4095];

	// ------------------------------------------------------------------
	// the tests
	// ------------------------------------------------------------------

	initial begin
		for (i = 0; i < 64; i = i + 1) resp[i] = 8'hA0 + i;
		repeat (8) @(posedge clka);
		rst = 0;
		repeat (4) @(posedge clka);

		// 1. CONFIG
		ra(CONFIG);
		check("CONFIG", r, 32'h4750_0000 | (4'hF << 8) | (ENG << 4) | NPORTS);
		ra(E(3, SCTL));
		check("unbuilt engine reads 0", r, 0);
		wa(E(3, SCTL), 32'hFFFF_FFFF);	// and swallows writes without hanging

		// SLOCK: the first read takes it, the second sees it taken,
		// writing 0 gives it back
		ra(E(0, 10)); check("SLOCK first read", r, 0);
		ra(E(0, 10)); check("SLOCK second read", r, 1);
		ra(E(1, 10)); check("SLOCK is per engine", r, 0);
		wa(E(0, 10), 0);
		ra(E(0, 10)); check("SLOCK released", r, 0);
		wa(E(0, 10), 0); wa(E(1, 10), 0);

		// 2. ownership
		wa(PORT1 + DIR, 8'hFF);
		wa(PORT1 + OUT, 8'h00);
		wait_a(4);
		if (pa[13] !== 1'b0) begin $display("FAIL port drives 1.5"); errors = errors + 1; end
		wa(E(0, SPINS), role(1, 5));			// TX on 1.5
		wa(E(0, SCTL), M_RAW | B_IDLE);		// raw, idle high
		wait_a(8);
		if (pa[13] !== 1'b1) begin $display("FAIL engine does not own 1.5"); errors = errors + 1; end
		ra(PORT1 + IN);
		if (!r[5]) begin $display("FAIL IN does not read an owned pin"); errors = errors + 1; end
		ra(PORT1 + DIR);
		check("DIR still reads the register", r, 8'hFF);
		wa(E(0, SCTL), 0);
		wait_a(4);
		if (pa[13] !== 1'b0) begin $display("FAIL 1.5 not returned to GPIO"); errors = errors + 1; end
		wa(PORT1 + DIR, 0);

		// 3. FIFO plumbing, with the engine off so nothing drains it
		wa(E(0, SFLUSH), 7);
		wa(E(0, STX), 9'h1BC);			// K28.5
		wa(E(0, STX4), 32'h44332211);
		ra(E(0, SSTAT));
		check("TX level 5", r[10:0], 5);
		for (i = 0; i < 1019; i = i + 1) wa(E(0, STX), i);
		ra(E(0, SSTAT));
		check("TX level full", r[10:0], 1024);
		check("no overflow yet", r[13], 0);
		wa(E(0, STX), 0);
		ra(E(0, SSTAT));
		check("overflow flagged", r[13], 1);
		wa(E(0, SFLUSH), 7);
		ra(E(0, SSTAT));
		check("flushed", { r[13], r[10:0] }, 0);

		// internal loopback through raw mode puts bytes in RX
		wa(E(0, SPINS), 0);
		wa(E(0, SRATE), 0);
		wa(E(0, SCTL), M_RAW | B_RXEN | B_LOOP | B_LSB);
		wa(E(0, STX4), 32'hDDCCBBAA);
		// raw RX starts at once, so the bytes land on some bit offset;
		// what matters here is SRX4's mechanics, so just count entries
		wait_a(200);
		wa(E(0, SCTL), M_RAW | B_LOOP | B_LSB);	// stop capturing
		ra(E(0, SSTAT));
		if (r[26:16] < 4) begin $display("FAIL loopback RX level %0d", r[26:16]); errors = errors + 1; end
		n = r[26:16];
		ra(E(0, SRX4));
		ra(E(0, SSTAT));
		check("SRX4 popped 4", r[26:16], n - 4);
		wa(E(0, SCTL), 0);
		wa(E(0, SFLUSH), 7);

		// 4. raw at 48 Mbit/s through the jumper, triggered on the first
		// edge so the byte boundaries line up: TX idles high, the first
		// byte starts with a 0 bit
		for (j = 0; j < 2; j = j + 1) begin
			wa(E(0, SFLUSH), 7);
			wa(E(0, SPINS), role(0, 0) | (role(0, 1) << 8));
			wa(E(0, SRATE), 0);
			wa(E(0, SCTL), M_RAW | B_IDLE | B_RXEN | B_TRIG | (j ? B_LSB : 0));
			// first byte's first bit must be 0 for the trigger
			wa(E(0, STX4), j ? 32'h0F_5A_3C_E4 : 32'h0F_5A_3C_27);
			wa(E(0, STX4), 32'h88_77_66_55);
			wait_a(150);
			na = 0;
			drain_a(0, 8, 4);
			check(j ? "raw LSB count" : "raw MSB count", na, 8);
			check("raw b0", buf_a[0], j ? 9'h0E4 : 9'h027);
			check("raw b1", buf_a[1], 9'h03C);
			check("raw b3", buf_a[3], 9'h00F);
			check("raw b7", buf_a[7], 9'h088);
			wa(E(0, SCTL), 0);
		end

		// 5. raw repeat: two bytes, looped, captured. The pattern goes
		// in with the engine off; turning it on with REPEAT marks where
		// the loop starts (docs/gpio.md, "Raw").
		wa(E(0, SCTL), 0);
		wa(E(0, SFLUSH), 7);
		wa(E(0, SRATE), 1);
		wa(E(0, STX), 8'h0F);
		wa(E(0, STX), 8'h96);
		wa(E(0, SCTL), M_RAW | B_IDLE | B_RXEN | B_TRIG | B_RPT);
		wait_a(400);
		na = 0;
		drain_a(0, 10, 4);
		check("repeat count", na, 10);
		for (i = 0; i < 10; i = i + 1)
			check("repeat byte", buf_a[i], (i % 2) ? 9'h096 : 9'h00F);
		wa(E(0, SCTL), 0);
		wa(E(0, SFLUSH), 7);

		// 6. UART both ways, three speeds
		uart_test(103, 64);		// 48e6 / (4 * 104) = 115384 baud
		uart_test(383, 16);		// 31250 baud, MIDI, exact
		uart_test(3, 256);		// 3 Mbaud
		uart_framing();

		// 7. SPI
		for (j = 0; j < 8; j = j + 1)
			spi_test(j[1:0], j[2], 1);	// all modes, MSB and LSB, 12 MHz
		spi_test(2'd0, 1'b0, 3);		// 6 MHz
		spi_writeonly();

		// 8. zlink, full duplex, two speeds
		zlink_test(0, 1000);			// 12 Mbit/s
		zlink_test(3, 300);				// 3 Mbit/s

		// 9. a glitch, then recovery
		zlink_glitch();

		// 10. engine 1 alongside engine 0
		engine1_test();

		if (errors == 0) $display("tb_gpio_stream: PASS (PPM=%0d, JIT=%0d ns)", `PPM, `JIT);
		else $display("tb_gpio_stream: FAIL (%0d errors)", errors);
		$finish;
	end

	// ------------------------------------------------------------------

	task uart_cfg(input integer d);
	begin
		wa(E(0, SCTL), 0); wb(E(0, SCTL), 0);
		wa(E(0, SFLUSH), 7); wb(E(0, SFLUSH), 7);
		wa(E(0, SRATE), d); wb(E(0, SRATE), d);
		wa(E(0, SPINS), role(1, 2) | (role(1, 6) << 8));	// TX 1.2, RX 1.6
		wb(E(0, SPINS), role(0, 4) | (role(0, 3) << 8));	// TX 0.4, RX 0.3
		wa(E(0, SCTL), M_UART | B_RXEN);
		wb(E(0, SCTL), M_UART | B_RXEN);
		wait_a(4 * (d + 1) * 12);
	end
	endtask

	task uart_test(input integer d, input integer count);
		integer k, w;
	begin
		uart_cfg(d);
		for (k = 0; k < count; k = k + 1) begin
			sent[k] = ($random(seed) & 8'hFF);
			sentb[k] = ($random(seed) & 8'hFF);
			wa(E(0, STX), sent[k]);
			wb(E(0, STX), sentb[k]);
		end
		// wait out the frames: 10 bits each at 4 * (d + 1) clocks
		w = count * 10 * 4 * (d + 1) + 2000;
		na = 0; nb = 0;
		wait_a(w);
		drain_b(0, count, 4);
		drain_a(0, count, 4);
		check("uart A->B count", nb, count);
		check("uart B->A count", na, count);
		for (k = 0; k < count; k = k + 1) begin
			if (buf_b[k] !== sent[k]) begin
				$display("FAIL uart A->B div %0d byte %0d: %02x want %02x", d, k, buf_b[k], sent[k]);
				errors = errors + 1;
			end
			if (buf_a[k] !== sentb[k]) begin
				$display("FAIL uart B->A div %0d byte %0d: %02x want %02x", d, k, buf_a[k], sentb[k]);
				errors = errors + 1;
			end
		end
		rb(E(0, SERR));
		check("uart errors", r, 0);
	end
	endtask

	// a byte sent with B's TX inverted reaches A as a start bit and
	// garbage with a low stop bit: one framing error, no byte
	task uart_framing;
	begin
		uart_cfg(3);
		wa(E(0, SFLUSH), 7);
		wb(E(0, SCTL), M_UART | B_RXEN | (1 << 6));		// TXINV
		wait_a(200);
		wb(E(0, STX), 8'h00);
		wait_a(400);
		ra(E(0, SERR));
		if (r == 0) begin $display("FAIL no framing error counted"); errors = errors + 1; end
		wb(E(0, SCTL), 0);
		wa(E(0, SCTL), 0);
	end
	endtask

	task spi_test(input [1:0] md, input lsbf, input integer d);
		integer k, w;
	begin
		dev_cpha = md[0]; dev_cpol = md[1]; dev_lsb = lsbf;
		wa(E(0, SCTL), 0);
		wa(E(0, SFLUSH), 7);
		wa(E(0, SRATE), d);
		// SCK 0.4, MOSI 0.5, MISO 0.6, CS 0.7
		wa(E(0, SPINS), role(0, 5) | (role(0, 6) << 8) |
		                (role(0, 4) << 16) | (role(0, 7) << 24));
		wa(E(0, SCTL), M_SPI | B_RXEN | B_CS | (md[0] ? B_CPHA : 0) |
		               (md[1] ? B_CPOL : 0) | (lsbf ? B_LSB : 0));
		wait_a(8);
		wa(E(0, SCTL), M_SPI | B_RXEN | (md[0] ? B_CPHA : 0) |
		               (md[1] ? B_CPOL : 0) | (lsbf ? B_LSB : 0));	// CS low
		wait_a(8);
		for (k = 0; k < 16; k = k + 1) sent[k] = (k * 37 + 11) & 8'hFF;
		wa(E(0, STX4), { sent[3][7:0], sent[2][7:0], sent[1][7:0], sent[0][7:0] });
		wa(E(0, STX4), { sent[7][7:0], sent[6][7:0], sent[5][7:0], sent[4][7:0] });
		wa(E(0, STX4), { sent[11][7:0], sent[10][7:0], sent[9][7:0], sent[8][7:0] });
		wa(E(0, STX4), { sent[15][7:0], sent[14][7:0], sent[13][7:0], sent[12][7:0] });
		// wait for not-busy
		w = 0;
		ra(E(0, SSTAT));
		while (r[11] && w < 2000) begin ra(E(0, SSTAT)); w = w + 1; end
		wa(E(0, SCTL), M_SPI | B_RXEN | B_CS | (md[0] ? B_CPHA : 0) |
		               (md[1] ? B_CPOL : 0) | (lsbf ? B_LSB : 0));	// CS high
		na = 0;
		drain_a(0, 16, 4);
		check("spi rx count", na, 16);
		check("spi device count", dk, 16);
		for (k = 0; k < 16; k = k + 1) begin
			if (got[k] !== sent[k][7:0]) begin
				$display("FAIL spi mode %0d lsb %0d MOSI byte %0d: %02x want %02x", md, lsbf, k, got[k], sent[k][7:0]);
				errors = errors + 1;
			end
			if (buf_a[k] !== { 1'b0, resp[k] }) begin
				$display("FAIL spi mode %0d lsb %0d MISO byte %0d: %02x want %02x", md, lsbf, k, buf_a[k], resp[k]);
				errors = errors + 1;
			end
		end
	end
	endtask

	task spi_writeonly;
		integer w;
	begin
		dev_cpha = 0; dev_cpol = 0; dev_lsb = 0;
		wa(E(0, SCTL), 0);
		wa(E(0, SFLUSH), 7);
		wa(E(0, SRATE), 1);
		wa(E(0, SCTL), M_SPI);					// no RXEN, CS low
		wa(E(0, STX4), 32'h04030201);
		w = 0;
		ra(E(0, SSTAT));
		while (r[11] && w < 2000) begin ra(E(0, SSTAT)); w = w + 1; end
		check("write-only RX level", r[26:16], 0);
		check("write-only reached device", got[3], 8'h04);
		wa(E(0, SCTL), M_SPI | B_CS);
		wa(E(0, SCTL), 0);
	end
	endtask

	task zlink_cfg(input integer d);
	begin
		wa(E(0, SCTL), 0); wb(E(0, SCTL), 0);
		wa(E(0, SFLUSH), 7); wb(E(0, SFLUSH), 7);
		wa(E(0, SRATE), d); wb(E(0, SRATE), d);
		wa(E(0, SPINS), role(1, 2) | (role(1, 6) << 8));
		wb(E(0, SPINS), role(0, 4) | (role(0, 3) << 8));
		wa(E(0, SCTL), M_ZLINK | B_RXEN);
		wb(E(0, SCTL), M_ZLINK | B_RXEN);
		// a few idle commas to align on
		wait_a(40 * (d + 1) * 6);
	end
	endtask

	task zlink_test(input integer d, input integer count);
		integer k, w, chunk;
	begin
		zlink_cfg(d);
		ra(E(0, SSTAT));
		check("A aligned", r[14], 1);
		rb(E(0, SSTAT));
		check("B aligned", r[14], 1);
		// data and the three framing K symbols zlink will use
		for (k = 0; k < count; k = k + 1) begin
			sent[k] = (k % 97 == 5) ? 9'h1FB :		// K27.7
			          (k % 97 == 50) ? 9'h1FD :		// K29.7
			          (k % 97 == 90) ? 9'h1FE :		// K30.7
			          ($random(seed) & 8'hFF);
			sentb[k] = ($random(seed) & 8'hFF);
		end
		na = 0; nb = 0;
		// push in chunks the size of the FIFO, draining as we go
		for (k = 0; k < count; k = k + 1) begin
			wa(E(0, STX), sent[k]);
			wb(E(0, STX), sentb[k]);
		end
		w = count * 40 * (d + 1) + 4000;
		wait_a(w);
		drain_b(0, count, 4);
		drain_a(0, count, 4);
		check("zlink A->B count", nb, count);
		check("zlink B->A count", na, count);
		for (k = 0; k < count; k = k + 1) begin
			if (buf_b[k] !== sent[k]) begin
				if (errors < 20) $display("FAIL zlink A->B div %0d sym %0d: %03x want %03x", d, k, buf_b[k], sent[k]);
				errors = errors + 1;
			end
			if (buf_a[k] !== sentb[k]) begin
				if (errors < 20) $display("FAIL zlink B->A div %0d sym %0d: %03x want %03x", d, k, buf_a[k], sentb[k]);
				errors = errors + 1;
			end
		end
		ra(E(0, SERR));
		check("zlink A errors", r, 0);
		rb(E(0, SERR));
		check("zlink B errors", r, 0);
	end
	endtask

	// A fault mid-burst, then a clean link. 8b/10b does not promise to
	// flag every corruption -- some turn one valid symbol into another,
	// which is what the link layer's CRC is for (docs/zlink.md) -- so
	// the test only insists that the fault visibly hurt the burst
	// (errors counted, or bytes wrong or missing) and that the link is
	// clean afterwards, having re-aligned on the next comma.
	task zlink_glitch;
		integer k, bad;
	begin
		zlink_cfg(3);								// 3 Mbit/s: a long burst
		for (k = 0; k < 600; k = k + 1) sent[k] = k & 8'hFF;
		fork
			for (k = 0; k < 600; k = k + 1) wa(E(0, STX), sent[k]);
			begin
				wait_a(3000);
				glitch = 1; #1000; glitch = 0;	// three bit-times
			end
		join
		wait_a(600 * 160 + 4000);
		nb = 0;
		drain_b(0, 700, 4);
		rb(E(0, SERR));
		bad = r;
		for (k = 0; k < 600; k = k + 1)
			if (k >= nb || buf_b[k] !== sent[k]) bad = bad + 1;
		if (nb != 600) bad = bad + 1;
		if (bad == 0) begin $display("FAIL the fault did not reach the receiver"); errors = errors + 1; end
		// after the burst the link must be clean again
		wb(E(0, SFLUSH), 7);
		nb = 0;
		for (k = 0; k < 50; k = k + 1) wa(E(0, STX), 8'hC0 + k);
		wait_a(50 * 160 + 2000);
		drain_b(0, 50, 4);
		check("after fault count", nb, 50);
		for (k = 0; k < nb; k = k + 1) check("after fault", buf_b[k], 8'hC0 + k);
		rb(E(0, SERR));
		check("after fault errors", r, 0);
		rb(E(0, SSTAT));
		check("after fault aligned", r[14], 1);
		wa(E(0, SCTL), 0); wb(E(0, SCTL), 0);
	end
	endtask

	// engine 1 runs raw loopback while engine 0 holds other settings
	task engine1_test;
	begin
		wa(E(0, SCTL), 0);
		wa(E(0, SRATE), 16'h1234);
		wa(E(1, SFLUSH), 7);
		wa(E(1, SRATE), 0);
		wa(E(1, SPINS), role(0, 0) | (role(0, 1) << 8));
		wa(E(1, SCTL), M_RAW | B_IDLE | B_RXEN | B_TRIG);
		wa(E(1, STX), 8'h5A);
		wait_a(80);
		ra(E(1, SRX));
		check("engine 1 raw", r, { 22'd0, 1'b1, 9'h05A });
		ra(E(0, SRATE));
		check("engine 0 untouched", r, 32'h1234);
		ra(E(1, SCTL));
		check("engine 1 SCTL", r, M_RAW | B_IDLE | B_RXEN | B_TRIG);
		wa(E(1, SCTL), 0);
	end
	endtask

	initial begin
		#200_000_000;
		$display("tb_gpio_stream: TIMEOUT");
		$finish;
	end

endmodule
