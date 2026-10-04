/*
 * Testbench for rtl/usb_cdc_uart.v -- the console's transmit side with
 * and without a USB host. docs/usb_cdc.md, "Power only".
 *
 *   iverilog -g2005 -I rtl -o /tmp/t rtl/tb/tb_usb_cdc_uart.v \
 *       rtl/usb_cdc_uart.v rtl/ext/usb_cdc/[a-z]*.v && vvp /tmp/t
 *
 * The real USB core is instantiated, with D+ pulled up and D- pulled
 * down -- the idle J state of a cable with nothing on the far end. With
 * no host on the bus it genuinely stays unconfigured and its frame
 * number never moves: the charger case, not a model of it. For the
 * cases WITH a host the evidence a host would produce is forced onto
 * the core's outputs -- the frame number moving, configured, and
 * in_ready for a host draining the port -- since generating USB
 * packets here would test the core rather than this block.
 *
 * The two timeouts are parameters, cut from seconds to cycles so the
 * whole thing simulates in moments.
 *
 *   1. no host: the first byte waits for the host window and no longer,
 *      then every write is dropped at once, THRE stays empty, the
 *      transmit interrupt is pending, and an FCR transmit reset does not
 *      re-arm a wait.
 *   2. host present: exactly as before -- bytes held, the ten-second
 *      give-up (here STALL) still the only way out, and recovery when
 *      the host starts taking bytes, every byte delivered in order.
 *   3. host late: no_host first, then the host's first frame ends it
 *      and bytes are held and delivered again.
 *   4. configured counts as evidence, as frames do.
 */

