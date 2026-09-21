// expect: for loop variable 'i' must be declared integer
module top(input [3:0] a, output reg y);
    always @(*) begin y = 0; for (i = 0; i < 4; i = i + 1) y = y ^ a[i]; end
endmodule
