// expect: is outside parameter P
module top(output [3:0] y);
    localparam [7:0] P = 8'd200;
    assign y = P[11:8];
endmodule
