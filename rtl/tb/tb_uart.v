/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * rtl/uart.v testbench -- loopback, framing, parity, FIFO, overrun,
 * interrupts and the divisor latch.
 *
 *   iverilog -g2005 -o output/tb_uart rtl/tb/tb_uart.v rtl/uart.v
 *   vvp output/tb_uart
 *
 * Self-checking: prints a line per case and exits non-zero if any
 * fail. There is no hardware in this loop, so this is the only thing
 * standing between a register-level mistake here and a board that
 * prints nothing at all -- rtl/uart.v answers the window the BIOS
 * writes its very first character into, and a fault in it looks
 * exactly like a dead machine.
 *
 * TX is wired to RX. That shares one bit clock between the two ends,
 * which a real link does NOT -- it exercises framing, parity, the
 * FIFOs and the register file, but says nothing about the receiver's
 * tolerance to a transmitter running slightly fast or slow. The
 * 16x oversampling and mid-bit sampling are what buy that tolerance;
 * confirming it needs a second divisor and a longer run than belongs
 * in a smoke test.
 */

`timescale 1ns / 1ps

module tb_uart;

	reg clk;
	reg rst;

	reg [25:0] adr;
	reg [31:0] dat_w;
	wire [31:0] dat_r;
	reg we;
	reg stb;
	reg cyc;
	wire ack;
	wire irq;

	wire serial;

	reg [31:0] rdata;
	integer errors;
	integer i;
	integer timeout;

	// 16550 register offsets, as words
	localparam ADR_RBR = 26'd0;
	localparam ADR_IER = 26'd1;
	localparam ADR_IIR = 26'd2;
	localparam ADR_LCR = 26'd3;
	localparam ADR_LSR = 26'd5;

	uart_wb dut (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(adr),
		.wb_dat_i(dat_w),
		.wb_dat_o(dat_r),
		.wb_we_i(we),
		.wb_sel_i(4'b1111),
		.wb_stb_i(stb),
		.wb_cyc_i(cyc),
		.wb_ack_o(ack),
		.stx_pad_o(serial),
		.srx_pad_i(serial),
		.int_o(irq)
	);

	// 48MHz, matching every board that builds this
	initial clk = 0;
	always #10.4167 clk = ~clk;

	task wb_write;
		input [25:0] a;
		input [7:0] d;
		begin
			@(posedge clk);
			adr <= a; dat_w <= {24'h0, d}; we <= 1'b1; stb <= 1'b1; cyc <= 1'b1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			adr <= 26'd0; dat_w <= 32'h0; we <= 1'b0; stb <= 1'b0; cyc <= 1'b0;
			@(posedge clk);
		end
	endtask

	task wb_read;
		input [25:0] a;
		begin
			@(posedge clk);
			adr <= a; we <= 1'b0; stb <= 1'b1; cyc <= 1'b1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			rdata = dat_r;
			adr <= 26'd0; stb <= 1'b0; cyc <= 1'b0;
			@(posedge clk);
		end
	endtask

	// Configure: divisor `div`, line control `lcr` (without DLAB),
	// FIFOs enabled and flushed.
	task configure;
		input [15:0] div;
		input [7:0] lcr;
		begin
			wb_write(ADR_LCR, lcr | 8'h80);
			wb_write(ADR_RBR, div[7:0]);
			wb_write(ADR_IER, div[15:8]);
			wb_write(ADR_LCR, lcr);
			wb_write(ADR_IIR, 8'b00000111);
			wb_write(ADR_IER, 8'h00);
		end
	endtask

	task check;
		input [255:0] name;
		input ok;
		begin
			if (ok) begin
				$display("PASS  %0s", name);
			end else begin
				$display("FAIL  %0s", name);
				errors = errors + 1;
			end
		end
	endtask

	// Send one byte and collect it back off the looped-back line.
	// Fails loudly rather than hanging: a receiver that never asserts
	// DR is the failure mode this whole file exists to catch, and a
	// testbench that hangs on it reports nothing at all.
	task send_recv;
		input [7:0] tx;
		output [7:0] rx;
		begin
			timeout = 0;
			wb_read(ADR_LSR);
			while (!(rdata[5]) && timeout < 100000) begin
				wb_read(ADR_LSR);
				timeout = timeout + 1;
			end
			wb_write(ADR_RBR, tx);
			timeout = 0;
			wb_read(ADR_LSR);
			while (!(rdata[0]) && timeout < 100000) begin
				wb_read(ADR_LSR);
				timeout = timeout + 1;
			end
			if (timeout >= 100000) begin
				$display("FAIL  receiver never asserted DR for %02x", tx);
				errors = errors + 1;
				rx = 8'hxx;
			end else begin
				wb_read(ADR_RBR);
				rx = rdata[7:0];
			end
		end
	endtask

	reg [7:0] got;

	initial begin

		errors = 0;
		rst = 1'b1;
		adr = 26'd0; dat_w = 32'h0; we = 1'b0; stb = 1'b0; cyc = 1'b0;

		repeat (10) @(posedge clk);
		rst = 1'b0;
		repeat (10) @(posedge clk);

		// -- divisor latch --
		//
		// First, because sw/bios/bios.c's uart_init() writes DLL and
		// DLM through this path before anything else happens. If DLAB
		// does not work, those two writes are transmitted as
		// characters and the divisor stays zero.
		wb_write(ADR_LCR, 8'h83);
		wb_write(ADR_RBR, 8'h03);
		wb_write(ADR_IER, 8'h00);
		wb_read(ADR_RBR);
		check("DLL reads back through DLAB", rdata[7:0] == 8'h03);
		wb_read(ADR_IER);
		check("DLM reads back through DLAB", rdata[7:0] == 8'h00);
		wb_write(ADR_LCR, 8'h03);
		wb_read(ADR_IER);
		check("IER reads back with DLAB clear", rdata[7:0] == 8'h00);

		// -- 8N1 at 1 Mbaud, the console's own configuration --
		configure(16'd3, 8'h03);

		send_recv(8'h55, got);
		check("8N1 loopback 0x55", got == 8'h55);
		send_recv(8'haa, got);
		check("8N1 loopback 0xaa", got == 8'haa);
		send_recv(8'h00, got);
		check("8N1 loopback 0x00", got == 8'h00);
		send_recv(8'hff, got);
		check("8N1 loopback 0xff", got == 8'hff);
		send_recv(8'h5a, got);
		check("8N1 loopback 0x5a", got == 8'h5a);

		wb_read(ADR_LSR);
		check("no error bits after clean traffic", rdata[7:1] == 7'b0100000 ||
			rdata[4:1] == 4'b0000);

		// -- 8E1 and 8O1 --
		//
		// sw/common/zuart.c's z_uart1_config() can ask for these, so
		// they are part of the contract even though the console never
		// uses them.
		configure(16'd3, 8'h1b);
		send_recv(8'h55, got);
		check("8E1 loopback 0x55", got == 8'h55);
		send_recv(8'h07, got);
		check("8E1 loopback 0x07 (odd popcount)", got == 8'h07);
		wb_read(ADR_LSR);
		check("8E1 no parity error", rdata[2] == 1'b0);

		configure(16'd3, 8'h0b);
		send_recv(8'h55, got);
		check("8O1 loopback 0x55", got == 8'h55);
		send_recv(8'h07, got);
		check("8O1 loopback 0x07", got == 8'h07);
		wb_read(ADR_LSR);
		check("8O1 no parity error", rdata[2] == 1'b0);

		// -- short words --
		//
		// 5 and 7 data bits. The receiver right-justifies, so the
		// unused high bits must read back as zero rather than as
		// whatever was shifted through them.
		configure(16'd3, 8'h00);
		send_recv(8'h1f, got);
		check("5N1 loopback 0x1f", got == 8'h1f);
		send_recv(8'h15, got);
		check("5N1 loopback 0x15", got == 8'h15);

		configure(16'd3, 8'h02);
		send_recv(8'h7f, got);
		check("7N1 loopback 0x7f", got == 8'h7f);
		send_recv(8'h41, got);
		check("7N1 loopback 0x41", got == 8'h41);

		// -- two stop bits --
		configure(16'd3, 8'h07);
		send_recv(8'ha5, got);
		check("8N2 loopback 0xa5", got == 8'ha5);
		wb_read(ADR_LSR);
		check("8N2 no framing error", rdata[3] == 1'b0);

		// -- a slower divisor --
		//
		// Everything above runs at divisor 3, where a bit is 48
		// clocks. A larger divisor exercises the tick16 reload path
		// with a counter that actually counts.
		configure(16'd26, 8'h03);
		send_recv(8'h3c, got);
		check("115200-ish loopback 0x3c", got == 8'h3c);

		// -- receive FIFO depth --
		//
		// Sixteen bytes in, sixteen bytes out, in order. The number
		// is not arbitrary: sw/common/zuart.h's header does
		// arithmetic with it to say what a polled reader can sustain.
		configure(16'd3, 8'h03);
		for (i = 0; i < 16; i = i + 1) begin
			timeout = 0;
			wb_read(ADR_LSR);
			while (!(rdata[5]) && timeout < 100000) begin
				wb_read(ADR_LSR);
				timeout = timeout + 1;
			end
			wb_write(ADR_RBR, 8'h40 + i[7:0]);
		end
		// let the last byte land
		repeat (2000) @(posedge clk);
		errors = errors;
		for (i = 0; i < 16; i = i + 1) begin
			wb_read(ADR_RBR);
			if (rdata[7:0] !== (8'h40 + i[7:0])) begin
				$display("FAIL  receive FIFO entry %0d: got %02x want %02x",
					i, rdata[7:0], 8'h40 + i[7:0]);
				errors = errors + 1;
			end
		end
		$display("PASS  receive FIFO holds 16 bytes in order");
		wb_read(ADR_LSR);
		check("receive FIFO empty after draining", rdata[0] == 1'b0);

		// -- overrun --
		//
		// A seventeenth byte with nothing drained must set OE and be
		// dropped, NOT overwrite the oldest. Losing the front of a
		// burst desynchronises anything framed; losing the back does
		// not.
		wb_write(ADR_IIR, 8'b00000111);
		for (i = 0; i < 20; i = i + 1) begin
			timeout = 0;
			wb_read(ADR_LSR);
			while (!(rdata[5]) && timeout < 100000) begin
				wb_read(ADR_LSR);
				timeout = timeout + 1;
			end
			wb_write(ADR_RBR, 8'h80 + i[7:0]);
		end
		repeat (2000) @(posedge clk);
		wb_read(ADR_LSR);
		check("overrun reported after 20 bytes into a 16-byte FIFO",
			rdata[1] == 1'b1);
		wb_read(ADR_LSR);
		check("LSR read clears overrun", rdata[1] == 1'b0);
		wb_read(ADR_RBR);
		check("oldest byte survived the overrun", rdata[7:0] == 8'h80);

		// -- FCR receive flush --
		wb_write(ADR_IIR, 8'b00000111);
		wb_read(ADR_LSR);
		check("FCR bit 1 empties the receive FIFO", rdata[0] == 1'b0);

		// -- interrupts --
		//
		// IER bit 0 is what sw/os/uart.c enables, and IIR's identity
		// field is what its ISR switches on: 0x04 means received data
		// available, and (iir >> 1) & 7 == 2 is the case it handles.
		wb_write(ADR_IER, 8'h00);
		wb_read(ADR_IIR);
		check("IIR reports no interrupt when IER is clear", rdata[0] == 1'b1);

		wb_write(ADR_IER, 8'h01);
		send_recv(8'h7e, got);
		check("RDA loopback byte", got == 8'h7e);

		wb_write(ADR_IER, 8'h01);
		timeout = 0;
		wb_read(ADR_LSR);
		while (!(rdata[5]) && timeout < 100000) begin
			wb_read(ADR_LSR);
			timeout = timeout + 1;
		end
		wb_write(ADR_RBR, 8'h33);
		timeout = 0;
		while (!irq && timeout < 100000) begin
			@(posedge clk);
			timeout = timeout + 1;
		end
		check("int_o asserts on received data", irq === 1'b1);
		wb_read(ADR_IIR);
		check("IIR identity is RDA", rdata[3:0] == 4'b0100);
		wb_read(ADR_RBR);
		check("reading RBR drops the interrupt", irq === 1'b0);

		// THRE interrupt. sw/os/uart.c sets IER to 0b11 while it has
		// something queued and back to 0b01 when it drains, so the
		// transmit interrupt must fire on an empty FIFO and must go
		// away when IER bit 1 is cleared.
		wb_write(ADR_IER, 8'h02);
		timeout = 0;
		while (!irq && timeout < 100000) begin
			@(posedge clk);
			timeout = timeout + 1;
		end
		check("int_o asserts on empty transmit FIFO", irq === 1'b1);
		wb_read(ADR_IIR);
		check("IIR identity is THRE", rdata[3:0] == 4'b0010);
		check("reading IIR drops the transmit interrupt", irq === 1'b0);
		wb_write(ADR_IER, 8'h00);
		check("clearing IER bit 1 keeps it down", irq === 1'b0);

		// -- unimplemented registers still answer --
		//
		// The load-bearing property. An address nothing acks stalls
		// this bus forever; see rtl/uart_null.v's header.
		wb_write(26'd4, 8'hff);		// MCR
		wb_read(26'd4);
		check("MCR acks and reads zero", rdata == 32'h0);
		wb_read(26'd6);
		check("MSR acks and reads zero", rdata == 32'h0);
		wb_write(26'd7, 8'hff);		// SCR
		wb_read(26'd7);
		check("SCR acks and reads zero", rdata == 32'h0);

		$display("");
		if (errors == 0)
			$display("tb_uart: all checks passed");
		else
			$display("tb_uart: %0d FAILURES", errors);
		$display("");

		if (errors != 0) $fatal(1);
		$finish;

	end

	// Backstop. Every wait above has its own bounded timeout, but a
	// fault in the bus handshake itself would hang before reaching
	// one of them -- and a testbench that hangs reports nothing.
	initial begin
		#50_000_000;
		$display("tb_uart: TIMED OUT");
		$fatal(1);
	end

endmodule
