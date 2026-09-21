// expect: tristate is not supported yet
module top(input a, input en, output y);
    assign y = en ? a : 1'bz;
endmodule
