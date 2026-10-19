/*
my_hdmi_device 

Copyright (C) 2021  Hirosh Dabui <hirosh@dabui.de>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
/*
 * Zeitlos: with ISLANDS = 1 (`GPU_DDMI_AUDIO, docs/ddmi_audio.md) the
 * periods between the pixels also carry data islands and the video
 * preamble and guard band, from rtl/gpu/ddmi_island.v, whenever
 * `islands_en` is high. With it low -- or ISLANDS = 0, where it is
 * forced low and yosys removes the scheduler -- the output is the DVI
 * this module always produced, character for character
 * (rtl/gpu/bench/ddmi_dvi_golden.sha256).
 */
module gpu_ddmi (
           input pclk,
           input tmds_clk, /* 5 times faster of pclk */

           // gpu_video.v's counters and the frame-adopted enable
           input [10:0] hc,
           input [10:0] vc,
           input islands_en,

           // the sound half (ddmi_island.v): settings in this clock
           // domain, frames written on aud_clk
           input sound_en,
           input [3:0] fs_code,
           input [19:0] acr_n,
           input [19:0] acr_cts,
           input aud_clk,
           input aud_we,
           input [31:0] aud_wdata,

           input [7:0] in_vga_red,
           input [7:0] in_vga_green,
           input [7:0] in_vga_blue,

           input in_vga_blank,
           input in_vga_vsync,
           input in_vga_hsync,

           output [1:0] out_tmds_red,
           output [1:0] out_tmds_green,
           output [1:0] out_tmds_blue,
           output [1:0] out_tmds_clk
       );

parameter DDR_ENABLED = 1;
parameter ISLANDS = 0;
localparam OUT_TMDS_MSB = DDR_ENABLED ? 1 : 0;

/* data islands: per channel MODE, TERC4 nibble, and CTL bits */
wire [1:0] m0, m1, m2;
wire [3:0] t0, t1, t2;
wire [1:0] ctl1, ctl2;

ddmi_island island_i (
    .pclk(pclk),
    .en(islands_en && (ISLANDS != 0)),
    .hc(hc), .vc(vc),
    .hsync(in_vga_hsync), .vsync(in_vga_vsync),
    .sound_en(sound_en && (ISLANDS != 0)),
    .fs_code(fs_code), .acr_n(acr_n), .acr_cts(acr_cts),
    .aud_clk(aud_clk), .aud_we(aud_we && (ISLANDS != 0)), .aud_wdata(aud_wdata),
    .mode0(m0), .t0(t0),
    .mode1(m1), .t1(t1), .ctl1(ctl1),
    .mode2(m2), .t2(t2), .ctl2(ctl2)
);

/* channel 0 is blue and carries the syncs; 1 is green, 2 is red */
wire [9:0] tmds_red;
wire [9:0] tmds_green;
wire [9:0] tmds_blue;
tmds_encoder #(.CH(2)) tmsds_encoder_i0(pclk, ~in_vga_blank, in_vga_red,   ctl2[1],      ctl2[0],      m2, t2, tmds_red);
tmds_encoder #(.CH(1)) tmsds_encoder_i1(pclk, ~in_vga_blank, in_vga_green, ctl1[1],      ctl1[0],      m1, t1, tmds_green);
tmds_encoder #(.CH(0)) tmsds_encoder_i2(pclk, ~in_vga_blank, in_vga_blue,  in_vga_vsync, in_vga_hsync, m0, t0, tmds_blue);

/**/
reg       tmds_shift_load   = 0;
reg [3:0] tmds_modulo       = 0;
reg [9:0] tmds_shift_red    = 0;
reg [9:0] tmds_shift_green  = 0;
reg [9:0] tmds_shift_blue   = 0;
reg [9:0] tmds_shift_clk    = 0;

wire [9:0] tmds_pixel_clk = 10'b00000_11111;
/* ddr 5 shifts a 2 bits, sdr 10 shifts a 1 bit */

wire max_shifts_reached = tmds_modulo == (DDR_ENABLED ? 4 : 9);
always @(posedge tmds_clk) tmds_modulo      <= max_shifts_reached ? 0 : tmds_modulo + 1;
always @(posedge tmds_clk) tmds_shift_load  <= max_shifts_reached;

always @(posedge tmds_clk) begin
    tmds_shift_red    <= tmds_shift_load ? tmds_red       : {DDR_ENABLED ? 2'b00 : 1'b0, tmds_shift_red   [9: DDR_ENABLED ? 2 : 1]};
    tmds_shift_green  <= tmds_shift_load ? tmds_green     : {DDR_ENABLED ? 2'b00 : 1'b0, tmds_shift_green [9: DDR_ENABLED ? 2 : 1]};
    tmds_shift_blue   <= tmds_shift_load ? tmds_blue      : {DDR_ENABLED ? 2'b00 : 1'b0, tmds_shift_blue  [9: DDR_ENABLED ? 2 : 1]};
    tmds_shift_clk    <= tmds_shift_load ? tmds_pixel_clk : {DDR_ENABLED ? 2'b00 : 1'b0, tmds_shift_clk   [9: DDR_ENABLED ? 2 : 1]};
end

assign out_tmds_clk   = tmds_shift_clk    [ OUT_TMDS_MSB : 0 ];
assign out_tmds_red   = tmds_shift_red    [ OUT_TMDS_MSB : 0 ];
assign out_tmds_green = tmds_shift_green  [ OUT_TMDS_MSB : 0 ];
assign out_tmds_blue  = tmds_shift_blue   [ OUT_TMDS_MSB : 0 ];

endmodule

module SB_LVCMOS(input DP, input DN, input clk_x5, input [1:0] tmds_signal);
defparam tmds_p.PIN_TYPE = 6'b010000;
defparam tmds_p.IO_STANDARD = "SB_LVCMOS";
SB_IO tmds_p (
          .PACKAGE_PIN (DP),
          .CLOCK_ENABLE (1'b1),
          .OUTPUT_CLK (clk_x5),
          .OUTPUT_ENABLE (1'b1),
          .D_OUT_0 (tmds_signal[1]),
          .D_OUT_1 (tmds_signal[0])
      );

defparam tmds_n.PIN_TYPE = 6'b010000;
defparam tmds_n.IO_STANDARD = "SB_LVCMOS";
SB_IO tmds_n (
          .PACKAGE_PIN (DN),
          .CLOCK_ENABLE (1'b1),
          .OUTPUT_CLK (clk_x5),
          .OUTPUT_ENABLE (1'b1),
          .D_OUT_0 (~tmds_signal[1]),
          .D_OUT_1 (~tmds_signal[0])
      );
endmodule
