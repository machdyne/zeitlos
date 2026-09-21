// zfpga pnr test design: IO on all four sides of a 25F, two IO
// standards, a real tristate, per-pin options, and flip-flops with an
// asynchronous reset and a clock enable.
module top(input clk, input rst, input en, input a, input b,
    output [3:0] q, output y, output od, inout io, input oe, output z);
    reg [3:0] c;
    always @(posedge clk or posedge rst)
        if (rst) c <= 4'd0;
        else if (en) c <= c + 4'd1;
    assign q = c;
    assign y = a ^ b;
    assign od = ~(a & b);
    assign io = oe ? c[0] : 1'bz;
    assign z = io | b;
endmodule
