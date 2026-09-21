// expect: module instances are not supported yet
module top(input a, output y);
    sub u(.a(a), .y(y));
endmodule
