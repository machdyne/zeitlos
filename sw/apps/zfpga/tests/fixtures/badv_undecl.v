// expect: 'nope' is not declared
module top(input a, output y);
    assign y = a & nope;
endmodule
