// Carry chain fed from logic and feeding logic: the feed-in routes ci
// into the chain, the feed-out brings the final carry back to co.
module top(input [7:0] a, input [7:0] b, input ci, output [7:0] s, output co);
    assign {co, s} = a + b + ci;
endmodule
