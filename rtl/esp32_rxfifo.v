/*
 * Zeitlos -- deep receive FIFO for the ESP32 link (ULX3S UART1 RX).
 *
 * The 16550's 16-byte FIFO cannot hold a ZNIC frame while a
 * time-sliced process is away from the CPU (~4 ms = 400 bytes at
 * 1 Mbaud). This module listens on the same RX pin and buffers 2 KiB
 * in block RAM, so the driver can wait for replies with interrupts
 * enabled and never lose a late one. TX still goes through the 16550.
 *
 * Wishbone (byte address, word access):
 *   +0  R  {19'b0, overrun, count[11:0]}   bytes waiting
 *   +4  R  {24'b0, data}                    pops one byte (0 if empty)
 *   +8  W  any value                        flush FIFO, clear overrun
 */

module esp32_rxfifo #(
	parameter CLK_PER_BIT = 48,	/* 48 MHz / 1 Mbaud */
	parameter DEPTH_BITS = 11	/* 2048 bytes */
) (
	input clk,
	input rst,
	input rx,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input wb_we_i,
	input wb_stb_i,
	input wb_cyc_i,
	output reg wb_ack_o
);

	/* ---- receiver: 8N1, sample mid-bit --------------------------- */
	reg [2:0] rxs;
	always @(posedge clk) rxs <= {rxs[1:0], rx};
	wire rxb = rxs[2];

	localparam ST_IDLE = 2'd0, ST_START = 2'd1, ST_DATA = 2'd2, ST_STOP = 2'd3;
	reg [1:0] state;
	reg [7:0] cnt;
	reg [2:0] idx;
	reg [7:0] shreg;
	reg [7:0] byte_data;
	reg byte_valid;

	always @(posedge clk) begin
		byte_valid <= 1'b0;
		if (rst) begin
			state <= ST_IDLE;
		end else case (state)
		ST_IDLE: if (!rxb) begin
			state <= ST_START;
			cnt <= (CLK_PER_BIT / 2) - 2;	/* to the middle of the start bit */
		end
		ST_START: if (cnt == 0) begin
			if (!rxb) begin
				state <= ST_DATA;
				cnt <= CLK_PER_BIT - 1;
				idx <= 0;
			end else
				state <= ST_IDLE;	/* glitch */
		end else
			cnt <= cnt - 1;
		ST_DATA: if (cnt == 0) begin
			shreg <= {rxb, shreg[7:1]};	/* LSB first */
			cnt <= CLK_PER_BIT - 1;
			idx <= idx + 1;
			if (idx == 3'd7)
				state <= ST_STOP;
		end else
			cnt <= cnt - 1;
		ST_STOP: if (cnt == 0) begin
			if (rxb) begin			/* valid stop bit */
				byte_data <= shreg;
				byte_valid <= 1'b1;
			end
			state <= ST_IDLE;
		end else
			cnt <= cnt - 1;
		endcase
	end

	/* ---- FIFO in block RAM ---------------------------------------- */
	localparam DEPTH = (1 << DEPTH_BITS);
	reg [7:0] mem [0:DEPTH-1];
	reg [DEPTH_BITS-1:0] wr_ptr;
	reg [DEPTH_BITS-1:0] rd_ptr;
	reg [DEPTH_BITS:0] count;
	reg [7:0] rd_data;
	reg overrun;

	wire full = (count == DEPTH);
	wire empty = (count == 0);
	wire do_push = byte_valid && !full;

	/* wishbone: two-cycle access so rd_data (registered BRAM read) is
	 * settled before it is sampled */
	reg pending;
	wire sel_count = (wb_adr_i[3:2] == 2'd0);
	wire sel_data = (wb_adr_i[3:2] == 2'd1);
	wire sel_flush = (wb_adr_i[3:2] == 2'd2);
	wire do_pop = pending && !wb_we_i && sel_data && !empty;
	wire do_flush = pending && wb_we_i && sel_flush;

	always @(posedge clk) begin
		if (do_push)
			mem[wr_ptr] <= byte_data;
		rd_data <= mem[rd_ptr];
	end

	always @(posedge clk) begin
		wb_ack_o <= 1'b0;
		if (rst) begin
			pending <= 1'b0;
			wr_ptr <= 0;
			rd_ptr <= 0;
			count <= 0;
			overrun <= 1'b0;
		end else begin
			if (do_flush) begin
				wr_ptr <= 0;
				rd_ptr <= 0;
				count <= 0;
				overrun <= 1'b0;
			end else begin
				if (do_push)
					wr_ptr <= wr_ptr + 1;
				if (do_pop)
					rd_ptr <= rd_ptr + 1;
				count <= count + {{DEPTH_BITS{1'b0}}, do_push} - {{DEPTH_BITS{1'b0}}, do_pop};
				if (byte_valid && full)
					overrun <= 1'b1;
			end
			if (wb_cyc_i && wb_stb_i && !pending && !wb_ack_o) begin
				pending <= 1'b1;
			end else if (pending) begin
				pending <= 1'b0;
				wb_ack_o <= 1'b1;
				if (sel_count)
					wb_dat_o <= {19'b0, overrun, count[11:0]};
				else if (sel_data)
					wb_dat_o <= {24'b0, empty ? 8'h00 : rd_data};
				else
					wb_dat_o <= 32'b0;
			end
		end
	end

endmodule
