// The Phase 1 test design: a 24-bit counter, CLK_48 to LED_B.
// Top bit high for 2^23 cycles (0.17 s) then low as long: ~3 blinks a second.
// Built on the host for now; zfpga synth (Phase 6) is meant to take it.
module top(input CLK_48, output LED_B);
	reg [23:0] ctr;
	always @(posedge CLK_48) ctr <= ctr + 1;
	assign LED_B = ctr[23];
endmodule
