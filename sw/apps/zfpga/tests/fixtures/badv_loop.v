// expect: combinational loop
module top(input a, output y);
    wire p, q;
    assign p = q ^ a;
    assign q = p;
    assign y = q;
endmodule
