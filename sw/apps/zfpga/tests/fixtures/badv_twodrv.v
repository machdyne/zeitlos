// expect: has more than one driver
module top(input a, input b, output y);
    assign y = a;
    assign y = b;
endmodule
