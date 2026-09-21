// expect: for loop runs more than
module top(input a, output reg y);
    integer i;
    always @(*) begin y = a; for (i = 0; i >= 0; i = i + 1) y = ~y; end
endmodule
