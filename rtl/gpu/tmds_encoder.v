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
 * tmds_encode implementation (c) 2020 Hirosh Dabui <hirosh@dabui.de>
 * based on Digital Visual Interface Revision 1.0, Page 29
 */
/*
 * Zeitlos: DDMI audio adds three more kinds of character for the
 * periods between the pixels (docs/ddmi_audio.md). `MODE` picks them
 * whenever DE is low:
 *
 *   0  control token from C1/C0, exactly as before
 *   1  TERC4 data-island character, from T
 *   2  video guard band (CH 0 and 2: 1011001100, CH 1: 0100110011)
 *   3  data-island guard band (CH 1 and 2: 0100110011; CH 0 is a
 *      TERC4 character, so the island scheduler sends it as MODE 1)
 *
 * gpu_ddmi.v ties MODE to 0 in a build without `GPU_DDMI_AUDIO, and
 * then this is the encoder it always was -- not merely equivalent:
 * the MODE != 0 arms are constant-false and yosys removes them, and
 * rtl/gpu/bench/ddmi_dvi_golden.sha256 is the check. None of the new
 * characters is DE, so each resets the running disparity like a
 * control token does.
 */
module tmds_encoder(
           input clk,
           input DE,
           input [7:0]D,
           input C1,
           input C0,
           input [1:0] MODE,
           input [3:0] T,
           output reg[9:0] q_out = 0
       );
parameter LEGACY_DVI_CONTROL_LUT = 0;
// which TMDS channel this is (0 = blue, carries the syncs), for the
// guard-band characters
parameter CH = 0;

function [9:0] terc4;
    input [3:0] t;
    begin
        case (t)
            4'h0: terc4 = 10'b1010011100;
            4'h1: terc4 = 10'b1001100011;
            4'h2: terc4 = 10'b1011100100;
            4'h3: terc4 = 10'b1011100010;
            4'h4: terc4 = 10'b0101110001;
            4'h5: terc4 = 10'b0100011110;
            4'h6: terc4 = 10'b0110001110;
            4'h7: terc4 = 10'b0100111100;
            4'h8: terc4 = 10'b1011001100;
            4'h9: terc4 = 10'b0100111001;
            4'hA: terc4 = 10'b0110011100;
            4'hB: terc4 = 10'b1011000110;
            4'hC: terc4 = 10'b1010001110;
            4'hD: terc4 = 10'b1001110001;
            4'hE: terc4 = 10'b0101100011;
            default: terc4 = 10'b1011000011;
        endcase
    end
endfunction

localparam [9:0] VIDEO_GB = (CH == 1) ? 10'b0100110011 : 10'b1011001100;
localparam [9:0] DATA_GB  = 10'b0100110011;

function [3:0] N0;
    input [7:0] d;
    integer i;
    begin
        N0 = 0;
        for (i = 0; i < 8; i = i +1)
            N0 = N0 + !d[i];
    end
endfunction

function [3:0] N1;
    input [7:0] d;
    integer i;
    begin
        N1 = 0;
        for (i = 0; i < 8; i = i +1)
            N1 = N1 + d[i];
    end
endfunction

reg signed [7:0] cnt_prev = 0;
reg signed [7:0] cnt = 0;

reg [8:0] q_m;

always @(*) begin

    if ( (N1(D) > 4) | (N1(D) == 4) & (D[0] == 0) ) begin

        q_m[0] =           D[0];
        q_m[1] = q_m[0] ~^ D[1];
        q_m[2] = q_m[1] ~^ D[2];
        q_m[3] = q_m[2] ~^ D[3];
        q_m[4] = q_m[3] ~^ D[4];
        q_m[5] = q_m[4] ~^ D[5];
        q_m[6] = q_m[5] ~^ D[6];
        q_m[7] = q_m[6] ~^ D[7];
        q_m[8] = 1'b0;

    end else begin

        q_m[0] =          D[0];
        q_m[1] = q_m[0] ^ D[1];
        q_m[2] = q_m[1] ^ D[2];
        q_m[3] = q_m[2] ^ D[3];
        q_m[4] = q_m[3] ^ D[4];
        q_m[5] = q_m[4] ^ D[5];
        q_m[6] = q_m[5] ^ D[6];
        q_m[7] = q_m[6] ^ D[7];
        q_m[8] = 1'b1;

    end /* (N1(D) > 4) | (N1(D) == 4) & (D[0] == 0) */

end

always @(posedge clk) begin

    if (DE) begin

        if ((cnt_prev == 0) | (N1(q_m[7:0]) == N0(q_m[7:0]))) begin

            q_out[9]   <= ~q_m[8];
            q_out[8]   <=  q_m[8];
            q_out[7:0] <= q_m[8] ? q_m[7:0] : ~q_m[7:0];

            if (q_m[8] == 0) begin
                cnt = cnt_prev + (N0(q_m[7:0]) - N1(q_m[7:0]));
            end else begin
                cnt = cnt_prev + (N1(q_m[7:0]) - N0(q_m[7:0]));
            end /*q_m[8] == 0*/

        end else begin

            if ( (cnt_prev > 0 & (N1(q_m[7:0]) > N0(q_m[7:0]))) |
                    (cnt_prev < 0 & (N0(q_m[7:0]) > N1(q_m[7:0]))) ) begin
                q_out[9] <= 1;
                q_out[8] <= q_m[8];
                q_out[7:0] <= ~q_m[7:0];
                cnt = cnt_prev + 2*q_m[8] + (N0(q_m[7:0]) - N1(q_m[7:0]));
            end else begin
                q_out[9] <= 0;
                q_out[8] <= q_m[8];
                q_out[7:0] <= q_m[7:0];
                cnt = cnt_prev - {~q_m[8], 1'b0} + (N1(q_m[7:0]) - N0(q_m[7:0]));
            end /*
                (cnt_prev > 0 & N1(q_m[7:0]) > N0(q_m[7:0]))) |
                (cnt_prev < 0 & N0(q_m[7:0]) > N1(q_m[7:0])))
              */

        end /* ((cnt_prev == 0) | (N1(q_m[7:0]) == N0(q_m[7:0]))) */

    end else begin
        /* !DE */
        cnt = 0;
        if (MODE == 2'd1)
            q_out <= terc4(T);
        else if (MODE == 2'd2)
            q_out <= VIDEO_GB;
        else if (MODE == 2'd3)
            q_out <= DATA_GB;
        else
        /* hsync -> c0 | vsync -> c1 */
        case ({C1, C0})
`ifdef LEGACY_DVI_CONTROL_LUT
            /* dvi control data lut */
            2'b00: q_out <= 10'b00101_01011;
            2'b01: q_out <= 10'b11010_10100;
            2'b10: q_out <= 10'b00101_01010;
            2'b11: q_out <= 10'b11010_10101;

`else
            /* hdmi control data period */
            2'b00: q_out <= 10'b1101010100;
            2'b01: q_out <= 10'b0010101011;
            2'b10: q_out <= 10'b0101010100;
            2'b11: q_out <= 10'b1010101011;
`endif
        endcase

    end /* DE */

    cnt_prev <= cnt;

end

endmodule
