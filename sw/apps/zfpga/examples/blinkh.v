// A blinky from two modules and an `include, for the Lakritz:
//
//   zfpga build /fpga/examples/blinkh.v -b lakritz
//
// The top bit of a 25-bit counter at 48MHz is high for 2^24 cycles,
// 0.35 s, then low as long: half the rate of blink.bit.
`include "blinkh.vh"

module counter #(parameter WIDTH = 8) (input clk, output reg [WIDTH-1:0] q);
    always @(posedge clk) q <= q + 1'b1;
endmodule

module top(input CLK_48, output LED_B);
    wire [`BLINK_BITS-1:0] c;
    counter #(.WIDTH(`BLINK_BITS)) u_ctr (.clk(CLK_48), .q(c));
    assign LED_B = c[`BLINK_BITS-1];
endmodule
