// zfpga synth: indexed part-selects, and a trailing comma in the ports.
module top(input clk, input [31:0] w, input [4:0] s, input [1:0] b, input [7:0] d,
           output [7:0] byte_c, output [7:0] byte_v, output [3:0] nib_dn, output [5:0] lo6,
           output reg [31:0] r,
          );
    assign byte_c = w[8 +: 8];          // constant start
    assign byte_v = w[b*8 +: 8];        // variable start
    assign nib_dn = w[{1'b0, s[3:0]} + 5'd3 -: 4];   // variable, downwards, in range
    assign lo6 = w[5 -: 6];
    always @(posedge clk)
        r[b*8 +: 8] <= d;               // a variable-start write
endmodule
