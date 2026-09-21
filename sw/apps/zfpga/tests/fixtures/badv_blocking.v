// expect: use <= in a clocked always block
module top(input clk, input d, output reg q);
    always @(posedge clk) q = d;
endmodule
