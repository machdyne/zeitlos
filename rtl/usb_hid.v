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

module usb_hid_wb #(
	// -- pointer sensitivity --
	//
	// Divides the pointer delta by 2^SENS_SHIFT. 0 is the historical
	// behaviour: one HID count, one pixel. That assumes a mouse whose
	// counts per centimetre are in the same range as the screen's
	// pixels, which a modern one is not -- at 1600 CPI a centimetre is
	// about 630 counts and the screen is 640 wide, so a nudge throws
	// the pointer across the display. 2 (a quarter) suits such a mouse
	// on 640x480. The remainder is carried rather than discarded, so a
	// slow drag still advances a pixel at a time instead of rounding
	// away to nothing. Set per board from rtl/boards.vh's
	// USB_HID_SENS_SHIFT.
	parameter SENS_SHIFT = 0
)
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

	// -- gamepad --
	//
	// usb_hid_host.v has decoded these since the day it was vendored
	// in -- it detects typ==3 and handles several different pad
	// report layouts (see its own gamepad section). Nothing in this
	// file connected them, so the information was being produced and
	// thrown away. This is that gap closed; there is no new protocol
	// handling here and no new state machine.
	wire uhh_game_l, uhh_game_r, uhh_game_u, uhh_game_d;
	wire uhh_game_a, uhh_game_b, uhh_game_x, uhh_game_y;
	wire uhh_game_sel, uhh_game_sta;

	// LATCHED into the wishbone domain rather than read straight
	// through, and that is not optional. These ten bits live in
	// usb_hid_host's 12MHz usbclk domain; a wishbone read that muxed
	// them directly would be sampling ten independent signals with no
	// synchroniser at all, so a read landing while the pad state
	// changes can return a mixture of the old and new values.
	//
	// For a d-pad that mixture is not a harmless glitch. Left and
	// right are separate bits, so a torn read during a left-to-right
	// flick can report BOTH pressed or NEITHER -- and a game that
	// resolves "both" as "stand still" gets a character that
	// occasionally refuses to move for one frame, which is exactly
	// the kind of bug nobody can reproduce on demand.
	//
	// report_edge already exists here for the mouse deltas and solves
	// precisely the same problem (see its own comment above): it is
	// one wb_clk-wide pulse per report, correctly synchronised. The
	// pad state is valid at that moment for the same reason the
	// deltas are, so it is captured there and held until the next
	// report. Software then reads a coherent snapshot of one report,
	// which is what a pad state IS -- never a blend of two.
	reg [9:0] game_state;

	// -- typ, synchronised, and why that matters here --
	//
	// uhh_usb_type lives in the 12MHz usbclk domain. Register 0 has
	// always read it straight through, and that is left exactly as it
	// was -- it is a status field software polls, a torn read of it
	// resolves itself on the next poll, and changing register 0 now
	// would be a gratuitous risk to code that already works.
	//
	// The gamepad path needs better than that, for two reasons.
	//
	// First, coherence: register 4 reports the pad state and the
	// device type together precisely so one read answers "is there a
	// pad, and what is it doing" without a second read that could
	// land the far side of a hotplug. That is only true if both come
	// from the same clock domain.
	//
	// Second, and this is the one that actually bites: HOT UNPLUG.
	// usb_hid_host stops issuing reports the moment a device goes
	// away, so report_edge stops firing and game_state would simply
	// freeze at whatever was last held. Pull the cable mid-jump and
	// the machine believes RIGHT is held down forever, with no event
	// coming that would ever correct it. Clearing the state whenever
	// the port is not currently a gamepad is what makes unplugging
	// safe, and it needs a stable typ in this domain to test against.
	//
	// usb_hid_host clears typ to 0 on disconnect (see its own
	// `~connected & connected_r` line), so this covers unplug, and it
	// equally covers swapping a pad for a keyboard on the same port.
	reg [1:0] typ_s0, typ_s1, typ_s2;
	wire typ_changed = (typ_s1 != typ_s2);

	// Fired on a report OR on a device type change.
	//
	// The report pulse alone was enough while every device on a port
	// stayed put: something arrives, software reads it. Hot swapping
	// breaks that, and asymmetrically -- PLUGGING a device in
	// eventually produces reports, so it announces itself, but
	// UNPLUGGING one produces silence. The last thing the ISR ever
	// saw was a report with keys held, and no further event ever
	// arrives to say otherwise, so sw/os/hid.c's per-port key state
	// keeps those keys held forever. Yanking a keyboard mid-keypress
	// left the machine believing that key was still down.
	//
	// So the type change is an interrupt in its own right. typ goes to
	// 0 on disconnect (usb_hid_host clears it), which gives the ISR
	// the edge it needs to flush that port's held keys as releases and
	// start clean. It equally covers swapping one device for another
	// on the same port, where the outgoing device's state is just as
	// stale.
	//
	// Safe to OR these together because rtl/sysctl.v marks both HID
	// interrupts LATCHED_IRQ: a one-cycle pulse from either source is
	// captured in hardware and stays visible until the ISR runs. Two
	// sources cannot lose each other's edge, and a spurious extra
	// interrupt costs one pass through an ISR that re-reads level
	// state anyway.
	//
	// uhh_report is a usbclk pulse and typ_changed is a wb_clk pulse.
	// Mixing domains into one interrupt line is fine here for exactly
	// the same reason: what reaches the CPU is a latch, not this wire.
	assign int_o = uhh_report || typ_changed;

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
	// sensitivity divide, with the remainder carried in sub_x/sub_y so
	// that slow movement is not rounded away (see SENS_SHIFT above)
	reg signed [11:0] sub_x;
	reg signed [11:0] sub_y;
	wire signed [11:0] acc_x = sub_x + dx_move;
	wire signed [11:0] acc_y = sub_y + dy_move;
	wire signed [11:0] step_x = acc_x >>> SENS_SHIFT;
	wire signed [11:0] step_y = acc_y >>> SENS_SHIFT;
	wire signed [11:0] rem_x = acc_x - (step_x <<< SENS_SHIFT);
	wire signed [11:0] rem_y = acc_y - (step_y <<< SENS_SHIFT);

	wire signed [11:0] curs_x_sum = $signed({2'b00, curs_x}) + step_x;
	wire signed [11:0] curs_y_sum = $signed({2'b00, curs_y}) + step_y;

	always @(posedge wb_clk_i) begin

		wb_ack_o <= 0;
		mouse_move <= 0;

		report_s0 <= uhh_report;
		report_s1 <= report_s0;
		report_s2 <= report_s1;

		typ_s0 <= uhh_usb_type;
		typ_s1 <= typ_s0;
		typ_s2 <= typ_s1;

		dx_cap <= uhh_mouse_dx;
		dy_cap <= uhh_mouse_dy;

		// Pad state. Cleared whenever this port is not currently a
		// gamepad, which is what makes hot unplug safe -- see the
		// typ_s* comment above. The clear is tested FIRST so it wins
		// over a capture: on the report that accompanies a device
		// change, "no pad here" is the newer and more useful truth
		// than whatever buttons the outgoing device last reported.
		if (typ_s2 != 2'd3)
			game_state <= 10'd0;
		else if (report_edge)
			game_state <= { uhh_game_sta, uhh_game_sel,
			                uhh_game_y, uhh_game_x,
			                uhh_game_b, uhh_game_a,
			                uhh_game_d, uhh_game_u,
			                uhh_game_r, uhh_game_l };

		if (report_edge) begin
			mouse_dx <= dx_cap;
			mouse_dy <= dy_cap;
			mouse_move <= 1;
		end

		if (mouse_move) begin

			sub_x <= rem_x;
			sub_y <= rem_y;

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

			// Gamepad state. Bit order is
			// start/select/y/x/b/a/down/up/right/left, LSB first --
			// KEEP IN SYNC with sw/common/zpad.h's Z_PAD_* constants,
			// the same hand-maintained split as this file's HID usage
			// codes and zkbd.h (there is no shared source between the
			// Verilog and C halves; edit both together).
			//
			// The upper bits carry typ -- the SYNCHRONISED copy, not
			// the raw one register 0 reports. A pad state of all-zero
			// is indistinguishable from no pad at all (both mean
			// "nothing is pressed"), and telling them apart with a
			// second register read leaves a window where a hotplug
			// lands between the two. Reporting both from one cycle of
			// one clock domain makes a single read self-describing.
			if (wb_adr_i[2:0] == 3'h04) wb_dat_o <=
				{ 20'd0, typ_s2, game_state };

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
		.game_l(uhh_game_l), .game_r(uhh_game_r),
		.game_u(uhh_game_u), .game_d(uhh_game_d),
		.game_a(uhh_game_a), .game_b(uhh_game_b),
		.game_x(uhh_game_x), .game_y(uhh_game_y),
		.game_sel(uhh_game_sel), .game_sta(uhh_game_sta),
	);

`endif

endmodule
