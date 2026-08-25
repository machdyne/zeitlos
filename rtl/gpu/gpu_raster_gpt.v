/*
 * Zeitlos SOC GPU
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * GPU Wishbone command interface & rasterizer.
 *
 */

module gpu_raster_wb (
    input  wire        clk,
    input  wire        rst,

    // Wishbone slave interface (command input)
    input  wire [31:0] wb_adr_i,
    input  wire [31:0] wb_dat_i,
    output reg  [31:0] wb_dat_o,
    input  wire        wb_we_i,
    input  wire  [3:0] wb_sel_i,
    input  wire        wb_stb_i,
    input  wire        wb_cyc_i,
    output reg         wb_ack_o,

    // Wishbone master interface to VRAM (framebuffer)
    output reg  [31:0] m_adr_o,
    output reg  [31:0] m_dat_o,
    input  wire [31:0] m_dat_i,
    output reg         m_cyc_o,
    output reg         m_stb_o,
    output reg         m_we_o,
    output wire [3:0]  m_sel_o,
    input  wire        m_ack_i,

    output wire [7:0] dbg
);

    localparam VRAM_ADDR = 32'h2000_0000;
    localparam COORD_BITS = 10;

`ifdef GPU_PIXEL_DOUBLE
    localparam FB_WIDTH = 512;
`else
    localparam FB_WIDTH = 1024;
`endif

    // FSM states
    localparam IDLE       = 3'd0;
    localparam READ_PIXEL = 3'd1;
    localparam WAIT_READ  = 3'd2;
    localparam DRAW_PIXEL = 3'd3;
    localparam WAIT_WRITE = 3'd4;

    reg [2:0] state;

    // Command FIFO (16 entries of 32 bits)
    reg [31:0] cmd_fifo [0:15];
    reg [3:0] wr_ptr, rd_ptr;
    reg [4:0] fifo_count;  // max 16 entries, 5 bits needed

    // Line drawing registers
    reg [9:0] x0, y0, x1, y1;
    reg [9:0] x, y;
    reg [10:0] dx, dy;
    reg signed [10:0] sx, sy;
    reg signed [11:0] err;  // signed wider for error calculations
    reg [31:0] pixel_data_latched;
    reg color;

    // Declare new_err at module scope (fix for local declaration issue)
    reg signed [11:0] new_err;

    assign m_sel_o = 4'b1111;
    assign dbg = {x[2:0], state};

    // Combinational signals for fifo count increment/decrement
    wire fifo_write = wb_stb_i && wb_cyc_i && wb_we_i && !wb_ack_o &&
		(fifo_count < 16);
    wire fifo_read  = (state == IDLE) && (fifo_count >= 2);

    // Wishbone command FIFO write logic and fifo_count update
    always @(posedge clk) begin
        if (rst) begin
            wr_ptr <= 0;
            rd_ptr <= 0;
            fifo_count <= 0;
            wb_ack_o <= 0;
        end else begin
            wb_ack_o <= 0;

            // FIFO write
            if (fifo_write) begin
                cmd_fifo[wr_ptr] <= wb_dat_i;
                wr_ptr <= wr_ptr + 1;
                fifo_count <= fifo_count + 1;
                wb_ack_o <= 1;
            end

            // FIFO read (consume 2 commands)
            else if (fifo_read) begin
                rd_ptr <= rd_ptr + 2;
                fifo_count <= fifo_count - 2;
            end

            // If neither writing nor reading, fifo_count stays the same
        end
    end

    // Wishbone command read response (for reads, no fifo change)
    always @(posedge clk) begin
        if (rst) begin
            wb_dat_o <= 0;
        end else begin
            if (wb_stb_i && wb_cyc_i && !wb_we_i && !wb_ack_o) begin
                // Return FIFO count
                wb_dat_o <= { 27'b0, fifo_count };
            end
        end
    end

    // Line drawing FSM
    always @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            m_cyc_o <= 0;
            m_stb_o <= 0;
            m_we_o <= 0;
            m_adr_o <= 0;
            m_dat_o <= 0;
            x <= 0; y <= 0;
            err <= 0;
            new_err <= 0;
            color <= 0;
            x0 <= 0; y0 <= 0; x1 <= 0; y1 <= 0;
            pixel_data_latched <= 0;
        end else begin
            case(state)
                IDLE: begin
                    m_cyc_o <= 0;
                    m_stb_o <= 0;
                    m_we_o <= 0;

                    if (fifo_count >= 2) begin
                        color <= cmd_fifo[rd_ptr][31];
                        x0 <= cmd_fifo[rd_ptr][29 -: COORD_BITS];
                        y0 <= cmd_fifo[rd_ptr][19 -: COORD_BITS];

                        x1 <= cmd_fifo[(rd_ptr + 1) & 4'hF][29 -: COORD_BITS] +
									cmd_fifo[rd_ptr][29 -: COORD_BITS];
                        y1 <= cmd_fifo[(rd_ptr + 1) & 4'hF][19 -: COORD_BITS] +
									cmd_fifo[rd_ptr][19 -: COORD_BITS];

                        x <= cmd_fifo[rd_ptr][29 -: COORD_BITS];
                        y <= cmd_fifo[rd_ptr][19 -: COORD_BITS];

                        dx <= (x1 > x0) ? (x1 - x0) : (x0 - x1);
                        dy <= (y1 > y0) ? (y1 - y0) : (y0 - y1);

                        sx <= (x1 > x0) ? 1 : -1;
                        sy <= (y1 > y0) ? 1 : -1;

                        err <= (dx > dy) ? (dx - dy) : -(dy - dx);

                        state <= READ_PIXEL;
                    end
                end

                READ_PIXEL: begin
                    m_adr_o <= VRAM_ADDR + ((((y * FB_WIDTH) + x) >> 5) << 2);
                    m_cyc_o <= 1;
                    m_stb_o <= 1;
                    m_we_o <= 0;
                    state <= WAIT_READ;
                end

                WAIT_READ: begin
                    if (m_ack_i) begin
                        m_cyc_o <= 0;
                        m_stb_o <= 0;
                        pixel_data_latched <= m_dat_i;
                        state <= DRAW_PIXEL;
                    end
                end

                DRAW_PIXEL: begin
                    m_adr_o <= VRAM_ADDR + (((y * FB_WIDTH + x) >> 5) << 2);
                    if (color)
                        m_dat_o <= pixel_data_latched | (32'h80000000 >> x[4:0]);
                    else
                        m_dat_o <= pixel_data_latched & ~(32'h80000000 >> x[4:0]);

                    m_cyc_o <= 1;
                    m_stb_o <= 1;
                    m_we_o <= 1;
                    state <= WAIT_WRITE;
                end

                WAIT_WRITE: begin
                    if (m_ack_i) begin
                        m_cyc_o <= 0;
                        m_stb_o <= 0;
                        m_we_o <= 0;

                        if (x == x1 && y == y1) begin
                            state <= IDLE;
                        end else begin
                            new_err = err;
                            if ((new_err << 1) > -dy) begin
                                new_err = new_err - dy;
                                x <= x + sx;
                            end
                            if ((new_err << 1) < dx) begin
                                new_err = new_err + dx;
                                y <= y + sy;
                            end
                            err <= new_err;
                            state <= READ_PIXEL;
                        end
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule

