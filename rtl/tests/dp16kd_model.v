/*
 * Zeitlos -- a behavioural DP16KD, for simulating yosys's ECP5 netlists.
 *
 * yosys's own techlibs/lattice/cells_sim_ecp5.v declares DP16KD as a
 * BLACK BOX: its outputs are undriven, so a post-synthesis simulation of
 * anything with a block RAM in it reads X from the RAM. That is easy to
 * miss -- a test that compares with `!=` inside an `if` treats X as
 * "equal" and passes -- and it is how the first netlist run of the
 * spiflash.v page buffer change looked broken when it was not.
 *
 * This models the ONE configuration yosys's brams_map_16kd.v produces
 * for a 36-bit pseudo-dual-port memory ($__PDPW16KD_): port A writes,
 * port B reads, both NOREG.
 *
 *   - 512 words of 36 bits: address bits 13:5 on each port.
 *   - Port A writes on CLKA when CEA and WEA, one 9-bit lane per
 *     ADA[3:0] bit (the byte enables), data {DIB, DIA}.
 *   - Port B reads on CLKB when CEB: {DOB, DOA} is the word as it was
 *     before any write on the same edge (the silicon leaves a same-
 *     address collision undefined; yosys adds bypass logic when the RTL
 *     needs one, so the model must not hide its absence).
 *   - RSTB clears the read data (RESETMODE either way).
 *
 * Anything else -- other widths, REGMODE "OUTREG", true dual port --
 * stops the simulation rather than quietly modelling it wrong. Used by
 * `make -C rtl/tests spiflash_netlist`.
 */

