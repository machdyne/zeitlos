// The screen modes' serialiser alone (rtl/gpu/gpu_vchar.v), with its
// output cells and a board's HDMI pins, under the names the full design
// gives it: `make serialiser_timing BOARD=...` synthesises this, pins
// it with tools/ddmi/pin_serialiser.py and places and routes it, in
// about ten seconds. It answers what a full build takes ten minutes
// to: does nextpnr accept this board's serialiser -- every pinned site
// of the 378 MHz one, or the edge clock of the ODDRX2F one
// (DDMI_ODDRX2, rtl/boards.vh) -- and what do its paths come to
// (docs/screen_modes.md, "The serialiser's place"). The inputs are a
// shift register's bits, so that synthesis keeps the encoders.
`include "boards.vh"

module vwrap(input pclk, input cclk, input bclk, output [3:0] dvi_p);
	reg ptog = 0;
	reg [63:0] l = 64'h1234_5678_9abc_def1;
	always @(posedge pclk) begin
		ptog <= !ptog;
		l <= { l[62:0], l[63] ^ l[62] ^ l[60] ^ l[59] };
	end
`ifdef DDMI_ODDRX2
	localparam SER_X2 = 1;
`else
	localparam SER_X2 = 0;
`endif
	wire eclk, sclk;
	wire [3:0] tr, tg, tb, tc;
	wire [8:0] vga; wire hs, vs;
	gpu_vchar #(.WIDE(1), .ISLANDS(0), .X2(SER_X2)) gpu_vchar_i (
		.cclk(cclk), .sclk(sclk), .bclk(bclk),
		.ptog(ptog), .s_new(1'b1), .s_mode(2'd3),
		.s_rgb0(l[23:0]), .s_rgb1(l[47:24]), .s_vga0(l[56:48]), .s_vga1(l[63:55]),
		.s_hs(l[1]), .s_vs(l[2]), .s_de(l[3]), .s_hc(l[14:4]), .s_vc(l[25:15]),
		.islands_en(1'b0), .sound_en(1'b0), .fs_code(4'd0), .acr_n(20'd0), .acr_cts(20'd0),
		.aud_clk(1'b0), .aud_we(1'b0), .aud_wdata(32'd0),
		.vga(vga), .vga_hs(hs), .vga_vs(vs),
		.tmds_r(tr), .tmds_g(tg), .tmds_b(tb), .tmds_c(tc));
`ifdef DDMI_ODDRX2
	ECLKSYNCB es (.ECLKI(bclk), .STOP(1'b0), .ECLKO(eclk));
	CLKDIVF #(.DIV("2.0")) cd (.CLKI(eclk), .RST(1'b0), .ALIGNWD(1'b0), .CDIVX(sclk));
	ODDRX2F c (.D0(tc[0]), .D1(tc[1]), .D2(tc[2]), .D3(tc[3]), .ECLK(eclk), .SCLK(sclk), .RST(1'b0), .Q(dvi_p[3]));
	ODDRX2F r (.D0(tr[0]), .D1(tr[1]), .D2(tr[2]), .D3(tr[3]), .ECLK(eclk), .SCLK(sclk), .RST(1'b0), .Q(dvi_p[2]));
	ODDRX2F g (.D0(tg[0]), .D1(tg[1]), .D2(tg[2]), .D3(tg[3]), .ECLK(eclk), .SCLK(sclk), .RST(1'b0), .Q(dvi_p[1]));
	ODDRX2F b (.D0(tb[0]), .D1(tb[1]), .D2(tb[2]), .D3(tb[3]), .ECLK(eclk), .SCLK(sclk), .RST(1'b0), .Q(dvi_p[0]));
`else
	assign eclk = 1'b0;
	assign sclk = 1'b0;
	ODDRX1F c (.D0(tc[0]), .D1(tc[1]), .Q(dvi_p[3]), .SCLK(bclk), .RST(1'b0));
	ODDRX1F r (.D0(tr[0]), .D1(tr[1]), .Q(dvi_p[2]), .SCLK(bclk), .RST(1'b0));
	ODDRX1F g (.D0(tg[0]), .D1(tg[1]), .Q(dvi_p[1]), .SCLK(bclk), .RST(1'b0));
	ODDRX1F b (.D0(tb[0]), .D1(tb[1]), .Q(dvi_p[0]), .SCLK(bclk), .RST(1'b0));
`endif
endmodule
module sysctl(input CLK_48, output DDMI_CK_P, output DDMI_D2_P, output DDMI_D1_P, output DDMI_D0_P);
	wire pclk, cclk, bclk, locked;
	pll1 pll (.clkin(CLK_48), .clkout0(bclk), .clkout1(cclk), .clkout2(pclk), .locked(locked));
	vwrap gpu_video_i (.pclk(pclk), .cclk(cclk), .bclk(bclk),
		.dvi_p({ DDMI_CK_P, DDMI_D2_P, DDMI_D1_P, DDMI_D0_P }));
endmodule
