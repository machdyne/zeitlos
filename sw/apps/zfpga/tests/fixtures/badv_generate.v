// expect: 'genvar' is not supported yet
module top(input [3:0] a, output [3:0] y);
    genvar g;
endmodule
