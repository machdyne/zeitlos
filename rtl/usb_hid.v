/*
 * Zeitlos SOC 
 a Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * USB HID wishbone interface
 *
 */

// -- pointer acceleration --
//
// Undefine USB_HID_ACCEL for a strictly linear 1:1 pointer: one HID
// count moves the cursor exactly one pixel, which is the finest this
// hardware can do and, on a high-DPI mouse, slow to cross the screen
// with. With it defined, deltas larger than USB_HID_ACCEL_THRESHOLD
// are doubled, so fine positioning keeps full precision while a sweep
// across the screen stays quick.
//
// Raise the threshold to make acceleration kick in later. The shift
// applied is in the module body; change <<< 1 to <<< 2 there for a
// more aggressive curve.
//
// Defined at file scope rather than inside the module: a `define
// anywhere in a file is global to the whole compilation from that
// point on, so putting one in a module body only hides that fact.
`define USB_HID_ACCEL
`define USB_HID_ACCEL_THRESHOLD 3

module usb_hid_wb #()
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
	input usb_clk,
	inout usb_dm,
	inout usb_dp,
	output int_o,
	output [9:0] curs_x,
	output [9:0] curs_y,
	output [1:0] typ,
);

`ifdef USB_HID

	wire uhh_report;
	wire uhh_dr;

	wire [1:0] uhh_usb_type;
	wire [7:0] uhh_key_modifiers, uhh_key1, uhh_key2, uhh_key3, uhh_key4;
	wire [7:0] uhh_mouse_btn;
	wire signed [7:0] uhh_mouse_dx, uhh_mouse_dy;

	assign int_o = uhh_report;

	// exposed as a plain wire (not just readable via the info register
	// at wb_adr_i[2:0]==0) so sysctl.v can select which instance's
	// curs_x/curs_y feeds the hardware cursor sprite (GPU_CURSOR)
	// without a wishbone read round-trip -- see sysctl.v's usb_cursor
	// mux, added alongside the second usb_hid_wb instance for port 1.
	assign typ = uhh_usb_type;

	reg [9:0] curs_x;
	reg [9:0] curs_y;

	reg signed [7:0] mouse_dx;
	reg signed [7:0] mouse_dy;

	reg mouse_move;

	// -- report pulse: usb_clk -> wb_clk --
	//
	// uhh_report is a ONE-CYCLE pulse in usb_hid_host's own usbclk
	// domain (12 MHz, see sysctl.v's clk12mhz). This block runs on
	// wb_clk_i, which is sys_clk -- 48 MHz. Four times faster.
	//
	// So a single report holds uhh_report high across FOUR wb_clk
	// edges, and sampling it with a plain `if (uhh_report)` set
	// mouse_move on every one of them: the same delta was added to
	// the cursor position four times over. One count of physical
	// mouse movement moved the pointer four or five pixels, which is
	// far too coarse to land on an 8x8 titlebar icon -- and it looked
	// like a sensitivity problem rather than the missing clock-domain
	// crossing it actually was.
	//
	// Two flops to resolve metastability, a third to detect the
	// rising edge of the settled signal. mouse_move is now exactly
	// one wb_clk cycle per report, whatever the clock ratio -- so
	// this stays correct if either clock changes, which the old code
	// silently did not.
	reg report_s0, report_s1, report_s2;

	// The deltas are only valid WHILE uhh_report is high:
	// usb_hid_host clears mouse_dx/dy on the same usbclk edge that
	// drops the pulse. The synchroniser above costs 2-3 wb_clk cycles
	// of the 4-cycle window, so sampling them at the edge itself
	// leaves almost no margin. Capturing them continuously, one cycle
	// behind, means the value used at the edge was latched safely
	// inside the window rather than at its very end.
	reg signed [7:0] dx_cap;
	reg signed [7:0] dy_cap;

	wire report_edge = report_s1 && !report_s2;

	// -- pointer acceleration --
	//
	// With the multiply-by-four bug gone, the cursor tracks the mouse
	// 1:1 -- one HID count, one pixel. That is as precise as this
	// hardware can be, and on a high-DPI mouse it is also four times
	// slower than before, which makes crossing the screen a chore.
	//
	// So: small movements stay 1:1 (the whole point -- fine
	// positioning is what this is for), and movements above a
	// threshold are doubled. Aiming at an icon produces small deltas
	// and gets full precision; sweeping across the screen produces
	// large ones and gets there twice as fast. Same idea as every
	// desktop pointer curve, kept to a single threshold and a shift
	// because this is a handful of LUTs in the middle of a cursor
	// path, not a place for a multiplier.
	//
	// Both knobs live at the top of this file -- see USB_HID_ACCEL
	// there for how to tune or disable this.

	wire signed [11:0] dx_s = { {4{mouse_dx[7]}}, mouse_dx };
	wire signed [11:0] dy_s = { {4{mouse_dy[7]}}, mouse_dy };

`ifdef USB_HID_ACCEL
	wire dx_fast = (mouse_dx > $signed(`USB_HID_ACCEL_THRESHOLD)) ||
		(mouse_dx < -$signed(`USB_HID_ACCEL_THRESHOLD));
	wire dy_fast = (mouse_dy > $signed(`USB_HID_ACCEL_THRESHOLD)) ||
		(mouse_dy < -$signed(`USB_HID_ACCEL_THRESHOLD));

	wire signed [11:0] dx_move = dx_fast ? (dx_s <<< 1) : dx_s;
	wire signed [11:0] dy_move = dy_fast ? (dy_s <<< 1) : dy_s;