`timescale 1ns / 1ps

module tb_usb_cdc_uart;

	localparam HOST_WAIT = 2000;
	localparam STALL     = 6000;

	localparam [25:0] R_THR = 26'd0;
	localparam [25:0] R_IER = 26'd1;
	localparam [25:0] R_IIR = 26'd2;		// FCR on write
	localparam [25:0] R_LSR = 26'd5;

	reg clk = 1'b0;
	reg rst = 1'b1;
	reg [25:0] adr;
	reg [31:0] dat_w;
	wire [31:0] dat_r;
	reg we, stb, cyc;
	wire ack;
	wire irq;

	tri1 usb_dp;		// pulled up: the device's own 1.5k, and nothing else
	tri0 usb_dn;		// pulled down
	wire usb_pu;
	wire configured;

	always #10.4167 clk = ~clk;		// 48MHz

	usb_cdc_uart #(
		.STALL_CYCLES(STALL),
		.HOST_WAIT_CYCLES(HOST_WAIT)
	) dut (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(adr),
		.wb_dat_i(dat_w),
		.wb_dat_o(dat_r),
		.wb_we_i(we),
		.wb_sel_i(4'b0001),
		.wb_stb_i(stb),
		.wb_cyc_i(cyc),
		.wb_ack_o(ack),
		.int_o(irq),
		.usb_dp(usb_dp),
		.usb_dn(usb_dn),
		.usb_pu(usb_pu),
		.configured_o(configured)
	);

	integer errors;
	integer checks;
	reg [31:0] rd;
	integer i;
	integer t_start;
	integer cycle;
	integer waited;
	integer nfill;
	integer t_held;

	// What a "host" took, in order.
	reg [7:0] taken [0:255];
	integer ntaken;
	reg host_draining;

	always @(posedge clk) cycle <= cycle + 1;

	always @(posedge clk)
		if (host_draining && dut.usb_in_valid && dut.usb_in_ready) begin
			taken[ntaken] <= dut.usb_in_data;
			ntaken <= ntaken + 1;
		end

	task check;
		input [8*48-1:0] what;
		input ok;
		begin
			checks = checks + 1;
			if (ok) $display("  ok    %0s", what);
			else begin errors = errors + 1; $display("  FAIL  %0s", what); end
		end
	endtask

	task wb_write;
		input [25:0] a;
		input [31:0] d;
		begin
			@(posedge clk);
			adr <= a; dat_w <= d; we <= 1'b1; stb <= 1'b1; cyc <= 1'b1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			stb <= 1'b0; cyc <= 1'b0; we <= 1'b0;
		end
	endtask

	task wb_read;
		input [25:0] a;
		begin
			@(posedge clk);
			adr <= a; we <= 1'b0; stb <= 1'b1; cyc <= 1'b1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			rd = dat_r;
			stb <= 1'b0; cyc <= 1'b0;
		end
	endtask

	task reset_dut;
		begin
			release dut.usb_frame;
			release dut.usb_cdc_configured;
			release dut.usb_in_ready;
			host_draining = 1'b0;
			ntaken = 0;
			rst = 1'b1;
			repeat (5) @(posedge clk);
			rst = 1'b0;
			@(posedge clk);
		end
	endtask

	// Write a byte the way bios.c's putchar() does: wait for THRE, then
	// write. Returns, in `waited`, how many cycles the wait took.
	task putchar;
		input [7:0] c;
		begin
			t_start = cycle;
			wb_read(R_LSR);
			while (!rd[5]) wb_read(R_LSR);
			waited = cycle - t_start;
			wb_write(R_THR, {24'b0, c});
		end
	endtask

	// Write bytes until THRE reads busy -- until one is really held. The
	// core's own IN FIFO takes the first few with no host at all (9, at
	// MAXPACKETSIZE 8), so the BIOS banner's first bytes never wait; the
	// one after that sits in the holding register and the timers start.
	// "Busy" has to LAST to count: the core takes bytes in bursts, and
	// THRE reads busy for a few cycles between them while it still has
	// room. Held means busy for 200 cycles in a row.
	task fill_until_held;
		begin
			nfill = 0;
			t_held = -1;
			while (t_held < 0 && nfill < 64) begin
				t_start = cycle;
				wb_read(R_LSR);
				while (!rd[5] && cycle - t_start < 200) wb_read(R_LSR);
				if (!rd[5]) t_held = t_start;
				else begin
					wb_write(R_THR, {24'b0, 8'h41 + nfill[7:0]});
					nfill = nfill + 1;
				end
			end
		end
	endtask

	initial begin

		errors = 0; checks = 0; cycle = 0;
		adr = 0; dat_w = 0; we = 0; stb = 0; cyc = 0;
		host_draining = 0; ntaken = 0;

		// -- 1. no host ---------------------------------------------------
		$display("=== 1. no host (a charger) ===");
		reset_dut;

		fill_until_held;			// the BIOS banner's first bytes
		check("a few bytes in, one is held (the core's FIFO is full)", nfill > 0 && nfill < 32);

		putchar("B");				// waits for the host window
		check("the wait ends by the end of the host window", cycle <= HOST_WAIT + 100);
		check("... not before it (a host still has time to appear)", cycle >= HOST_WAIT - 100);
		check("no host was seen", dut.no_host == 1'b1);

		for (i = 0; i < 100; i = i + 1) begin
			putchar(8'h40 + (i % 26));
			if (waited > 20) begin
				check("after the window every write is immediate", 1'b0);
				i = 1000;
			end
		end
		check("100 more bytes, none waited", i == 100);
		check("nothing was handed to the USB core", dut.usb_in_valid == 1'b0);

		// The transmit interrupt, as on a 16550 with nothing attached.
		wb_write(R_IER, 32'h02);
		repeat (3) @(posedge clk);
		check("THRE interrupt pending with IER[1] set", irq == 1'b1);
		wb_read(R_IIR);
		check("IIR says transmitter empty", rd[3:0] == 4'b0010);
		wb_write(R_IER, 32'h00);
		repeat (3) @(posedge clk);
		check("and gone with IER[1] clear", irq == 1'b0);

		// The BIOS's and the kernel's FCR, with the transmit reset bit.
		wb_write(R_IIR, 32'h07);
		putchar("@");
		check("an FCR transmit reset re-arms no wait", waited < 20);
		repeat (STALL + 100) @(posedge clk);
		wb_read(R_LSR);
		check("and THRE stays empty long after", rd[5] == 1'b1);

		// -- 2. host present --------------------------------------------
		$display("=== 2. host present ===");
		reset_dut;

		repeat (50) @(posedge clk);
		force dut.usb_frame = 11'd1;		// the host's first frame
		repeat (5) @(posedge clk);
		check("the host is seen", dut.host_seen == 1'b1);

		fill_until_held;
		check("a few bytes in, one is held", nfill > 0 && nfill < 32);
		repeat (HOST_WAIT + 200) @(posedge clk);
		wb_read(R_LSR);
		check("past the host window it is still held", !rd[5]);
		check("no_host never engages", dut.no_host == 1'b0);

		putchar("B");				// the old give-up, unchanged
		check("held for the full give-up, from when it stalled",
			cycle - t_held >= STALL - 100 && cycle - t_held <= STALL + 300);
		check("tx_giveup set", dut.tx_giveup == 1'b1);

		// The terminal opens: the host starts taking bytes.
		ntaken = 0;
		host_draining = 1'b1;
		force dut.usb_in_ready = 1'b1;
		repeat (5) @(posedge clk);
		check("recovery clears tx_giveup", dut.tx_giveup == 1'b0);
		for (i = 0; i < 20; i = i + 1) putchar("a" + i);
		repeat (5) @(posedge clk);
		check("every byte after recovery delivered", ntaken == 21);
		for (i = 0; i < 20; i = i + 1)
			if (taken[1 + i] != "a" + i) begin
				check("... in order", 1'b0);
				i = 100;
			end
		check("in order", i == 20);

		// -- 3. host late -----------------------------------------------
		$display("=== 3. host arrives after the window ===");
		reset_dut;

		fill_until_held;
		putchar("B");
		check("no host after the window", dut.no_host == 1'b1);
		putchar("x");
		check("dropping", waited < 20);

		force dut.usb_frame = 11'd7;		// plugged into a PC later
		repeat (3) @(posedge clk);
		check("its first frame ends no_host", dut.no_host == 1'b0);
		putchar("y");
		wb_read(R_LSR);
		check("bytes are held again", !rd[5]);
		ntaken = 0;
		host_draining = 1'b1;
		force dut.usb_in_ready = 1'b1;
		repeat (5) @(posedge clk);
		check("and delivered once it reads", ntaken == 1 && taken[0] == "y");

		// -- 4. configured is evidence too -----------------------------
		$display("=== 4. configured ===");
		reset_dut;
		force dut.usb_cdc_configured = 1'b1;
		repeat (HOST_WAIT + 100) @(posedge clk);
		check("configured: no_host never engages", dut.no_host == 1'b0);

		$display("");
		if (errors == 0) $display("=== tb_usb_cdc_uart: PASS (%0d checks) ===", checks);
		else $display("=== tb_usb_cdc_uart: FAIL (%0d of %0d) ===", errors, checks);
		$finish;

	end

endmodule
