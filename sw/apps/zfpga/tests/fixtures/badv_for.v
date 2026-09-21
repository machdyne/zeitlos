// expect: 'for' is not supported yet
module top(input [3:0] a, output reg y);
    always @(*) for (i = 0; i < 4; i = i + 1) y = a[i];
endmodule
