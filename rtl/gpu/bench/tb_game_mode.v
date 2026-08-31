`timescale 1ns / 1ps

// Self-checking testbench for rtl/gpu/gpu_video.v's game mode.
//
// The thing being tested is an ADDRESS TRANSFORM, so this testbench
// checks addresses rather than pixels: for a given viewport origin and
// mode, which framebuffer column and row does each physical pixel
// come from? That is exactly the x/y output pair (which now carry
// framebuffer coordinates -- see gpu_video.v's header), so the whole
// test reduces to sampling x and y at known points in the raster and
// comparing them against an independent model.
//
// Checked here:
//
//   1. desktop mode is still 1:1 -- x runs 0..639 across the visible
//      line, y runs 0..479 down the frame. This is the regression
//      guard: game mode must be invisible when it is off.
//
//   2. the off-by-one fix -- the first visible pixel of a line is
//      column 0 (it used to be column 0 twice, with 639 never
//      appearing at all), and the last visible pixel is column 639.
//
//   3. game mode doubles -- each framebuffer column appears on
//      exactly two consecutive physical pixels, each framebuffer row
//      on exactly two consecutive physical rows, and the viewport is
//      320x240 out of a 640x480 framebuffer.
//
//   4. the viewport origin offsets both axes, and quadrant page
//      origins land where a page-flipping game expects them to.
//
//   5. CLAMP mode limits the origin to (320,240) so the viewport
//      cannot hang off the edge, whatever software writes.
//
//   6. WRAP mode is toroidal -- an origin near the far edge scans off
//      column 639 and continues from column 0, and off row 479 and
//      continues from row 0, with no gap and no repeat.
//
//   7. config is adopted at a FRAME boundary, not mid-frame: a write
//      made in the middle of a frame does not change what the rest of
//      that frame scans out.
//
//   8. the frame counter advances once per frame, and in_vblank is
//      asserted between frames and clear during them.
//
// Note on running this: iverilog rejects the trailing comma in
// gpu_video.v's port list (yosys accepts it, and it is the file's
// existing style), so point it at a patched copy -- same treatment
// rtl/gpu/bench/README.md already documents for glyph.v and vram.v:
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -o /tmp/tb_game.out \
//       rtl/gpu/bench/tb_game_mode.v /tmp/gpu_video_fix.v
//   vvp /tmp/tb_game.out

module tb_game_mode;

	// timings, mirrored from gpu_video.v's own parameter defaults so
	// the model below can predict where in the raster we are
	localparam H_DISP = 640, H_FP = 16, H_PW = 96, H_BP = 48;
	localparam V_DISP = 480, V_FP = 10, V_PW = 2,  V_BP = 33;
	localparam H_START = H_FP + H_PW + H_BP;   // 160
	localparam H_STOP  = H_START + H_DISP;     // 800
	localparam V_START = V_FP + V_PW + V_BP;   // 45
	localparam V_STOP  = V_START + V_DISP;     // 525

	reg clk = 0;
	reg pclk = 0;
	reg bclk = 0;
	reg resetn = 0;

	reg [1:0] video_mode = 2'd0;
	reg [31:0] gb_dat_i = 32'h0;

	reg view_load = 0;
	reg game_en = 0;
	reg game_wrap = 0;
	reg [9:0] view_x = 0;
	reg [9:0] view_y = 0;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p;
	wire [3:0] dac;
	wire [15:0] frame_ctr;
	wire in_vblank;

	integer errors = 0;

	always #10 clk  = ~clk;    // 50MHz-ish; only the refill path uses it
	always #20 pclk = ~pclk;   // 25MHz-ish
	always #4  bclk = ~bclk;

	gpu_video #() dut (
		.pixel(1'b0),
		.clk(clk),
		.pclk(pclk),
		.bclk(bclk),
		.resetn(resetn),
		.video_mode(video_mode),
		.view_load(view_load),
		.game_en(game_en),
		.game_wrap(game_wrap),
		.view_x(view_x),
		.view_y(view_y),
		.frame_ctr(frame_ctr),
		.in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue),
		.hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p),
		.dac(dac),
		.x(x), .y(y),
		.is_visible(is_visible),
		.gb_adr_o(gb_adr_o),
		.gb_dat_i(gb_dat_i)
	);

	task check_eq;
		input [255:0] what;
		input [31:0] got;
		input [31:0] exp;
		begin
			if (got !== exp) begin
				$display("FAIL: %0s: got %0d, expected %0d", what, got, exp);
				errors = errors + 1;
			end
		end
	endtask

	// Write the game-mode payload the way socctl.v does: data and the
	// load toggle change on the same wishbone-domain edge.
	task set_view;
		input en;
		input wrap;
		input [9:0] vx;
		input [9:0] vy;
		begin
			@(posedge clk);
			game_en   <= en;
			game_wrap <= wrap;
			view_x    <= vx;
			view_y    <= vy;
			view_load <= ~view_load;
		end
	endtask

	// Park on the pclk edge at which hc == want_hc and vc == want_vc,
	// so x/y can be sampled at an exactly known point in the raster.
	// Reaches into the DUT's counters deliberately: predicting them
	// from the outside would mean reimplementing the thing under test.
	task goto;
		input [10:0] want_vc;
		input [10:0] want_hc;
		begin
			while (!(dut.vc == want_vc && dut.hc == want_hc))
				@(posedge pclk);
		end
	endtask

	// advance to the start of the next frame, so a config change made
	// now is in force for the whole of the frame that follows
	task next_frame;
		begin
			goto(V_STOP - 1, H_STOP - 1);
			@(posedge pclk);
			@(posedge pclk);
		end
	endtask

	integer i;
	integer seen_lo, seen_hi;
	reg [9:0] prev_x, prev_y;
	reg [15:0] f0, f1;

	initial begin

		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;

		// ---- 1 & 2: desktop mode is 1:1, and starts on column 0 ----

		next_frame();

		goto(V_START, H_START);
		check_eq("desktop: first visible column", x, 0);
		goto(V_START, H_START + 1);
		check_eq("desktop: second visible column", x, 1);
		goto(V_START, H_START + 320);
		check_eq("desktop: mid-line column", x, 320);
		goto(V_START, H_STOP - 1);
		check_eq("desktop: last visible column", x, 639);

		goto(V_START + 1, H_START);
		check_eq("desktop: row 1", y, 1);
		goto(V_START + 100, H_START);
		check_eq("desktop: row 100", y, 100);
		goto(V_STOP - 1, H_START);
		check_eq("desktop: last row", y, 479);

		// ---- 3: game mode doubles both axes ----

		set_view(1, 0, 0, 0);
		next_frame();

		goto(V_START, H_START);
		check_eq("game: first visible column", x, 0);
		goto(V_START, H_START + 1);
		check_eq("game: column repeated once", x, 0);
		goto(V_START, H_START + 2);
		check_eq("game: second column", x, 1);
		goto(V_START, H_START + 3);
		check_eq("game: second column repeated", x, 1);
		goto(V_START, H_STOP - 2);
		check_eq("game: last column", x, 319);
		goto(V_START, H_STOP - 1);
		check_eq("game: last column repeated", x, 319);

		goto(V_START, H_START);
		check_eq("game: first visible row", y, 0);
		goto(V_START + 1, H_START);
		check_eq("game: row repeated once", y, 0);
		goto(V_START + 2, H_START);
		check_eq("game: second row", y, 1);
		goto(V_STOP - 1, H_START);
		check_eq("game: last row", y, 239);

		// every physical column maps into 0..319 exactly twice --
		// checked exhaustively rather than at sampled points, since an
		// off-by-one in the phase would still pass the spot checks
		// above if it only bit at one end of the line
		goto(V_START, H_START);
		seen_lo = 0;
		for (i = 0; i < 640; i = i + 1) begin
			if (x !== (i / 2)) begin
				$display("FAIL: game: physical pixel %0d -> column %0d, expected %0d",
					i, x, i / 2);
				seen_lo = seen_lo + 1;
			end
			@(posedge pclk);
		end
		if (seen_lo != 0) errors = errors + 1;

		// ---- 4: viewport origin, at a quadrant page boundary ----

		set_view(1, 0, 320, 240);
		next_frame();

		goto(V_START, H_START);
		check_eq("page(320,240): first column", x, 320);
		check_eq("page(320,240): first row", y, 240);
		goto(V_START, H_STOP - 1);
		check_eq("page(320,240): last column", x, 639);
		goto(V_STOP - 1, H_START);
		check_eq("page(320,240): last row", y, 479);

		// an origin that is not page-aligned works just as well --
		// nothing in the hardware knows what a page is
		set_view(1, 0, 137, 61);
		next_frame();

		goto(V_START, H_START);
		check_eq("origin(137,61): first column", x, 137);
		check_eq("origin(137,61): first row", y, 61);
		goto(V_START, H_STOP - 1);
		check_eq("origin(137,61): last column", x, 137 + 319);
		goto(V_STOP - 1, H_START);
		check_eq("origin(137,61): last row", y, 61 + 239);

		// ---- 5: clamp mode limits the origin ----

		set_view(1, 0, 639, 479);
		next_frame();

		goto(V_START, H_START);
		check_eq("clamp: x limited to 320", x, 320);
		check_eq("clamp: y limited to 240", y, 240);
		goto(V_START, H_STOP - 1);
		check_eq("clamp: right edge is 639", x, 639);
		goto(V_STOP - 1, H_START);
		check_eq("clamp: bottom edge is 479", y, 479);

		// ---- 6: wrap mode is toroidal ----

		// origin 600 -- 40 columns to the right edge, then wrap
		set_view(1, 1, 600, 0);
		next_frame();

		goto(V_START, H_START);
		check_eq("wrap: origin honoured", x, 600);
		goto(V_START, H_START + 2 * 39);
		check_eq("wrap: last column before edge", x, 639);
		goto(V_START, H_START + 2 * 40);
		check_eq("wrap: wrapped to column 0", x, 0);
		goto(V_START, H_START + 2 * 41);
		check_eq("wrap: continues past the seam", x, 1);
		goto(V_START, H_STOP - 1);
		check_eq("wrap: last visible column", x, 279);

		// vertical wrap: origin 460 -- 20 rows to the bottom, then wrap
		set_view(1, 1, 0, 460);
		next_frame();

		goto(V_START, H_START);
		check_eq("wrap: origin row honoured", y, 460);
		goto(V_START + 2 * 19, H_START);
		check_eq("wrap: last row before edge", y, 479);
		goto(V_START + 2 * 20, H_START);
		check_eq("wrap: wrapped to row 0", y, 0);
		goto(V_STOP - 1, H_START);
		check_eq("wrap: last visible row", y, 219);

		// walk the whole visible line across the seam and confirm
		// every column is visited exactly twice, in order, with the
		// wrap the only discontinuity
		set_view(1, 1, 600, 0);
		next_frame();
		goto(V_START, H_START);
		seen_hi = 0;
		for (i = 0; i < 640; i = i + 1) begin
			if (x !== ((600 + (i / 2)) % 640)) begin
				$display("FAIL: wrap: physical pixel %0d -> column %0d, expected %0d",
					i, x, (600 + (i / 2)) % 640);
				seen_hi = seen_hi + 1;
			end
			@(posedge pclk);
		end
		if (seen_hi != 0) errors = errors + 1;

		// ---- 7: config is adopted at a frame boundary ----

		set_view(1, 0, 0, 0);
		next_frame();
		goto(V_START + 100, H_START);
		check_eq("adopt: before mid-frame write", x, 0);

		// write a wildly different origin in the middle of a frame
		set_view(1, 0, 300, 200);

		// the rest of THIS frame must be unaffected
		goto(V_START + 300, H_START);
		check_eq("adopt: same frame, column unchanged", x, 0);
		check_eq("adopt: same frame, row unchanged", y, 150);

		// and the next frame must pick it up
		next_frame();
		goto(V_START, H_START);
		check_eq("adopt: next frame, new column", x, 300);
		check_eq("adopt: next frame, new row", y, 200);

		// ---- 8: frame counter and vblank ----

		goto(V_START + 10, H_START);
		check_eq("vblank clear during active video", in_vblank, 0);

		f0 = frame_ctr;
		next_frame();
		// vc 0..44 is the vertical blanking interval -- the front
		// porch, sync pulse and back porch all sit at the START of
		// this counter's range, because vc wraps to 0 on the cycle it
		// reaches v_disp_stop - 1 and so never reaches v_disp_stop at
		// all. Parking at V_STOP + 2 to "find vblank" is a loop that
		// never terminates, which is exactly what this line used to
		// do. See gpu_video.v's vblank_p for the same trap stated
		// from the hardware side.
		goto(5, H_START);
		check_eq("vblank set between frames", in_vblank, 1);
		f1 = frame_ctr;
		check_eq("frame counter advanced by one", f1 - f0, 1);

		next_frame();
		goto(V_START + 10, H_START);
		check_eq("frame counter advanced again", frame_ctr - f0, 2);

		if (errors == 0)
			$display("RESULT: PASS");
		else
			$display("RESULT: FAIL (%0d errors)", errors);

		$finish;

	end

endmodule
