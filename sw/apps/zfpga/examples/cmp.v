// A comparator: the answer is a carry out, fed to logic (feed-out).
module top(input [7:0] a, input [7:0] b, output lt, output ge2);
    assign lt = a < b;
    assign ge2 = (a >= b) & a[0];
endmodule
