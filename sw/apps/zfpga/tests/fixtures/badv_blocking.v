// expect: use = in always @(*)
module top(input a, output reg q);
    always @(*) q <= a;
endmodule
