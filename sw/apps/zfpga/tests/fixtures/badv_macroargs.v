// expect: macros with arguments are not supported
`define TWICE(x) ((x) + (x))
module top(input [3:0] a, output [3:0] y);
    assign y = a;
endmodule
