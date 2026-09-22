// An audio test for boards with a PT8211 DAC (Mozart ML1): a sine-wave
// arpeggio -- A4, C#5, E5, A5, a fifth of a second each, round and
// round -- on both channels, at about half volume; and after three
// seconds, back to Zeitlos by itself.
//
//   zfpga build /fpga/examples/tone.v -b mozart_ml1
//   zfpga run tone.bit
//
// For a board with no LED to blink.
//
// -- returning to Zeitlos --
//
// After RETURN_CYCLES clocks (3 s at 48 MHz; 0 plays forever) the
// design pulls PROGRAMN low, and the FPGA reloads from this bitstream's
// boot address. It has none -- it is packed without -a -- so that is
// flash address 0: the DFU bootloader, then Zeitlos, or Zeitlos itself
// on a board flashed without one. The board's pin constraints make
// PROGRAMN open drain, so "1" here leaves the pin released rather than
// driving it high against a reset button (docs/zboot.md).
//
// The PT8211 framing is rtl/audio_out.v's, which is known to work on
// this board: BCK is a 64-slot counter's bit 0 and WS its bit 5, 16
// bits per channel, MSB first from the WS edge, DIN changing on BCK's
// falling edge. BCK's half-period is 17 cycles of the 48 MHz clock, so
// the sample rate is 48e6 / (64 * 17) = 44117.6 Hz. The same sample
// goes to both halves of the frame, so which one is left does not
// matter here.
//
// The tone is direct digital synthesis: a 32-bit phase accumulator
// advanced once per sample by a note's increment (f * 2^32 / 44117.6),
// whose top 8 bits index a sine built from a 64-entry quarter-wave
// table -- plain logic, since zfpga synth does not infer block RAM.

module top(input CLK_48, output AUD_BCK, output AUD_WS, output AUD_DIN, output PROGRAMN);

	// samples per note: 0.2 s. A parameter so a test bench can shorten it.
	parameter NOTE_SAMPLES = 8824;

	// clocks until returning to Zeitlos: 3 s at 48 MHz. 0 never returns.
	parameter RETURN_CYCLES = 144000000;

	// -- a quarter of a sine, 64 entries, amplitude 15000 of 32767 --
	// entry i is 15000 * sin((i + 0.5) * pi / 128); the half-step offset
	// makes the four quarters mirror exactly
	function [13:0] qsine;
		input [5:0] i;
		case (i)
			6'd0: qsine = 184;     6'd1: qsine = 552;     6'd2: qsine = 920;     6'd3: qsine = 1287;
			6'd4: qsine = 1653;    6'd5: qsine = 2019;    6'd6: qsine = 2383;    6'd7: qsine = 2746;
			6'd8: qsine = 3107;    6'd9: qsine = 3466;    6'd10: qsine = 3823;   6'd11: qsine = 4178;
			6'd12: qsine = 4530;   6'd13: qsine = 4880;   6'd14: qsine = 5226;   6'd15: qsine = 5570;
			6'd16: qsine = 5910;   6'd17: qsine = 6246;   6'd18: qsine = 6579;   6'd19: qsine = 6908;
			6'd20: qsine = 7233;   6'd21: qsine = 7553;   6'd22: qsine = 7869;   6'd23: qsine = 8180;
			6'd24: qsine = 8486;   6'd25: qsine = 8787;   6'd26: qsine = 9083;   6'd27: qsine = 9373;
			6'd28: qsine = 9657;   6'd29: qsine = 9936;   6'd30: qsine = 10209;  6'd31: qsine = 10476;
			6'd32: qsine = 10736;  6'd33: qsine = 10990;  6'd34: qsine = 11237;  6'd35: qsine = 11478;
			6'd36: qsine = 11711;  6'd37: qsine = 11938;  6'd38: qsine = 12157;  6'd39: qsine = 12369;
			6'd40: qsine = 12573;  6'd41: qsine = 12770;  6'd42: qsine = 12960;  6'd43: qsine = 13141;
			6'd44: qsine = 13315;  6'd45: qsine = 13480;  6'd46: qsine = 13638;  6'd47: qsine = 13787;
			6'd48: qsine = 13928;  6'd49: qsine = 14060;  6'd50: qsine = 14184;  6'd51: qsine = 14300;
			6'd52: qsine = 14406;  6'd53: qsine = 14505;  6'd54: qsine = 14594;  6'd55: qsine = 14675;
			6'd56: qsine = 14747;  6'd57: qsine = 14810;  6'd58: qsine = 14864;  6'd59: qsine = 14909;
			6'd60: qsine = 14945;  6'd61: qsine = 14972;  6'd62: qsine = 14990;  default: qsine = 14999;
		endcase
	endfunction

	// the whole wave from the quarter: mirror the 2nd and 4th quarters,
	// negate the second half
	function [15:0] sine;
		input [7:0] p;
		sine = p[7] ? 16'd0 - {2'b00, qsine(p[6] ? ~p[5:0] : p[5:0])}
		            : {2'b00, qsine(p[6] ? ~p[5:0] : p[5:0])};
	endfunction

	// the arpeggio: phase increments for A4, C#5, E5, A5 at 44117.6 Hz
	function [31:0] note_inc;
		input [1:0] n;
		case (n)
			2'd0: note_inc = 32'd42835140;     // A4   440.000 Hz
			2'd1: note_inc = 32'd53968870;     // C#5  554.365 Hz
			2'd2: note_inc = 32'd64180183;     // E5   659.255 Hz
			default: note_inc = 32'd85670281; // A5   880.000 Hz
		endcase
	endfunction

	// -- the PT8211 serialiser: rtl/audio_out.v's timing --
	reg [4:0] div = 0;           // 17 clocks per half-BCK
	reg [5:0] slot = 0;          // 64 half-BCK slots per frame
	reg [15:0] sr = 0;           // the word going out, MSB first
	reg [15:0] sample = 0;       // the next frame's sample

	assign AUD_BCK = slot[0];
	assign AUD_WS = slot[5];
	assign AUD_DIN = sr[15];

	// -- the tone --
	reg [31:0] acc = 0;          // phase accumulator
	reg [1:0] note = 0;          // which note of the four
	reg [13:0] count = 0;        // samples into this note

	// -- returning: count to RETURN_CYCLES, then pull PROGRAMN low --
	reg [31:0] life = 0;
	reg prog_n = 1;              // 1 is "released": the pad is open drain

	assign PROGRAMN = prog_n;

	always @(posedge CLK_48) begin
		if (RETURN_CYCLES != 0) begin
			if (life == RETURN_CYCLES - 1)
				prog_n <= 0;
			else
				life <= life + 1;
		end
	end

	always @(posedge CLK_48) begin
		if (div != 0) begin
			div <= div - 1;
		end else begin
			div <= 16;
			slot <= slot + 1;

			// DIN moves on BCK's falling edge (slot odd -> even); a new
			// word starts with each half-frame
			if (slot[0]) begin
				if (slot[4:0] == 5'd31)
					sr <= sample;
				else
					sr <= {sr[14:0], 1'b0};
			end

			// once a frame, a slot before it wraps (as audio_out.v):
			// the next sample, and the next note when this one is done
			if (slot == 6'd62) begin
				sample <= sine(acc[31:24]);
				acc <= acc + note_inc(note);
				if (count == NOTE_SAMPLES - 1) begin
					count <= 0;
					note <= note + 1;
				end else begin
					count <= count + 1;
				end
			end
		end
	end

endmodule
