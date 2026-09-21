// expect: block RAM inference is not supported yet
module top(input clk, input [9:0] a, input [7:0] d, output [7:0] q);
    reg [7:0] m [0:1023];
    always @(posedge clk) m[a] <= d;
    assign q = m[a];
endmodule
