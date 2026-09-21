module top(input CLK_48, output LED_B);
	reg [7:0] ctr;
	always @(posedge CLK_48) ctr <= ctr + 1;
	assign LED_B = ctr[3];
endmodule
