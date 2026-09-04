/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * GPIO -- general purpose I/O ports, plus the board LEDs.
 *
 * This is rtl/debug.v grown up. That block owned the whole 0xE nibble
 * and used two words of it for LED and LEDS; both keep their exact
 * addresses here, because sw/bios/bios.c writes them before anything
 * else in the system is alive and moving them would mean the first
 * thing to break on a bad build is the only thing that can tell you a
 * build is bad.
 *
 * -- Why this block is UNCONDITIONAL --
 *
 * rtl/debug.v sat behind `ifdef DEBUG. That define was universal, so
 * the branch where it is absent was never taken -- but if it ever had
 * been, nothing would have decoded 0xE, and an address nothing decodes
 * gets no ack on this bus and stalls the CPU forever (see rtl/csrs.v's
 * own header). The very first LED write in the BIOS would have hung
 * the machine.
 *
 * That was survivable while the block held two LED bits nobody probes.
 * It is not survivable now that software reads MAGIC here to ask
 * whether GPIO exists: a probe that hangs is worse than no probe. So
 * this block joins rtl/csrs.v and rtl/socctl.v as one that is always
 * instantiated, on every board, and `DEBUG has been retired from
 * rtl/boards.vh. What VARIES is how many ports have pins -- see
 * `GPIO_PORT0..3 there and the NPORTS parameter below.
 *
 * -- Register map --
 *
 * Word-addressed: wb_adr_i here is rtl/sysctl.v's wbm_adr_sel_word,
 * matching every other simple slave in this codebase.
 *
 *   0xe000_0000 region -- board-level registers:
 *
 *     word 0  LED     bit 0: LED_B (the board LED). Resets to 1.
 *     word 1  LEDS    bits 7:0: DBG[7:0], the `LED_DEBUG LED bar.
 *                     Present on every build whether or not a board
 *                     has pins for it -- if it doesn't, the bits are
 *                     simply not connected to anything.
 *     word 2  MAGIC   fixed 32'h5A47_5049 ("ZGPI"). Same purpose as
 *                     rtl/csrs.v's own MAGIC: an unmapped read does
 *                     not fault on this bus, so a known constant is
 *                     the only way software can tell "this block is
 *                     here" from "this is whatever the bus resolved
 *                     to". Reads back 0 or 1 on a pre-GPIO bitstream,
 *                     because debug.v returned {31'b0, led} for every
 *                     address it did not recognise -- neither of which
 *                     is the magic, which is the point.
 *     word 3  CONFIG  { 16'h4750, 12'b0, nports[3:0] } -- how many
 *                     GPIO ports this bitstream actually has pins
 *                     for. The signature is not decoration: a pre-GPIO
 *                     bitstream answers this address with {31'b0, led}
 *                     too, and "0 ports" and "1 port, LED off" would
 *                     otherwise be the same word. Same trick, and the
 *                     same reason for it, as rtl/socctl.v's VIDEO_SIG.
 *     words 4-7       reserved, read 0.
 *
 *   0xe000_1000 region -- the ports, eight words (32 bytes) each:
 *
 *     port N base = 0xe000_1000 + N * 0x20
 *
 *     +0x00  DIR     r/w  1 = drive the pin, 0 = input / high-Z
 *     +0x04  OUT     r/w  value driven where DIR is 1
 *     +0x08  IN      r    pin state, two-flop synchronised
 *     +0x0c  OUTSET  w    write 1 -> set that OUT bit
 *     +0x10  OUTCLR  w    write 1 -> clear that OUT bit
 *     +0x14  DIRSET  w    write 1 -> drive that pin
 *     +0x18  DIRCLR  w    write 1 -> float that pin
 *     +0x1c           reserved
 *
 * Only bits 7:0 of any port register mean anything; the rest read 0.
 *
 * -- Why the ports start at 0x1000 and not right after CONFIG --
 *
 * Room, and memory. The board-level registers are a handful of words
 * that will grow slowly and unpredictably; the port array is a regular
 * structure whose address arithmetic somebody will eventually do in
 * their head at a `repl` prompt. Putting the array at a round offset
 * means "port 2 DIR" is 0xe000_1040 rather than something that has to
 * be looked up, and it leaves 4KB for whatever else ends up wanting to
 * live down at the bottom of this nibble.
 *
 * -- Why OUTSET/OUTCLR/DIRSET/DIRCLR exist --
 *
 * They are not convenience. Bit-banging I2C means moving one pin while
 * the other holds its state, and with only DIR and OUT that is a bus
 * read, an ALU op and a bus write -- non-atomic against the KTIMER
 * interrupt, and three times the bus traffic on the hot path of every
 * bit of every byte.
 *
 * More specifically: I2C is open-drain, which this block has no
 * hardware for and does not need. Park the OUT bit at 0 once, then a
 * write to DIRCLR floats the pin (the external pull-up makes it a 1)
 * and a write to DIRSET drives it low. One store per edge, and it is
 * structurally impossible to accidentally drive a 1 into a bus another
 * device is pulling down -- which is the failure that ends with a hot
 * part rather than a wrong reading.
 *
 * The four aliases READ BACK the register they modify (OUTSET and
 * OUTCLR read OUT, DIRSET and DIRCLR read DIR) rather than reading 0
 * or floating. A write-only address that reads as something arbitrary
 * is a trap for anyone doing a register dump, and there is a correct
 * answer available here for free.
 *
 * -- Decode, and the aliasing it accepts --
 *
 * Timing on this design is tight (see the Makefile's own notes on the
 * critical path), so this decodes the SMALLEST number of address bits
 * that distinguishes the registers rather than the largest number that
 * would be tidy:
 *
 *   wb_adr_i[10]   selects the port region over the board region
 *   wb_adr_i[5:3]  selects the port
 *   wb_adr_i[2:0]  selects the register
 *
 * Everything above bit 10 is ignored. So the whole map repeats every
 * 8KB across the 256MB nibble -- LED is also at 0xe000_0020, port 0
 * DIR is also at 0xe000_3000, and so on. That is deliberate: the
 * alternative is a comparator against the top fifteen bits of the word
 * address feeding the same cycle that already carries the ack mux, and
 * this nibble is 256MB of address space allocated to one small
 * peripheral -- there is nothing else in it to collide with. Software
 * uses the addresses above and nothing detects that it did.
 *
 * ACK AND DATA ARE BOTH REGISTERED, unlike rtl/debug.v, whose ack was
 * combinational (`assign wb_ack_o = wb_cyc_i && wb_stb_i`). rtl/csrs.v
 * made the same change for the same reason and its own comment
 * explains it: one extra cycle on a register access is free, and
 * putting an address decode plus this block's position in the ack mux
 * into a single 48MHz cycle was not.
 *
 * -- NPORTS, and why unbuilt ports cost nothing --
 *
 * The register file below is written for eight ports because the map
 * reserves eight, but every write is gated on `port_ok` (pidx <
 * NPORTS), which is a comparison against a parameter and therefore
 * constant-folded per index. On a build with NPORTS=1, ports 1-7 are
 * never written, so their flops have no driver but reset, synthesis
 * proves them constant zero, and they disappear along with the read
 * mux entries that feed from them. The same applies to the input
 * synchroniser: rtl/sysctl.v ties the unused lanes of gpio_in_i to
 * zero, so those flops fold away too.
 *
 * The practical upshot is that NPORTS is the only thing that decides
 * the cost, and a board that builds one port pays for one port. That
 * matters on Obst, which is an ECP5 12F.
 */

module gpio_wb #(
	// How many ports have pins. Set by rtl/sysctl.v from the
	// `GPIO_PORT0..3 defines in rtl/boards.vh -- gpio_wb gets a
	// number, the same arrangement rtl/socctl.v's GAME_AVAIL uses.
	//
	// Defaults to 0: a gpio_wb instantiated without being told
	// anything is the LED block and nothing more, which is the safe
	// answer and is exactly what rtl/debug.v was.
	parameter NPORTS = 0
)
(
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output wb_ack_o,
	input wb_cyc_i,

	// The board LED (LED_B) and the `LED_DEBUG LED bar (DBG[7:0]).
	// Unchanged from rtl/debug.v, including `led` resetting to 1 --
	// the BIOS turns it off once it is running, so a board sitting
	// with it lit has not reached that point.
	output reg led,
	output reg [7:0] leds,

	// Port state, flat rather than an array: an array in a port list
	// is SystemVerilog, and this tree is plain Verilog. Port N owns
	// bits [N*8 +: 8] of each. rtl/sysctl.v builds the tri-state
	// buffers from dir/out and drives in from the pads.
	output [63:0] gpio_dir_o,
	output [63:0] gpio_out_o,
	input [63:0] gpio_in_i
);

	localparam MAGIC = 32'h5A47_5049;	// "ZGPI"

	// Top half of CONFIG -- "GP". See this file's header on why a
	// second signature is needed when MAGIC already exists.
	localparam CONFIG_SIG = 16'h4750;

	reg [7:0] dir [0:7];
	reg [7:0] out [0:7];

	// Two-flop synchroniser on the pads. Not optional and not a
	// nicety: these pins are driven by whatever is plugged into the
	// PMOD, which has no relationship to sys_clk at all, so a raw pad
	// feeding the read mux is a metastability path straight into the
	// CPU's data bus.
	//
	// Two flops, not three, and no glitch filter: the cost of a
	// missed short pulse here is a sample of an input, and software
	// polling a pin over the wishbone bus cannot see anything shorter
	// than a few hundred nanoseconds anyway.
	reg [63:0] in_meta;
	reg [63:0] in_sync;

	reg [31:0] dat_r;
	reg ack_r;

	assign wb_dat_o = dat_r;
	assign wb_ack_o = ack_r;

	assign gpio_dir_o = { dir[7], dir[6], dir[5], dir[4],
	                      dir[3], dir[2], dir[1], dir[0] };
	assign gpio_out_o = { out[7], out[6], out[5], out[4],
	                      out[3], out[2], out[1], out[0] };

	// See the header on what is and is not decoded here.
	wire is_port = wb_adr_i[10];
	wire [2:0] pidx = wb_adr_i[5:3];
	wire [2:0] preg = wb_adr_i[2:0];

	// Constant-folded per index against the parameter -- this is what
	// makes an unbuilt port cost nothing. See the header.
	wire port_ok = (pidx < NPORTS);

	// One access, one ack, same shape as rtl/csrs.v and
	// rtl/socctl.v.
	wire sel = wb_cyc_i && wb_stb_i && !ack_r;

	// Byte lanes are honoured to the extent they mean anything here:
	// every register in this block is eight bits wide and lives in
	// lane 0, so lane 0 is the only one with anything to write. A
	// byte store to lane 3 of DIR does nothing, which is the honest
	// behaviour -- there is nothing there.
	wire wr = sel && wb_we_i && wb_sel_i[0];

	// Selected port's current value, named once rather than repeated
	// in six places below. The synthesis result is one 8-wide 8:1 mux
	// per register, shared between the read path and the read-modify
	// -write of the SET/CLR aliases.
	wire [7:0] dir_cur = dir[pidx];
	wire [7:0] out_cur = out[pidx];

	integer i;

	always @(posedge wb_clk_i) begin

		// Free-running, outside the bus logic entirely: the pins are
		// sampled every cycle whether or not anyone is reading, so a
		// read never returns a value that is one synchroniser stage
		// stale.
		in_meta <= gpio_in_i;
		in_sync <= in_meta;

		if (wb_rst_i) begin

			// LED on at reset, unchanged from rtl/debug.v. A board
			// showing this after boot has not reached the BIOS.
			led <= 1'b1;
			leds <= 8'h00;

			// EVERY PIN FLOATS AT RESET. This is the only defensible
			// power-on state for a connector going to somebody else's
			// hardware: the FPGA has no idea what is on the other end
			// of a PMOD, and driving a pin that turns out to be an
			// output on the module means two drivers fighting from
			// the moment the bitstream loads until software gets
			// around to saying otherwise.
			//
			// OUT resets to 0 as well, which is also what makes the
			// open-drain idiom safe from the first instruction: a
			// caller that only ever touches DIRSET/DIRCLR never has
			// to remember to clear OUT first, because it was already
			// clear.
			for (i = 0; i < 8; i = i + 1) begin
				dir[i] <= 8'h00;
				out[i] <= 8'h00;
			end

			ack_r <= 1'b0;
			dat_r <= 32'h0000_0000;

		end else begin

			ack_r <= sel;

			if (wr) begin

				if (!is_port) begin
					if (preg == 3'd0) led <= wb_dat_i[0];
					if (preg == 3'd1) leds <= wb_dat_i[7:0];
					// words 2-7 are read-only or reserved. Writes are
					// silently ignored rather than faulting, matching
					// this bus's behaviour everywhere else.
				end
				else if (port_ok) begin
					case (preg)
						3'd0: dir[pidx] <= wb_dat_i[7:0];
						3'd1: out[pidx] <= wb_dat_i[7:0];
						3'd3: out[pidx] <= out_cur | wb_dat_i[7:0];
						3'd4: out[pidx] <= out_cur & ~wb_dat_i[7:0];
						3'd5: dir[pidx] <= dir_cur | wb_dat_i[7:0];
						3'd6: dir[pidx] <= dir_cur & ~wb_dat_i[7:0];
						default: ;
					endcase
				end
				// A write to a port this bitstream does not have is
				// dropped. Not an error -- there is no way to report
				// one on this bus -- but CONFIG is how software finds
				// out, before it writes rather than after.

			end

			if (sel && !wb_we_i) begin

				if (!is_port) begin
					case (preg)
						3'd0: dat_r <= { 31'b0, led };
						3'd1: dat_r <= { 24'b0, leds };
						3'd2: dat_r <= MAGIC;
						3'd3: dat_r <= { CONFIG_SIG, 12'b0, NPORTS[3:0] };
						default: dat_r <= 32'h0000_0000;
					endcase
				end
				else if (port_ok) begin
					case (preg)
						// SET/CLR read back the register they modify
						// -- see the header on why they do not read 0.
						3'd0, 3'd5, 3'd6: dat_r <= { 24'b0, dir_cur };
						3'd1, 3'd3, 3'd4: dat_r <= { 24'b0, out_cur };
						3'd2: dat_r <= { 24'b0, in_sync[pidx*8 +: 8] };
						default: dat_r <= 32'h0000_0000;
					endcase
				end
				else begin
					// A port that is not built reads as zero rather
					// than as the last thing this register happened
					// to hold. CONFIG is the register that says which
					// ports those are.
					dat_r <= 32'h0000_0000;
				end

			end

		end

	end

endmodule
