/*
 * Zeitlos SOC -- simulation only.
 *
 * The USB DEVICE co-simulation (docs/usb_dev.md): rtl/usbd_ep.v run by
 * the real sw/apps/usbd and net's real usbd_eth.c (rtl/tb/cosim/
 * usbd_vpi.c), on the wire against the real rtl/usb host gateware run
 * by the real sw/apps/usb driver (usbh_vpi.c). Neither side is a model:
 * both are what runs on the boards, and they were written separately.
 *
 * The device's CPU is scheduled as the kernel would: when its interrupt
 * line (irq_o) rises, the "kernel" writes IRQEN = 0 -- the silence
 * write -- and runs one pass of usbd's loop, which ends by writing
 * IRQEN back. Between those, the CPU is the host's: every host bus
 * cycle and every clock of the host's own pauses checks the line. A
 * pass also runs every POLL bus cycles, as usbd's timeout does while a
 * terminal is open (it is how new console output is noticed); with
 * POLL 0 nothing runs but on an interrupt, so a missing IRQEN bit
 * shows as a stall, not as slowness.
 *
 * Three runs:
 *
 *   (default)   the host binds the CDC-ACM function: the console. The
 *               boot log replayed when the port opens, live output,
 *               output in multiples of 64, typing (into a ring that
 *               takes a few bytes at a time, too), and a restart of
 *               usbd: off the bus and back, enumerated again, the log
 *               replayed again.
 *   COSIM_ECM   the host (built with Z_USBH_COSIM_NO_SER) binds the
 *               CDC-ECM function: the MAC string, the link coming up
 *               and down as net does, frames of every length that
 *               matters both ways, a frame held off by full slots, and
 *               tx_room.
 *   COSIM_MSC   the device is `share`'s (usbd_cosim_msc: -DUSBD_MSC),
 *               five interfaces; the host (built with NO_SER and
 *               NO_ECM) binds the card reader: bring-up through UNIT
 *               ATTENTION, reads and writes checked against the card
 *               on both sides, and the medium taken away.
 *
 *   make test_usbd
 */