`else
	wire signed [11:0] dx_move = dx_s;
	wire signed [11:0] dy_move = dy_s;
`endif

	// compute the candidate next position with extra headroom bits so
	// overshoot past [0,639]/[0,479] can be detected and saturated
	// instead of wrapping in curs_x/curs_y's own 10-bit unsigned width.
	// mouse_dx/mouse_dy are real HID deltas (commonly more than +-1),
	// so a single update can jump straight past a boundary -- checking
	// "curs_x == 0/639" before applying the delta (as this used to)
	// only catches the case where the *current* value already happens
	// to sit exactly on the boundary, not the general overshoot case.
	wire signed [11:0] curs_x_sum = $signed({2'b00, curs_x}) + dx_move;
	wire signed [11:0] curs_y_sum = $signed({2'b00, curs_y}) + dy_move;

	always @(posedge wb_clk_i) begin

		wb_ack_o <= 0;
		mouse_move <= 0;

		report_s0 <= uhh_report;
		report_s1 <= report_s0;
		report_s2 <= report_s1;

		dx_cap <= uhh_mouse_dx;
		dy_cap <= uhh_mouse_dy;

		if (report_edge) begin
			mouse_dx <= dx_cap;
			mouse_dy <= dy_cap;
			mouse_move <= 1;
		end

		if (mouse_move) begin

			if (curs_x_sum < 0) curs_x <= 0;
			else if (curs_x_sum > 639) curs_x <= 639;
			else curs_x <= curs_x_sum[9:0];

			if (curs_y_sum < 0) curs_y <= 0;
			else if (curs_y_sum > 479) curs_y <= 479;
			else curs_y <= curs_y_sum[9:0];

		end

		if (wb_cyc_i && wb_stb_i && !wb_we_i) begin

			if (wb_adr_i[2:0] == 3'h00) wb_dat_o <=
				{ uhh_report, 5'b00000, uhh_usb_type, 16'b0, uhh_key_modifiers };

			if (wb_adr_i[2:0] == 3'h01) wb_dat_o <=
				{ uhh_key1, uhh_key2, uhh_key3, uhh_key4 };

			if (wb_adr_i[2:0] == 3'h02) wb_dat_o <=
				{ 8'h00, uhh_mouse_btn, mouse_dy, mouse_dx };

			if (wb_adr_i[2:0] == 3'h03) wb_dat_o <=
				{ 11'd0, uhh_mouse_btn, curs_y, curs_x };

			wb_ack_o <= 1;

		end

	end

	usb_hid_host usb_hid_host_i (
		.usbclk(usb_clk), .usbrst_n(~wb_rst_i),
		.usb_dm(usb_dm), .usb_dp(usb_dp),
		.typ(uhh_usb_type), .report(uhh_report),
		.key_modifiers(uhh_key_modifiers),
		.key1(uhh_key1), .key2(uhh_key2), .key3(uhh_key3), .key4(uhh_key4),
		.mouse_btn(uhh_mouse_btn),
		.mouse_dx(uhh_mouse_dx), .mouse_dy(uhh_mouse_dy),
	);

`endif

endmodule
