// zfpga synth: module hierarchy, parameters, the preprocessor.
`include "hierdefs.vh"

module counter #(parameter WIDTH = 4, parameter [7:0] STEP = 8'd1) (
    input clk, input rst, input en,
    output reg [WIDTH-1:0] count, output wrap);
    localparam integer TOP = (1 << WIDTH) - 1;
    assign wrap = en && (count == TOP[WIDTH-1:0]);
    always @(posedge clk)
        if (rst) count <= 0;
        else if (en) count <= count + STEP;
endmodule

module mixer #(parameter W = 4) (input [W-1:0] a, input [W-1:0] b, output [W-1:0] y);
`ifdef ZF_USE_XOR
    assign y = a ^ b;
`else
    assign y = a | b;
`endif
endmodule

module pair #(parameter N = 3) (input clk, input rst, input en, output [N-1:0] lo, output hi_wrap,
                                output [$clog2(N * 5):0] sz);
    wire w0, unused;
    counter #(.WIDTH(N)) c0 (.clk(clk), .rst(rst), .en(en), .count(lo), .wrap(w0));
    counter #(N + 2, 8'd3) c1 (clk, rst, w0, , hi_wrap);
    assign sz = N * 5;
endmodule

module top(input clk, input rst, input en, input [3:0] a, input [3:0] b,
           output [3:0] mx, output [2:0] lo, output hw, output [4:0] sz, output [`ZF_WIDE-1:0] wide);
    mixer #(4) m (.a(a), .b(b), .y(mx));
    pair #(.N(3)) p (.clk(clk), .rst(rst), .en(en), .lo(lo), .hi_wrap(hw), .sz(sz));
    counter #(.WIDTH(`ZF_WIDE), .STEP(8'd7)) big (.clk(clk), .rst(rst), .en(1'b1), .count(wide), .wrap());
endmodule
