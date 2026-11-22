// Zeitlos: rtl/xlink.v against an SPI master at the RP2040's 8.93 MHz
// (machboot's docs/xlink.md). iverilog -o tb tb_xlink.v ../../rtl/xlink.v && vvp tb
`timescale 1ns/1ps
module tb_xlink;
	reg clk = 0;
	reg rst = 1;
	always #10.4 clk = ~clk;		// 48 MHz

	reg [31:0] adr = 0;
	reg [31:0] wdat = 0;
	reg we = 0;
	reg cyc = 0;
	wire [31:0] rdat;
	wire ack;
	reg sck = 0;
	reg mosi = 0;
	reg cs_n = 1;
	wire miso;
	wire miso_oe;
	wire msg;

	xlink dut (
		.clk(clk), .rst(rst),
		.wb_adr_i(adr), .wb_dat_i(wdat), .wb_dat_o(rdat), .wb_sel_i(4'hf),
		.wb_we_i(we), .wb_stb_i(cyc), .wb_cyc_i(cyc), .wb_ack_o(ack),
		.x_sck(sck), .x_mosi(mosi), .x_cs_n(cs_n), .x_miso(miso),
		.x_miso_oe(miso_oe), .msg_o(msg)
	);

	integer fails = 0;
	integer checks = 0;
	integer i;
	integer b;
	reg [7:0] so;
	reg [7:0] si;
	reg [31:0] rv;
	reg [7:0] st [0:5];

	task check;
		input c;
		input [8*48-1:0] m;
		begin
			checks = checks + 1;
			if (!c) begin
				fails = fails + 1;
				$display("FAIL: %0s", m);
			end
		end
	endtask

	task wb_read;
		input [31:0] a;
		begin
			@(posedge clk); adr <= a; we <= 0; cyc <= 1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			rv = rdat; cyc <= 0;
			@(posedge clk);
		end
	endtask

	task wb_write;
		input [31:0] a;
		input [31:0] d;
		begin
			@(posedge clk); adr <= a; wdat <= d; we <= 1; cyc <= 1;
			@(posedge clk);
			while (!ack) @(posedge clk);
			cyc <= 0; we <= 0;
			@(posedge clk);
		end
	endtask

	// SPI mode 0, MSB first, 112 ns per bit
	task spi;
		input [7:0] o;
		begin
			for (b = 7; b >= 0; b = b - 1) begin
				mosi = o[b];
				#56 sck = 1;
				si[b] = miso;
				#56 sck = 0;
			end
		end
	endtask

	task status;
		begin
			cs_n = 0; #200;
			spi(8'h01); st[0] = si;
			spi(8'h00); st[1] = si;
			spi(8'h00); st[2] = si;
			spi(8'h00); st[3] = si;
			spi(8'h00); st[4] = si;
			spi(8'h00); st[5] = si;
			#200 cs_n = 1; #400;
		end
	endtask

	initial begin
		#100 rst = 0; #200;

		wb_read(32'h0); check(rv == 32'h4B4E4C58, "MAGIC");
		status;
		check(st[0] == 8'h5A, "STATUS: sync");
		check(st[2][1:0] == 2'b00, "STATUS: empty, no reply");
		check(msg == 0, "msg_o low");

		wb_write(32'hC, 32'h06);
		status;
		check(st[1] == 8'h06, "STATUS: FLAGS byte");

		// SEND an 8-byte request
		cs_n = 0; #200;
		spi(8'h02);
		for (i = 0; i < 8; i = i + 1) spi(8'h10 + i);
		#200 cs_n = 1; #400;
		check(msg == 1, "msg_o: a message waits");
		wb_read(32'h4);
		check(rv[0] == 1, "RX: full");
		check(rv[26:16] == 8, "RX: length 8");
		wb_read(32'h1000); check(rv == 32'h13121110, "RX buffer word 0");
		wb_read(32'h1004); check(rv == 32'h17161514, "RX buffer word 1");
		status;
		check(st[2][0] == 1, "STATUS: receive buffer full");
		wb_write(32'h4, 1);
		check(msg == 0, "msg_o: taken");
		status;
		check(st[2][0] == 0, "STATUS: receive buffer free");

		// a reply of 6 bytes
		wb_write(32'h1800, 32'h0400818c);
		wb_write(32'h1804, 32'h0000beef);
		wb_write(32'h8, 32'h80000006);
		status;
		check(st[2][1] == 1, "STATUS: a reply is ready");
		check(st[3] == 6, "STATUS: its length");
		cs_n = 0; #200;
		spi(8'h03);
		spi(8'h00); check(si == 8'h8c, "RECV byte 0");
		spi(8'h00); check(si == 8'h81, "RECV byte 1");
		spi(8'h00); check(si == 8'h00, "RECV byte 2");
		spi(8'h00); check(si == 8'h04, "RECV byte 3");
		spi(8'h00); check(si == 8'hef, "RECV byte 4");
		spi(8'h00); check(si == 8'hbe, "RECV byte 5");
		#200 cs_n = 1; #400;
		status;
		check(st[2][1] == 0, "STATUS: the reply is gone");
		wb_read(32'h8); check(rv[31] == 0 || rv[0] == 0, "TX: disarmed");

		// a long request: 1518 bytes
		cs_n = 0; #200;
		spi(8'h02);
		for (i = 0; i < 1518; i = i + 1) spi(i[7:0] ^ 8'h55);
		#200 cs_n = 1; #400;
		wb_read(32'h4);
		check(rv[0] == 1 && rv[26:16] == 1518, "RX: 1518 bytes");
		wb_read(32'h1000 + 1512); check(rv == {8'h55 ^ 8'd235, 8'h55 ^ 8'd234, 8'h55 ^ 8'd233, 8'h55 ^ 8'd232}, "RX: the end of it");
		// a second SEND while full is not taken
		cs_n = 0; #200;
		spi(8'h02); spi(8'hAA); spi(8'hBB);
		#200 cs_n = 1; #400;
		wb_read(32'h1000); check(rv[7:0] == 8'h55, "a SEND while full does not overwrite");
		wb_write(32'h4, 1);
		wb_read(32'h10); check(rv >= 2, "FRAMES counts");

		$display("tb_xlink: %0d checks, %0d failed", checks, fails);
		$finish;
	end
endmodule
