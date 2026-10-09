// yosys's generic tri-state buffer, for simulating its netlists:
// write_verilog emits $_TBUF_ cells that no vendor library defines.
module \$_TBUF_ (input A, input E, output Y);
	assign Y = E ? A : 1'bz;
endmodule
