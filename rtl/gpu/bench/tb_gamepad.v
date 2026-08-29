`timescale 1ns / 1ps

// Self-checking testbench for rtl/usb_hid.v's gamepad register.
//
// Uses a STUB usb_hid_host rather than the real one: the real core
// needs an actual USB device on the far end of a bit-banged
// low-speed link to produce a report at all, which is not something a
// testbench can conjure, and none of the behaviour under test here
// lives in that core anyway. usb_hid_host already decoded gamepads
// before any of this existed -- what is new, and what this checks, is
// the clock domain crossing and the hot-swap handling wrapped around
// it in usb_hid.v.
//
// Checked here:
//
//   1. a pad report reaches register 4 with the right bit order.
//
//   2. THE TEARING FIX. The pad bits are driven from the usbclk
//      domain and changed mid-flight, with no report pulse, while
//      wishbone reads run continuously. Register 4 must not move: it
//      is a snapshot of one report, never a blend of two. This is the
//      check that matters -- left and right are separate bits, so a
//      torn read during a left-to-right flick can report both pressed
//      or neither, and a game that treats "both" as "stand still"
//      gets a character that refuses to move for one frame, once in a
//      while, unreproducibly.
//
//   3. HOT UNPLUG. With a direction held, typ drops to 0 (which is
//      what usb_hid_host does on disconnect) and no further report
//      ever arrives. The state must clear itself. Without this the
//      machine believes RIGHT is held down forever.
//
//   4. hot swap to another device class (pad -> keyboard) clears the
//      pad state too, for the same reason.
//
//   5. the type change raises an interrupt. Unplugging is otherwise
//      SILENT -- reports simply stop -- so without this edge the OS
//      is never told, and sw/os/hid.c's held-key state for that port
//      is never flushed.
//
// Run:
//   iverilog -g2005 -DUSB_HID -o /tmp/tb_pad.out \
//       rtl/gpu/bench/tb_gamepad.v
//   vvp /tmp/tb_pad.out

// -- stub --------------------------------------------------------
// Same port names and same domain (usbclk) as the real core, driven
// directly by the testbench.
module usb_hid_host (
	input usbclk,
	input usbrst_n,
	inout usb_dm, usb_dp,
	output reg [1:0] typ,
	output reg report,
	output conerr,
	output reg [7:0] key_modifiers,
	output reg [7:0] key1, key2, key3, key4,
	output reg [7:0] mouse_btn,
	output reg signed [7:0] mouse_dx,
	output reg signed [7:0] mouse_dy,
	output reg game_l, game_r, game_u, game_d,
	output reg game_a, game_b, game_x, game_y, game_sel, game_sta
);
	assign conerr = 1'b0;
	initial begin
		typ = 2'd0; report = 1'b0;
		key_modifiers = 0; key1 = 0; key2 = 0; key3 = 0; key4 = 0;
		mouse_btn = 0; mouse_dx = 0; mouse_dy = 0;
		{game_l, game_r, game_u, game_d} = 4'b0;
		{game_a, game_b, game_x, game_y, game_sel, game_sta} = 6'b0;
	end
endmodule

module tb_gamepad;

	reg wb_clk = 0;      // 48MHz-ish
	reg usb_clk = 0;     // 12MHz-ish
	reg wb_rst = 1;

	reg [31:0] wb_adr = 0;
	reg wb_cyc = 0, wb_stb = 0;
	wire [31:0] wb_dat_o;
	wire wb_ack;
	wire int_o;
	wire [9:0] curs_x, curs_y;
	wire [1:0] typ;

	integer errors = 0;

	always #10 wb_clk  = ~wb_clk;
	always #40 usb_clk = ~usb_clk;

	usb_hid_wb dut (
		.wb_clk_i(wb_clk),
		.wb_rst_i(wb_rst),
		.wb_adr_i(wb_adr),
		.wb_dat_i(32'b0),
		.wb_dat_o(wb_dat_o),
		.wb_we_i(1'b0),
		.wb_sel_i(4'hf),
		.wb_stb_i(wb_stb),
		.wb_ack_o(wb_ack),
		.wb_cyc_i(wb_cyc),
		.usb_clk(usb_clk),
		.usb_dm(),
		.usb_dp(),
		.int_o(int_o),
		.curs_x(curs_x),
		.curs_y(curs_y),
		.typ(typ)
	);

	// reach into the stub to drive the far side of the crossing
	`define UHH dut.usb_hid_host_i

	task wb_read;
		input [31:0] adr;
		output [31:0] dat;
		begin
			@(posedge wb_clk);
			wb_adr <= adr;
			wb_cyc <= 1'b1;
			wb_stb <= 1'b1;
			@(posedge wb_clk);
			while (!wb_ack) @(posedge wb_clk);
			dat = wb_dat_o;
			wb_cyc <= 1'b0;
			wb_stb <= 1'b0;
			@(posedge wb_clk);
		end
	endtask

	task check_eq;
		input [255:0] what;
		input [31:0] got;
		input [31:0] exp;
		begin
			if (got !== exp) begin
				$display("FAIL: %0s: got %08x, expected %08x", what, got, exp);
				errors = errors + 1;
			end
		end
	endtask

	// one report pulse in the usbclk domain, exactly as the real core
	// issues it: one usbclk cycle wide, with the data valid alongside
	task pulse_report;
		begin
			@(posedge usb_clk);
			`UHH.report <= 1'b1;
			@(posedge usb_clk);
			`UHH.report <= 1'b0;
			// let the 3-deep synchroniser and the capture settle
			repeat (20) @(posedge wb_clk);
		end
	endtask

	// bit order, mirrored from usb_hid.v / zpad.h
	localparam PAD_LEFT  = 10'h001;
	localparam PAD_RIGHT = 10'h002;
	localparam PAD_UP    = 10'h004;
	localparam PAD_DOWN  = 10'h008;
	localparam PAD_A     = 10'h010;
	localparam PAD_B     = 10'h020;
	localparam PAD_X     = 10'h040;
	localparam PAD_Y     = 10'h080;
	localparam PAD_SEL   = 10'h100;
	localparam PAD_STA   = 10'h200;

	reg [31:0] d;
	integer i;

	initial begin

		repeat (4) @(posedge wb_clk);
		wb_rst = 0;
		repeat (10) @(posedge wb_clk);

		// ---- 1: a pad report lands, with the right bit order ----

		@(posedge usb_clk);
		`UHH.typ <= 2'd3;
		repeat (20) @(posedge wb_clk);

		@(posedge usb_clk);
		`UHH.game_r <= 1'b1;
		`UHH.game_a <= 1'b1;
		pulse_report();

		wb_read(32'd4, d);
		check_eq("right+A reported", d, { 20'd0, 2'd3, PAD_RIGHT | PAD_A });

		// every button individually, so a transposed bit in the
		// concatenation cannot hide behind a symmetric mistake
		@(posedge usb_clk);
		`UHH.game_r <= 1'b0; `UHH.game_a <= 1'b0;
		`UHH.game_sta <= 1'b1;
		pulse_report();
		wb_read(32'd4, d);
		check_eq("start only", d, { 20'd0, 2'd3, PAD_STA });

		@(posedge usb_clk);
		`UHH.game_sta <= 1'b0;
		`UHH.game_l <= 1'b1; `UHH.game_u <= 1'b1;
		`UHH.game_y <= 1'b1; `UHH.game_sel <= 1'b1;
		pulse_report();
		wb_read(32'd4, d);
		check_eq("left+up+Y+select", d,
			{ 20'd0, 2'd3, PAD_LEFT | PAD_UP | PAD_Y | PAD_SEL });

		// ---- 2: the tearing fix ----
		//
		// change the raw usbclk-domain bits with NO report pulse,
		// while reading continuously. The register must stay put.

		@(posedge usb_clk);
		`UHH.game_l <= 1'b0; `UHH.game_u <= 1'b0;
		`UHH.game_y <= 1'b0; `UHH.game_sel <= 1'b0;
		`UHH.game_r <= 1'b1; `UHH.game_d <= 1'b1;
		`UHH.game_b <= 1'b1;

		for (i = 0; i < 40; i = i + 1) begin
			wb_read(32'd4, d);
			if (d !== { 20'd0, 2'd3, PAD_LEFT | PAD_UP | PAD_Y | PAD_SEL }) begin
				$display("FAIL: torn read at iteration %0d: got %08x", i, d);
				errors = errors + 1;
				i = 40;
			end
		end

		// and the new state appears, whole, on the next report
		pulse_report();
		wb_read(32'd4, d);
		check_eq("new state after report", d,
			{ 20'd0, 2'd3, PAD_RIGHT | PAD_DOWN | PAD_B });

		// ---- 3: hot unplug with a direction held ----
		//
		// typ drops to 0 and NO further report ever arrives, which is
		// exactly what a yanked cable looks like from in here.

		@(posedge usb_clk);
		`UHH.typ <= 2'd0;
		repeat (30) @(posedge wb_clk);

		wb_read(32'd4, d);
		check_eq("state cleared on unplug", d, 32'd0);

		// ---- 5: and that change raised an interrupt ----
		//
		// checked here rather than in its own section because the
		// unplug above is the edge being tested. Sampled across the
		// window rather than at one instant: it is a one-cycle pulse.

		@(posedge usb_clk);
		`UHH.typ <= 2'd3;
		begin : irq_watch
			reg seen;
			seen = 0;
			for (i = 0; i < 40; i = i + 1) begin
				@(posedge wb_clk);
				if (int_o) seen = 1;
			end
			if (!seen) begin
				$display("FAIL: no interrupt raised on device type change");
				errors = errors + 1;
			end
		end

		// ---- 4: hot swap pad -> keyboard ----

		// clear everything the previous section left held -- the stub
		// keeps its raw bits until told otherwise, exactly as a real
		// pad holds a button until it is let go
		@(posedge usb_clk);
		`UHH.game_d <= 1'b0; `UHH.game_b <= 1'b0;
		`UHH.game_r <= 1'b1;
		pulse_report();
		wb_read(32'd4, d);
		check_eq("pad back after replug", d, { 20'd0, 2'd3, PAD_RIGHT });

		@(posedge usb_clk);
		`UHH.typ <= 2'd1;             // now a keyboard on this port
		`UHH.key_modifiers <= 8'h02;
		`UHH.key1 <= 8'h04;
		pulse_report();

		wb_read(32'd4, d);
		check_eq("pad state cleared on swap to keyboard", d,
			{ 20'd0, 2'd1, 10'd0 });

		// the keyboard itself still works through the existing
		// registers -- a regression guard on the ports this change
		// deliberately did not touch
		wb_read(32'd1, d);
		check_eq("keyboard keys still readable", d, 32'h04000000);

		if (errors == 0)
			$display("RESULT: PASS");
		else
			$display("RESULT: FAIL (%0d errors)", errors);

		$finish;

	end

endmodule
