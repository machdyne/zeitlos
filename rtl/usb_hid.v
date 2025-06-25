/*
 * Zeitlos SOC 
 a Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * USB HID wishbone interface
 *
 */

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
);

`ifdef USB_HID

	wire uhh_report;
	wire uhh_dr;

	wire [1:0] uhh_usb_type;
	wire [7:0] uhh_key_modifiers, uhh_key1, uhh_key2, uhh_key3, uhh_key4;
	wire [7:0] uhh_mouse_btn;
	wire signed [7:0] uhh_mouse_dx, uhh_mouse_dy;

	assign int_o = uhh_report;

	reg [9:0] curs_x;
	reg [9:0] curs_y;

	reg signed [7:0] mouse_dx;
	reg signed [7:0] mouse_dy;

	reg mouse_move;

	always @(posedge wb_clk_i) begin

		wb_ack_o <= 0;
		mouse_move <= 0;

		if (uhh_report) begin
			mouse_dx <= uhh_mouse_dx;
			mouse_dy <= uhh_mouse_dy;
			mouse_move <= 1;
		end

		if (mouse_move) begin

			if (curs_x == 1023 && mouse_dx > 0) curs_x = 1023;
			else if (curs_x == 0 && mouse_dx < 0) curs_x = 0;
			else curs_x <= curs_x + mouse_dx;

			if (curs_y == 767 && mouse_dy > 0) curs_y = 767;
			else if (curs_y == 0 && mouse_dy < 0) curs_y = 0;
			else curs_y <= curs_y + mouse_dy;

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
