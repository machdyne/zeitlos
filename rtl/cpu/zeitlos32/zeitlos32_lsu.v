/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Redistribution and use in source, binary or physical forms, with or
 * without modification, is permitted provided that the following
 * condition is met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * THIS HARDWARE, SOFTWARE, DATA AND/OR DOCUMENTATION ("THE ASSETS") IS
 * PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE ASSETS OR THE USE OR OTHER DEALINGS IN THE ASSETS. USE AT
 * YOUR OWN RISK.
 *
 */

/*
 * zeitlos32_store / zeitlos32_load -- byte-lane steering for stores and
 * extraction for loads, shared by zeitlos32 and zeitlos32p.
 *
 * Both purely combinational. The bus is 32 bits wide and word-aligned;
 * a byte or halfword store puts its data on every lane it could land
 * on and selects the right one with sel, and a load takes the whole
 * word and picks its byte or halfword back out by the address's low
 * two bits. Misalignment is the core's to detect (it raises IRQ 2
 * before anything reaches the bus), so neither module sees it.
 */

module zeitlos32_store (
	input   [1:0] size,		// funct3[1:0]: 0 byte, 1 half, 2 word
	input   [1:0] adr_lo,		// address bits 1:0
	input  [31:0] rs2,
	output reg  [3:0] sel,
	output reg [31:0] dat
);

	always @* begin
		case (size)
			2'b00: begin							// sb
				dat = {4{rs2[7:0]}};
				case (adr_lo)
					2'b00: sel = 4'b0001;
					2'b01: sel = 4'b0010;
					2'b10: sel = 4'b0100;
					default: sel = 4'b1000;
				endcase
			end
			2'b01: begin							// sh
				dat = {2{rs2[15:0]}};
				sel = adr_lo[1] ? 4'b1100 : 4'b0011;
			end
			default: begin							// sw
				dat = rs2;
				sel = 4'b1111;
			end
		endcase
	end

endmodule

module zeitlos32_load (
	input   [2:0] funct3,		// lb lh lw lbu lhu
	input   [1:0] adr_lo,		// address bits 1:0
	input  [31:0] word,		// the whole aligned word from the bus
	output reg [31:0] out
);

	reg [7:0]  byt;
	reg [15:0] half;

	always @* begin
		case (adr_lo)
			2'b00:   byt = word[7:0];
			2'b01:   byt = word[15:8];
			2'b10:   byt = word[23:16];
			default: byt = word[31:24];
		endcase
		half = adr_lo[1] ? word[31:16] : word[15:0];
		case (funct3)
			3'b000:  out = {{24{byt[7]}}, byt};		// lb
			3'b001:  out = {{16{half[15]}}, half};		// lh
			3'b100:  out = {24'd0, byt};			// lbu
			3'b101:  out = {16'd0, half};			// lhu
			default: out = word;				// lw
		endcase
	end

endmodule
