/*
 * A behavioural W25Q16 (2 MB SPI NOR), for rtl/tests/tb_spiflash.v.
 *
 * Mode 0: MOSI sampled on SCK rising, MISO driven on SCK falling. The
 * commands rtl/spiflash.v may send, with W25Q16JV semantics -- READ
 * (03), RDSR (05), WREN (06), SE 4 KB (20), PP (02), RDID (9F) -- and
 * erase/program times shortened to keep the simulation quick.
 *
 * And three tripwires the test bench checks at the end:
 *
 *   illegal     any other command at all -- chip erase, status write,
 *               block erase: the controller must never send one
 *   prot_hits   an erase or program aimed below PROTECT_END, whether
 *               or not it would have taken effect: the controller must
 *               refuse those before they reach the pins
 *   while_busy  any command but RDSR while an erase or program is in
 *               progress -- a real chip ignores it or returns garbage,
 *               so the controller must never issue one
 */
`timescale 1ns/1ps
module w25q16_model #(
	parameter [23:0] PROTECT_END = 24'h040000,
	parameter T_SE = 20000,		// ns; the real part is 45 ms typical
	parameter T_PP = 3000		// ns; the real part is 0.4 ms typical
) (
	input cs_n,
	input sck,
	input mosi,
	output reg miso
);
	localparam SIZE = 2 * 1024 * 1024;
	reg [7:0] mem [0:SIZE-1];

	integer illegal = 0, prot_hits = 0, while_busy = 0;
	integer n_read = 0, n_se = 0, n_pp = 0, n_wren = 0, n_rdsr = 0, n_rdid = 0;

	reg wip = 0, wel = 0;
	integer bits;
	reg [7:0] cmd, inbyte;
	reg [23:0] addr;
	reg [7:0] outbyte;
	integer outbit;
	reg outputting;
	reg [7:0] page [0:255];
	integer pp_count, pp_n;
	reg [23:0] pp_addr, se_addr;
	reg [7:0] pp_off;
	integer i;

	initial begin
		miso = 1'b0;
		for (i = 0; i < SIZE; i = i + 1) mem[i] = 8'hFF;
	end

	// a fresh command on every falling chip select
	always @(negedge cs_n) begin
		bits = 0;
		cmd = 0;
		addr = 0;
		outputting = 0;
		pp_count = 0;
	end

	always @(posedge sck) if (!cs_n) begin
		inbyte = { inbyte[6:0], mosi };
		bits = bits + 1;
		if (bits == 8) begin
			cmd = inbyte;
			if (wip && cmd != 8'h05) while_busy = while_busy + 1;
			case (cmd)
				8'h03: n_read = n_read + 1;
				8'h05: begin n_rdsr = n_rdsr + 1; outbyte = { 6'b0, wel, wip }; outputting = 1; outbit = 7; end
				8'h06: ;
				8'h20: ;
				8'h02: ;
				8'h9F: begin n_rdid = n_rdid + 1; outbyte = 8'hEF; outputting = 1; outbit = 7; end
				default: illegal = illegal + 1;
			endcase
		end else if (bits > 8 && bits <= 32 && (cmd == 8'h03 || cmd == 8'h20 || cmd == 8'h02)) begin
			addr = { addr[22:0], mosi };
			if (bits == 32 && cmd == 8'h03) begin
				outbyte = mem[addr[20:0]];
				outputting = 1;
				outbit = 7;
			end
		end else if (bits > 32 && cmd == 8'h02 && bits % 8 == 0) begin
			page[(addr[7:0] + pp_count) & 8'hFF] = inbyte;
			pp_count = pp_count + 1;
		end
	end

	// MISO: the next bit on each falling edge
	always @(negedge sck) if (!cs_n && outputting) begin
		miso = outbyte[outbit];
		if (outbit == 0) begin
			outbit = 7;
			if (cmd == 8'h03) begin addr = addr + 1; outbyte = mem[addr[20:0]]; end
			else if (cmd == 8'h05) outbyte = { 6'b0, wel, wip };
			else if (cmd == 8'h9F) outbyte = (outbyte == 8'hEF) ? 8'h40 : 8'h15;
		end else begin
			outbit = outbit - 1;
		end
	end

	// commands take effect as chip select rises, as on the real part
	always @(posedge cs_n) begin
		if (bits == 8 && cmd == 8'h06) begin
			n_wren = n_wren + 1;
			if (!wip) wel = 1;
		end
		if (bits == 32 && cmd == 8'h20) begin
			n_se = n_se + 1;
			if ({ addr[23:12], 12'h000 } < PROTECT_END) prot_hits = prot_hits + 1;
			if (wel && !wip) begin
				// captured now: the next chip select (a status poll)
				// resets addr before the erase completes
				se_addr = addr;
				wip = 1;
				fork begin
					#(T_SE);
					for (i = 0; i < 4096; i = i + 1) mem[{ se_addr[20:12], 12'h000 } + i] = 8'hFF;
					wip = 0;
					wel = 0;
				end join_none
			end
		end
		if (bits >= 40 && cmd == 8'h02 && bits % 8 == 0) begin
			n_pp = n_pp + 1;
			if (addr < PROTECT_END) prot_hits = prot_hits + 1;
			if (wel && !wip) begin
				pp_addr = addr;
				pp_n = pp_count;
				wip = 1;
				fork begin
					#(T_PP);
					// the byte index as 8 bits: an integer in the
					// concatenation made the index 45 bits wide
					for (i = 0; i < pp_n && i < 256; i = i + 1) begin
						pp_off = pp_addr[7:0] + i[7:0];
						mem[{ pp_addr[20:8], pp_off }] = mem[{ pp_addr[20:8], pp_off }] & page[pp_off];
					end
					wip = 0;
					wel = 0;
				end join_none
			end
		end
	end

endmodule
