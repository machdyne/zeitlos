`timescale 1ns / 1ps

// Checks the four virtual phosphor modes drive the VGA outputs
// correctly, and -- the case that actually matters -- that GPU_PAPER
// stays BLACK during the blanking intervals rather than driving the
// DAC high through sync.

module tb_video_mode;

	reg clk = 0;
	reg pclk = 0;
	reg bclk = 0;
	reg resetn = 0;
	reg [1:0] video_mode = 2'd0;
	reg [31:0] gb_dat_i = 32'hffff_ffff;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p;
	wire [3:0] dac;

	integer errors = 0;

	always #10 clk = ~clk;		// ~50MHz
	always #20 pclk = ~pclk;	// ~25MHz
	always #4  bclk = ~bclk;

	gpu_video #() dut (
		.pixel(1'b0),
		.clk(clk),
		.pclk(pclk),
		.bclk(bclk),
		.resetn(resetn),
		.video_mode(video_mode),
		.red(red), .green(green), .blue(blue),
		.hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p),
		.dac(dac),
		.x(x), .y(y),
		.is_visible(is_visible),
		.gb_adr_o(gb_adr_o),
		.gb_dat_i(gb_dat_i)
	);

	// wait until we are somewhere safely inside the visible area
	task wait_visible;
		begin
			@(posedge pclk);
			while (!(is_visible && x > 100 && x < 500 && y > 100 && y < 300))
				@(posedge pclk);
		end
	endtask

	// wait until we are safely inside blanking
	task wait_blank;
		begin
			@(posedge pclk);
			while (is_visible) @(posedge pclk);
			repeat (4) @(posedge pclk);
		end
	endtask

	task check;
		input [8*24-1:0] label;
		input exp_r, exp_g, exp_b;
		begin
			if (red !== exp_r || green !== exp_g || blue !== exp_b) begin
				$display("FAIL %0s: got rgb=%b%b%b expected %b%b%b",
					label, red, green, blue, exp_r, exp_g, exp_b);
				errors = errors + 1;
			end else begin
				$display("  ok %0s: rgb=%b%b%b", label, red, green, blue);
			end
		end
	endtask

	// a mode change is only adopted at a frame boundary, so give it
	// two full frames to land before sampling
	task set_mode;
		input [1:0] m;
		begin
			video_mode = m;
			repeat (2) begin
				@(posedge vsync);
				@(negedge vsync);
			end
		end
	endtask

	initial begin
		resetn = 0;
		repeat (10) @(posedge pclk);
		resetn = 1;

		// ---- pixels ON (framebuffer all ones) ----
		gb_dat_i = 32'hffff_ffff;

		set_mode(2'd0); wait_visible; check("white  pixel-on", 1,1,1);
		set_mode(2'd1); wait_visible; check("amber  pixel-on", 1,1,0);
		set_mode(2'd2); wait_visible; check("green  pixel-on", 0,1,0);
		set_mode(2'd3); wait_visible; check("paper  pixel-on", 0,0,0);

		// ---- pixels OFF (framebuffer all zeros) ----
		gb_dat_i = 32'h0000_0000;

		set_mode(2'd0); wait_visible; check("white  pixel-off", 0,0,0);
		set_mode(2'd1); wait_visible; check("amber  pixel-off", 0,0,0);
		set_mode(2'd2); wait_visible; check("green  pixel-off", 0,0,0);
		set_mode(2'd3); wait_visible; check("paper  pixel-off", 1,1,1);

		// ---- blanking must be black in EVERY mode ----
		// paper is the one that would break: its inactive state is 1,
		// so an ungated invert drives the DAC high through sync.
		set_mode(2'd0); wait_blank; check("white  blanking", 0,0,0);
		set_mode(2'd1); wait_blank; check("amber  blanking", 0,0,0);
		set_mode(2'd2); wait_blank; check("green  blanking", 0,0,0);
		set_mode(2'd3); wait_blank; check("paper  blanking", 0,0,0);

		gb_dat_i = 32'hffff_ffff;
		set_mode(2'd3); wait_blank; check("paper  blanking (fb set)", 0,0,0);

		$display("");
		if (errors == 0) $display("ALL CHECKS PASSED");
		else $display("%0d CHECK(S) FAILED", errors);
		$finish;
	end

endmodule
