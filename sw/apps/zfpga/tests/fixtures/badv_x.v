// expect: x in a literal is not supported
module top(input a, output [1:0] y);
    assign y = 2'bx1;
endmodule