`timescale 1ns/1ps

module tb_usbd_cosim;

	reg clk;
	reg rst;

	// the host controller's bus (usbh_vpi.c)
	reg [10:0] wb_adr;
	reg [31:0] wb_wdat;
	wire [31:0] wb_rdat;
	reg wb_we;
	reg [3:0] wb_sel;
	reg wb_stb;
	reg wb_cyc;
	wire wb_ack;
	wire usb_int;

	// the device's bus (usbd_vpi.c)
	reg [12:0] d_adr;
	reg [31:0] d_wdat;
	wire [31:0] d_rdat;
	reg d_we;
	reg d_stb;
	reg d_cyc;
	wire d_ack;
	wire d_irq;
	wire d_netrx;

	wire [1:0] dp;
	wire [1:0] dm;
	wire d_pu;

	wire [9:0] curs_x0, curs_y0, curs_x1, curs_y1;
	wire [1:0] typ0, typ1;
	wire hid0_int, hid1_int;

	integer q_valid, q_we, q_adr, q_wdat, q_bw;
	integer rdat;
	integer dv, dwe, dadr, dwd, drd;
	integer drc, dbad, dfirst;
	integer rc, bad, first_bad;
	integer guard, errors, k, n, wa, lane, i;
	integer poll_every, poll_ctr, wakes, polls;
	integer in_dev;
	reg netrx_q;		// net's interrupt is an edge (rtl/sysctl.v)
	integer lens [0:9];

	// the host's 15k pull-downs on both ports
	assign (highz1, weak0) dp[0] = 1'b0;
	assign (highz1, weak0) dm[0] = 1'b0;
	assign (highz1, weak0) dp[1] = 1'b0;
	assign (highz1, weak0) dm[1] = 1'b0;

	// the device's 1.5k pull-up on D+, switched by usb_pu (driven high
	// or released), at pull strength as tb_usb_device.v's
	assign (pull1, highz0) dp[0] = (d_pu === 1'b1) ? 1'b1 : 1'b0;

	initial clk = 1'b0;
	always #10.4167 clk = ~clk;		// 48 MHz: the host's

	// The device's own 48 MHz, as on two real boards: another phase and
	// +300 ppm (USB allows 2500). On ONE clock the host would change the
	// lines on the very edges the device samples on -- a race no
	// hardware has, which lost a byte of the first packet after an SOF.
	`ifndef DEV_PPM
	`define DEV_PPM 300
	`endif
	reg dclk;
	initial begin
		dclk = 1'b0;
		#7.3;
		forever #(10.4167 * (1.0 - `DEV_PPM * 1.0e-6)) dclk = ~dclk;
	end

	usb_host #(
		.PORTS(2),
		.T_MS(32'd20),
		.DEBOUNCE_MS(32'd100),
		.RESET_MS(32'd10),
		.RECOVERY_MS(32'd10),
		.T_FRAME(32'd48000)
	) host (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(wb_adr),
		.wb_dat_i(wb_wdat),
		.wb_dat_o(wb_rdat),
		.wb_we_i(wb_we),
		.wb_sel_i(wb_sel),
		.wb_stb_i(wb_stb),
		.wb_ack_o(wb_ack),
		.wb_cyc_i(wb_cyc),
		.usb_dp(dp),
		.usb_dm(dm),
		.curs_x0(curs_x0), .curs_y0(curs_y0),
		.curs_x1(curs_x1), .curs_y1(curs_y1),
		.typ0(typ0), .typ1(typ1),
		.hid0_int_o(hid0_int), .hid1_int_o(hid1_int),
		.int_o(usb_int)
	);

	`ifndef DEV_RX_SLOTS
	`define DEV_RX_SLOTS 2
	`endif
	usbd_ep #(.RX_SLOTS(`DEV_RX_SLOTS)) dev (
		.clk(dclk),
		.rst(rst),
		.wb_adr_i(d_adr),
		.wb_dat_i(d_wdat),
		.wb_dat_o(d_rdat),
		.wb_sel_i(4'hf),
		.wb_we_i(d_we),
		.wb_stb_i(d_stb),
		.wb_cyc_i(d_cyc),
		.wb_ack_o(d_ack),
		.irq_o(d_irq),
		.net_rx_o(d_netrx),
		.usb_dp(dp[0]),
		.usb_dn(dm[0]),
		.usb_pu(d_pu)
	);

	// -- contention: host and device driving at once shows as x --
	integer xcount;
	initial xcount = 0;
	always @(dp[0] or dm[0])
		if (dp[0] === 1'bx || dm[0] === 1'bx) begin
			xcount = xcount + 1;
			if (xcount <= 4)
				$display("[wire %0t] CONTENTION", $time);
		end

	// -- monitors, for debugging: set `mon` around what to look at. The
	// SIE's packet ends on endpoint 4 and toggles, the host's frames
	// and transactions, and raw line changes (decode them offline) --
	reg mon;
	initial mon = 1'b0;
	always @(posedge dclk) if (mon && dev.gate) begin
		if (dev.out_ready && !dev.out_valid)
			$display("[mon %0t] ep %0d %s%s%s wp %0d drop %0d/%0d rx_n %0d fwp %0d tog %0d en %0d",
				$time / 1000, dev.endp, dev.setup ? "SETUP " : "",
				dev.out_err ? "ERR " : "", (!dev.setup && !dev.out_err) ? "EOP" : "",
				dev.wp, dev.pkt_drop, dev.hs_drop, dev.rx_n, dev.fwp,
				dev.sie_i.out_toggle_q[4], dev.ep4_en);
		if (dev.in_ready && dev.in_req)
			$display("[mon %0t] IN ep %0d", $time / 1000, dev.endp);
	end
	// raw line changes after the first frame tick seen while mon is on
	integer mon_lines;
	reg mon_arm;
	initial begin mon_lines = 0; mon_arm = 0; end
	always @(posedge clk) if (mon) mon_arm <= 1'b1;
	always @(dp[0] or dm[0]) if (mon && mon_arm && mon_lines < 3000) begin
		mon_lines = mon_lines + 1;
		$display("[line %0t] %b%b", $realtime, dp[0], dm[0]);
	end
	reg mon_tog_q;
	always @(posedge clk) begin
		if (mon && dev.sie_i.out_toggle_q[4] !== mon_tog_q)
			$display("[mon %0t] device's OUT toggle on ep 4 -> %0d (wr_toggle %0d tog_out4 %0d, en %0d)",
				$time / 1000, dev.sie_i.out_toggle_q[4], dev.wr_toggle, dev.tog_out[4], dev.ep4_en);
		mon_tog_q <= dev.sie_i.out_toggle_q[4];
	end
	reg mon_busy_q;
	always @(posedge clk) if (mon) begin
		mon_busy_q <= host.x_busy;
		if (host.frame_tick)
			$display("[mon %0t] host frame tick", $time / 1000);
		if (host.x_busy && !mon_busy_q)
			$display("[mon %0t] host starts %s", $time / 1000,
				host.sched_is_sof ? "SOF" : "a transaction");
		if (!host.x_busy && mon_busy_q)
			$display("[mon %0t] host done", $time / 1000);
	end

	// -- the device's CPU --

	task dbus;
		input integer a;
		input integer we;
		input integer wd;
		begin
			@(posedge dclk);
			d_adr <= a[12:0];
			d_we <= we[0];
			d_wdat <= wd;
			d_stb <= 1'b1;
			d_cyc <= 1'b1;
			@(posedge dclk);
			guard = 0;
			while (!d_ack && guard < 100) begin
				@(posedge dclk);
				guard = guard + 1;
			end
			if (guard >= 100) begin
				$display("  FAIL device bus timeout at %08x", a);
				errors = errors + 1;
			end
			drd = d_rdat;
			d_stb <= 1'b0;
			d_cyc <= 1'b0;
			d_we <= 1'b0;
		end
	endtask

	task dev_step;
		begin
			in_dev = 1;
			$usbd_step(dv, dwe, dadr, dwd);
			while (dv) begin
				dbus(dadr, dwe, dwd);
				$usbd_done(drd, dv, dwe, dadr, dwd);
			end
			in_dev = 0;
		end
	endtask

	task dev_op;
		input integer op;
		input integer arg;
		begin
			$usbd_arm(op, arg);
			dev_step;
			$usbd_result(drc, dbad, dfirst);
		end
	endtask

	// the kernel: the line fired -- silence it, run usbd; or a frame
	// arrived (Z_IRQ_ETH, a rising edge) -- run net
	task kernel;
		begin
			if (!in_dev) begin
				if (d_netrx === 1'b1 && !netrx_q) begin
					netrx_q = 1'b1;
					wakes = wakes + 1;
					dev_step;
				end else if (d_netrx !== 1'b1)
					netrx_q = 1'b0;
				if (d_irq === 1'b1) begin
					wakes = wakes + 1;
					dbus(32'h018, 1, 0);	// IRQEN = 0, the silence write
					dev_step;
				end else if (poll_every) begin
					poll_ctr = poll_ctr + 1;
				end
				if (!d_irq && poll_every && poll_ctr >= poll_every) begin
					poll_ctr = 0;
					polls = polls + 1;
					dev_step;
				end
			end
		end
	endtask

	// -- the host's CPU (as tb_usb_ecm_cosim.v) --

	task bus_cycle;
		input integer a;
		input integer we;
		input integer wd;
		input integer bw;
		begin
			wa = (a & 32'h0fffffff) >> 2;
			lane = a & 3;
			@(posedge clk);
			wb_adr <= wa[10:0];
			wb_we <= we[0];
			if (we && bw) begin
				wb_sel <= (4'b0001 << lane);
				wb_wdat <= wd << (8 * lane);
			end else begin
				wb_sel <= 4'hf;
				wb_wdat <= wd;
			end
			wb_stb <= 1'b1;
			wb_cyc <= 1'b1;
			@(posedge clk);
			guard = 0;
			while (!wb_ack && guard < 1000) begin
				@(posedge clk);
				guard = guard + 1;
			end
			if (guard >= 1000) begin
				$display("  FAIL host bus timeout at %08x", a);
				errors = errors + 1;
			end
			if (bw) rdat = (wb_rdat >> (8 * lane)) & 32'hff;
			else rdat = wb_rdat;
			wb_stb <= 1'b0;
			wb_cyc <= 1'b0;
			wb_we <= 1'b0;
			kernel;		// the device's CPU runs between the host's cycles
		end
	endtask

	task driver_step;
		begin
			$usbh_step(q_valid, q_we, q_adr, q_wdat, q_bw);
			while (q_valid) begin
				bus_cycle(q_adr, q_we, q_wdat, q_bw);
				$usbh_done(rdat, q_valid, q_we, q_adr, q_wdat, q_bw);
			end
		end
	endtask

	task run_for;
		input integer steps;
		begin
			for (k = 0; k < steps; k = k + 1) begin
				driver_step;
				$usbh_tick;
				$usbd_tick;
				for (n = 0; n < 8; n = n + 1) begin
					repeat (8) @(posedge clk);
					kernel;
				end
			end
		end
	endtask

	task host_op;
		input integer op;
		input integer arg;
		begin
			$usbh_msc_arm(op, arg);
			driver_step;
			$usbh_msc_result(rc, bad, first_bad);
		end
	endtask

	// Until the host has bound the function: the PHY holds the pull-up
	// off for its 16 ms attach debounce first (phy_rx.v, TSIGATT), then
	// the host's debounce, reset and enumeration.
	task wait_bound;
		input integer op;
		integer tries;
		begin
			tries = 0;
			rc = 0;
			while (!(rc & 1) && tries < 80) begin
				run_for(500);
				host_op(op, 0);
				tries = tries + 1;
			end
			$display("  (bound after %0d steps, %0t ns)", tries * 500, $time / 1000);
		end
	endtask

	// The host reads the notification endpoint from an idle receive,
	// four times a second (usbh_ecm.c, ECM_LINK_TICKS): two of them.
	task link_poll;
		begin
			run_for(200);
			host_op(ECM_RECV, 0);
			run_for(200);
			host_op(ECM_RECV, 0);
		end
	endtask

	task check;
		input [255:0] name;
		input integer got;
		input integer want;
		begin
			if (got !== want) begin
				$display("  FAIL %0s: got %0d want %0d", name, got, want);
				errors = errors + 1;
			end else begin
				$display("  ok   %0s = %0d", name, got);
			end
		end
	endtask

	// usbd_vpi.c's device ops
	localparam D_START = 0, D_STATE = 1, D_CON_PUSH = 2, D_CON_STATS = 3,
		D_NET_UP = 4, D_NET_DOWN = 5, D_NET_SEND = 6, D_NET_STATS = 7,
		D_DETACH = 8, D_RING = 9, D_NET_HOLD = 10, D_TX_ROOM = 11,
		D_MSC_ON = 12, D_MSC_OFF = 13, D_MSC_STATS = 14, D_MSC_CHECK = 15;
	// usbh_vpi.c's host ops
	localparam ECM_INFO = 9, ECM_RECV = 10, ECM_SEND = 11, ECM_STATS = 13,
		SER_INFO = 15, SER_READ = 16, SER_WRITE = 17, SER_RESET = 18,
		LSUSB = 4, MSC_START = 0, MSC_READ = 1, MSC_WRITE = 2, MSC_STATE = 3,
		MSC_READN = 7, MSC_WRITEN = 8;

	initial begin
		errors = 0; wakes = 0; polls = 0; poll_ctr = 0; in_dev = 0; netrx_q = 0;
		poll_every = 0;
		rst = 1'b1;
		wb_adr = 0; wb_wdat = 0; wb_we = 0; wb_sel = 0; wb_stb = 0; wb_cyc = 0;
		d_adr = 0; d_wdat = 0; d_we = 0; d_stb = 0; d_cyc = 0;
		lens[0] = 60; lens[1] = 64; lens[2] = 100; lens[3] = 128; lens[4] = 511;
		lens[5] = 512; lens[6] = 513; lens[7] = 1024; lens[8] = 1472; lens[9] = 1514;
		repeat (20) @(posedge clk);
		rst = 1'b0;
		repeat (20) @(posedge clk);

		$display("");
		$display("== usbd cosim: real usbd + usbd_ep.v against the real USB host ==");

		run_for(4);

		// The kernel has been printing since boot, before usbd ran.
		dev_op(D_CON_PUSH, 3000);
`ifdef COSIM_MSC
		dev_op(D_MSC_ON, 0);		// share: the card is the host's from the start
`endif
		dev_op(D_START, 0);

		$display("");
		$display("== enumeration ==");
`ifdef COSIM_MSC
		wait_bound(MSC_STATE);
`elsif COSIM_ECM
		wait_bound(ECM_INFO);
`else
		wait_bound(SER_INFO);
`endif
		run_for(500);
		dev_op(D_STATE, 0);
		check("configured", drc & 1, 1);
		host_op(LSUSB, 0);

`ifdef COSIM_MSC
		// -------------------------------------------------- the card
		host_op(MSC_STATE, 0);
		check("host has the card reader", rc & 1, 1);

		$display("");
		$display("== bring-up: UNIT ATTENTION, then ready ==");
		host_op(MSC_START, 0);
		check("started", rc, 0);
		host_op(MSC_STATE, 0);
		check("ready", (rc >> 1) & 1, 1);

		$display("");
		$display("== reads ==");
		host_op(MSC_READ, 0);
		check("sector 0 (boot signature)", rc, 0);
		check("wrong", bad, 0);
		host_op(MSC_READ, 1);
		check("sector 1", rc, 0);
		check("wrong", bad, 0);
		host_op(MSC_READ, 13);
		check("sector 13", rc, 0);
		check("wrong", bad, 0);
		host_op(MSC_READN, 4);
		check("sectors 4-7, one call", rc, 0);
		check("wrong", bad, 0);

		$display("");
		$display("== writes ==");
		host_op(MSC_WRITE, 3);
		check("sector 3 written and read back", rc, 0);
		check("wrong", bad, 0);
		dev_op(D_MSC_CHECK, 3);
		check("on the card", dbad, 0);
		host_op(MSC_WRITEN, 8);
		check("8-11 written, read back", rc, 0);
		check("wrong", bad, 0);
		dev_op(D_MSC_CHECK, 8);
		check("8 on the card", dbad, 0);
		dev_op(D_MSC_CHECK, 11);
		check("11 on the card", dbad, 0);
		host_op(MSC_READ, 2);
		check("sector 2 untouched", rc, 0);
		check("wrong", bad, 0);
		dev_op(D_MSC_STATS, 0);
		check("sectors written to the card", dbad, 5);
		check("not ejected", dfirst, 0);

		$display("");
		$display("== Eject here: the medium goes ==");
		dev_op(D_MSC_OFF, 0);
		host_op(MSC_READ, 1);
		check("a read is refused", rc != 0, 1);
		dev_op(D_STATE, 0);
		check("still configured", drc & 1, 1);
`else
`ifndef COSIM_ECM
		// -------------------------------------------------- the console
		host_op(SER_INFO, 0);
		check("host has a serial port", rc & 1, 1);
		check("kind CDC-ACM", rc >> 1, 1);
		dev_op(D_STATE, 0);
		check("DTR seen (port open)", (drc >> 1) & 1, 1);

		poll_every = 400;
		$display("");
		$display("== the boot log, replayed when the port opens ==");
		host_op(SER_READ, 3000);
		check("bytes", rc, 3000);
		check("wrong", bad, 0);

		$display("");
		$display("== live output ==");
		dev_op(D_CON_PUSH, 900);
		host_op(SER_READ, 900);
		check("bytes", rc, 900);
		check("wrong", bad, 0);
		dev_op(D_CON_PUSH, 64);
		host_op(SER_READ, 64);
		check("64 bytes (a full packet, then a short one)", rc, 64);
		check("wrong", bad, 0);
		dev_op(D_CON_PUSH, 128);
		host_op(SER_READ, 128);
		check("128 bytes", rc, 128);
		check("wrong", bad, 0);

		$display("");
		$display("== typing ==");
		host_op(SER_WRITE, 500);
		check("host wrote", rc, 500);
		run_for(400);
		dev_op(D_CON_STATS, 0);
		check("kernel got", drc, 500);
		check("wrong", dbad, 0);
		dev_op(D_RING, 7);		// a nearly full ring: 7 bytes a call
		host_op(SER_WRITE, 300);
		check("host wrote", rc, 300);
		run_for(800);
		dev_op(D_CON_STATS, 0);
		check("kernel got, 7 at a time", drc, 800);
		check("wrong", dbad, 0);
		dev_op(D_RING, 0);

		$display("");
		$display("== usbd restarted: off the bus and back ==");
		dev_op(D_DETACH, 0);
		run_for(3000);
		host_op(SER_INFO, 0);
		check("host's serial port gone", rc & 1, 0);
		dev_op(D_START, 0);
		wait_bound(SER_INFO);
		check("serial port back", rc & 1, 1);
		host_op(SER_RESET, 0);
		host_op(SER_READ, 3000 + 900 + 64 + 128);
		check("the log replayed again", rc, 3000 + 900 + 64 + 128);
		check("wrong", bad, 0);
`else
		// -------------------------------------------------- the network
		host_op(ECM_INFO, 0);
		check("host has the ECM function", rc & 1, 1);
		check("MAC string read (5 bytes differ from the RTL8152's)", bad, 5);
		check("no link before net", (rc >> 2) & 1, 0);
		dev_op(D_STATE, 0);
		check("data interface alt 1", (drc >> 2) & 1, 1);
		check("frame endpoint enabled", (dbad >> 2) & 1, 1);

		$display("");
		$display("== net comes up ==");
		dev_op(D_NET_UP, 0);
		link_poll;
		host_op(ECM_INFO, 0);
		check("link up (NETWORK_CONNECTION)", (rc >> 2) & 1, 1);

		$display("");
		$display("== frames to the device ==");
		for (i = 0; i < 10; i = i + 1) begin
			host_op(ECM_SEND, lens[i] | ((i + 1) << 16));
			run_for(300);
			dev_op(D_NET_STATS, 0);
			if (rc != 0 || drc != i + 1 || dfirst != lens[i] || dbad != 0) begin
				$display("  FAIL %0d bytes: send %0d, frames %0d, last %0d, wrong %0d",
					lens[i], rc, drc, dfirst, dbad);
				errors = errors + 1;
			end else
				$display("  ok   %0d bytes", lens[i]);
		end

		$display("");
		$display("== frames from the device ==");
		for (i = 0; i < 10; i = i + 1) begin
			dev_op(D_NET_SEND, lens[i] | ((i + 40) << 16));
			check("send accepted", drc, 1);
			run_for(400);
			host_op(ECM_RECV, 0);
			if (rc != lens[i] || bad != 0 || first_bad != i + 40) begin
				$display("  FAIL %0d bytes: got %0d, %0d wrong, seed %0d",
					lens[i], rc, bad, first_bad);
				errors = errors + 1;
			end else
				$display("  ok   %0d bytes", lens[i]);
		end

		$display("");
		$display("== the receive slots full: the next frame waits in the host ==");
		dev_op(D_NET_HOLD, 1);
		host_op(ECM_SEND, 300 | (61 << 16));
		check("first sent", rc, 0);
		if (`DEV_RX_SLOTS == 2) begin
			host_op(ECM_SEND, 400 | (62 << 16));
			check("second sent", rc, 0);
		end
		run_for(300);
		dev_op(D_STATE, 0);
		check("a frame is ready", dbad & 1, 1);
		dev_op(D_NET_HOLD, 0);
		dev_step;		// net's backstop timer: the edge has passed
		run_for(300);
		dev_op(D_NET_STATS, 0);
		check("all arrived", drc, 10 + `DEV_RX_SLOTS);
		check("wrong", dbad, 0);
		// This frame's second packet ends in a CRC with six ones: the
		// host must stuff a zero before its EOP (rtl/usb/usb_sie.v,
		// tx_stuff_eop), and before that fix it did not.
		host_op(ECM_SEND, 500 | (63 << 16));
		check("third sent", rc, 0);
		run_for(300);
		dev_op(D_NET_STATS, 0);
		check("and one more after", drc, 11 + `DEV_RX_SLOTS);
		check("its length", dfirst, 500);

		$display("");
		$display("== tx_room ==");
		dev_op(D_NET_SEND, 1000 | (70 << 16));
		dev_op(D_TX_ROOM, 0);
		check("no room while the host has not taken it", drc, 0);
		run_for(400);
		dev_op(D_TX_ROOM, 0);
		check("still none: this host reads only when asked", drc, 0);
		host_op(ECM_RECV, 0);
		check("the frame", rc, 1000);
		check("wrong", bad, 0);
		dev_op(D_TX_ROOM, 0);
		check("room once it has", drc, 1);

		$display("");
		$display("== net goes down ==");
		dev_op(D_NET_DOWN, 0);
		link_poll;
		host_op(ECM_INFO, 0);
		check("link down", (rc >> 2) & 1, 0);
		host_op(ECM_STATS, 0);
		check("host: transmit errors", first_bad, 0);
		check("host: receive errors", bad, 0);
`endif
`endif

		$display("");
		$display("device CPU: %0d interrupts, %0d polls", wakes, polls);
		check("wire contention", xcount, 0);
		if (errors == 0) $display("PASS");
		else $display("FAIL: %0d", errors);
		$finish;
	end

	// a run that hangs is a failure, not a wait
	initial begin
		#400_000_000;
		$display("FAIL: timeout");
		$finish;
	end

endmodule
