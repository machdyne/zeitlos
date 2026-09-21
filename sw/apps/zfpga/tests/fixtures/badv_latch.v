// expect: that is a latch
module top(input en, input d, output reg q);
    always @(*) if (en) q = d;
endmodule