`timescale 1ns / 1ps

module DP16KD (
	input ADA13, ADA12, ADA11, ADA10, ADA9, ADA8, ADA7, ADA6, ADA5, ADA4, ADA3, ADA2, ADA1, ADA0,
	input DIA17, DIA16, DIA15, DIA14, DIA13, DIA12, DIA11, DIA10, DIA9, DIA8, DIA7, DIA6, DIA5, DIA4, DIA3, DIA2, DIA1, DIA0,
	input CLKA, CEA, OCEA, WEA, CSA2, CSA1, CSA0, RSTA,
	input ADB13, ADB12, ADB11, ADB10, ADB9, ADB8, ADB7, ADB6, ADB5, ADB4, ADB3, ADB2, ADB1, ADB0,
	input DIB17, DIB16, DIB15, DIB14, DIB13, DIB12, DIB11, DIB10, DIB9, DIB8, DIB7, DIB6, DIB5, DIB4, DIB3, DIB2, DIB1, DIB0,
	input CLKB, CEB, OCEB, WEB, CSB2, CSB1, CSB0, RSTB,
	output DOA17, DOA16, DOA15, DOA14, DOA13, DOA12, DOA11, DOA10, DOA9, DOA8, DOA7, DOA6, DOA5, DOA4, DOA3, DOA2, DOA1, DOA0,
	output DOB17, DOB16, DOB15, DOB14, DOB13, DOB12, DOB11, DOB10, DOB9, DOB8, DOB7, DOB6, DOB5, DOB4, DOB3, DOB2, DOB1, DOB0
);
	parameter DATA_WIDTH_A = 18;
	parameter DATA_WIDTH_B = 18;
	parameter REGMODE_A = "NOREG";
	parameter REGMODE_B = "NOREG";
	parameter RESETMODE = "SYNC";
	parameter ASYNC_RESET_RELEASE = "SYNC";
	parameter CSDECODE_A = "0b000";
	parameter CSDECODE_B = "0b000";
	parameter WRITEMODE_A = "NORMAL";
	parameter WRITEMODE_B = "NORMAL";
	parameter CLKAMUX = "CLKA";
	parameter CLKBMUX = "CLKB";
	parameter GSR = "ENABLED";
	parameter INIT_DATA = "STATIC";
	// INITVAL_00 .. _3F are accepted and ignored: the memories this is
	// used for have no initial contents.
	parameter INITVAL_00 = 320'h0;
	parameter INITVAL_01 = 320'h0;
	parameter INITVAL_02 = 320'h0;
	parameter INITVAL_03 = 320'h0;
	parameter INITVAL_04 = 320'h0;
	parameter INITVAL_05 = 320'h0;
	parameter INITVAL_06 = 320'h0;
	parameter INITVAL_07 = 320'h0;
	parameter INITVAL_08 = 320'h0;
	parameter INITVAL_09 = 320'h0;
	parameter INITVAL_0A = 320'h0;
	parameter INITVAL_0B = 320'h0;
	parameter INITVAL_0C = 320'h0;
	parameter INITVAL_0D = 320'h0;
	parameter INITVAL_0E = 320'h0;
	parameter INITVAL_0F = 320'h0;
	parameter INITVAL_10 = 320'h0;
	parameter INITVAL_11 = 320'h0;
	parameter INITVAL_12 = 320'h0;
	parameter INITVAL_13 = 320'h0;
	parameter INITVAL_14 = 320'h0;
	parameter INITVAL_15 = 320'h0;
	parameter INITVAL_16 = 320'h0;
	parameter INITVAL_17 = 320'h0;
	parameter INITVAL_18 = 320'h0;
	parameter INITVAL_19 = 320'h0;
	parameter INITVAL_1A = 320'h0;
	parameter INITVAL_1B = 320'h0;
	parameter INITVAL_1C = 320'h0;
	parameter INITVAL_1D = 320'h0;
	parameter INITVAL_1E = 320'h0;
	parameter INITVAL_1F = 320'h0;
	parameter INITVAL_20 = 320'h0;
	parameter INITVAL_21 = 320'h0;
	parameter INITVAL_22 = 320'h0;
	parameter INITVAL_23 = 320'h0;
	parameter INITVAL_24 = 320'h0;
	parameter INITVAL_25 = 320'h0;
	parameter INITVAL_26 = 320'h0;
	parameter INITVAL_27 = 320'h0;
	parameter INITVAL_28 = 320'h0;
	parameter INITVAL_29 = 320'h0;
	parameter INITVAL_2A = 320'h0;
	parameter INITVAL_2B = 320'h0;
	parameter INITVAL_2C = 320'h0;
	parameter INITVAL_2D = 320'h0;
	parameter INITVAL_2E = 320'h0;
	parameter INITVAL_2F = 320'h0;
	parameter INITVAL_30 = 320'h0;
	parameter INITVAL_31 = 320'h0;
	parameter INITVAL_32 = 320'h0;
	parameter INITVAL_33 = 320'h0;
	parameter INITVAL_34 = 320'h0;
	parameter INITVAL_35 = 320'h0;
	parameter INITVAL_36 = 320'h0;
	parameter INITVAL_37 = 320'h0;
	parameter INITVAL_38 = 320'h0;
	parameter INITVAL_39 = 320'h0;
	parameter INITVAL_3A = 320'h0;
	parameter INITVAL_3B = 320'h0;
	parameter INITVAL_3C = 320'h0;
	parameter INITVAL_3D = 320'h0;
	parameter INITVAL_3E = 320'h0;
	parameter INITVAL_3F = 320'h0;

	initial begin
		if (DATA_WIDTH_A != 36 || DATA_WIDTH_B != 36 ||
		    REGMODE_A != "NOREG" || REGMODE_B != "NOREG" ||
		    CLKAMUX != "CLKA" || CLKBMUX != "CLKB") begin
			$display("dp16kd_model: only the 36-bit pseudo-dual-port NOREG mode is modelled");
			$finish;
		end
	end

	reg [35:0] mem [0:511];
	reg [35:0] q = 36'd0;

	wire [8:0] wa = { ADA13, ADA12, ADA11, ADA10, ADA9, ADA8, ADA7, ADA6, ADA5 };
	wire [8:0] ra = { ADB13, ADB12, ADB11, ADB10, ADB9, ADB8, ADB7, ADB6, ADB5 };
	wire [3:0] be = { ADA3, ADA2, ADA1, ADA0 };
	wire [35:0] di = { DIB17, DIB16, DIB15, DIB14, DIB13, DIB12, DIB11, DIB10, DIB9,
	                   DIB8, DIB7, DIB6, DIB5, DIB4, DIB3, DIB2, DIB1, DIB0,
	                   DIA17, DIA16, DIA15, DIA14, DIA13, DIA12, DIA11, DIA10, DIA9,
	                   DIA8, DIA7, DIA6, DIA5, DIA4, DIA3, DIA2, DIA1, DIA0 };

	always @(posedge CLKA) begin
		if (CEA && WEA) begin
			if (be[0]) mem[wa][8:0]   <= di[8:0];
			if (be[1]) mem[wa][17:9]  <= di[17:9];
			if (be[2]) mem[wa][26:18] <= di[26:18];
			if (be[3]) mem[wa][35:27] <= di[35:27];
		end
	end

	always @(posedge CLKB) begin
		if (RSTB) q <= 36'd0;
		else if (CEB) q <= mem[ra];
	end

	assign { DOB17, DOB16, DOB15, DOB14, DOB13, DOB12, DOB11, DOB10, DOB9,
	         DOB8, DOB7, DOB6, DOB5, DOB4, DOB3, DOB2, DOB1, DOB0,
	         DOA17, DOA16, DOA15, DOA14, DOA13, DOA12, DOA11, DOA10, DOA9,
	         DOA8, DOA7, DOA6, DOA5, DOA4, DOA3, DOA2, DOA1, DOA0 } = q;

endmodule
