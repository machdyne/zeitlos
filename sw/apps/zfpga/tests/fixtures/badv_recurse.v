// expect: calls itself: recursion is not supported
module top(input [3:0] a, output [3:0] y);
    function [3:0] f(input [3:0] x);
        f = x == 0 ? 0 : f(x - 1);
    endfunction
    assign y = f(a);
endmodule
