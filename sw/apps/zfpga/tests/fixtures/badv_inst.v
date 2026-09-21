// expect: module sub is not defined
module top(input a, output y);
    sub u(.a(a), .y(y));
endmodule
