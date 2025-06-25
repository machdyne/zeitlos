/*
 * Zeitlos SOC GPU
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Line rasterizer with command FIFO and clipping.
 *
 */

module gpu_raster_wb (
    input           clk,
    input           rst,

    // Wishbone SLAVE interface (CPU control)
    input           wb_cyc_i,
    input           wb_stb_i,
    input           wb_we_i,
    input   [3:0]   wb_sel_i,
    input   [31:0]  wb_adr_i,
    input   [31:0]  wb_dat_i,
    output  reg     wb_ack_o,
    output  reg [31:0] wb_dat_o,

    // Wishbone MASTER interface (to framebuffer memory)
    output  reg         m_cyc_o,
    output  reg         m_stb_o,
    output  reg         m_we_o,
    output  reg [3:0]   m_sel_o,
    output  reg [31:0]  m_adr_o,
    output  reg [31:0]  m_dat_o,
    input       [31:0]  m_dat_i,
    input               m_ack_i
);

// CPU interface registers  
reg [8:0] cpu_x0, cpu_x1, cpu_y0, cpu_y1;
reg cpu_color;

// Clipping registers
reg [8:0] clip_x0, clip_y0, clip_x1, clip_y1;
reg clip_enable;

// Command FIFO parameters
parameter FIFO_DEPTH = 16;
parameter FIFO_ADDR_WIDTH = 4;

// Command FIFO storage (37 bits per entry: 9+9+9+9+1)
reg [36:0] fifo_mem [0:FIFO_DEPTH-1];
reg [FIFO_ADDR_WIDTH-1:0] fifo_wr_ptr;
reg [FIFO_ADDR_WIDTH-1:0] fifo_rd_ptr;
reg [FIFO_ADDR_WIDTH:0] fifo_count;

// FIFO signals
wire fifo_empty = (fifo_count == 0);
wire fifo_full = (fifo_count == FIFO_DEPTH);
wire fifo_push;
wire fifo_pop;

// Current drawing command registers
reg [8:0] x0, x1, y0, y1;
reg color;

// Drawing state registers
reg [8:0] cur_x, cur_y;
reg draw_busy;
reg [15:0] pixel_count;

// FSM
reg [2:0] state;
parameter IDLE           = 3'd0;
parameter SETUP          = 3'd1;
parameter READ           = 3'd2;
parameter WAIT_READ      = 3'd3;
parameter WRITE          = 3'd4;
parameter WAIT_WRITE     = 3'd5;
parameter NEXT           = 3'd6;
parameter DONE           = 3'd7;

// Bresenham algorithm signals
wire signed [10:0] deltax, deltay;
wire signed [10:0] dx, dy;
wire right, down;
wire signed [12:0] err_init;

// Current error term (registered)
reg signed [12:0] err;

// Bresenham step calculation
wire signed [12:0] e2;
wire e2_gt_dy, e2_lt_dx;
wire signed [12:0] err1, err2;
wire [8:0] xa, ya, xb, yb;
wire [8:0] next_x, next_y;
wire at_end;

// Calculate step directions and distances
assign deltax = x1 - x0;
assign right = ~deltax[8];   // right if deltax is not negative
assign dx = right ? deltax : -deltax;     // dx is always positive

assign deltay = y1 - y0;
assign down = ~deltay[8];    // down if deltay is not negative  
assign dy = down ? -deltay : deltay;      // dy is always negative

// Error calculation
assign err_init = dx + dy;
assign e2 = err << 1;
assign e2_gt_dy = (e2 > dy);
assign e2_lt_dx = (e2 < dx);

// Step calculation
assign xa = right ? (cur_x + 1) : (cur_x - 1);
assign xb = e2_gt_dy ? xa : cur_x;
assign ya = down ? (cur_y + 1) : (cur_y - 1);
assign yb = e2_lt_dx ? ya : cur_y;

assign next_x = xb;
assign next_y = yb;

// Error update
assign err1 = e2_gt_dy ? (err + dy) : err;
assign err2 = e2_lt_dx ? (err1 + dx) : err1;

// Check if we've reached the end
assign at_end = (cur_x == x1) && (cur_y == y1);

// FIFO control
assign fifo_push = wb_cyc_i && wb_stb_i && wb_we_i && (wb_adr_i[3:0] == 4'd5) && !fifo_full;
assign fifo_pop = (state == SETUP);

// Overall busy signal (FIFO not empty OR currently drawing)
wire busy_signal = !fifo_empty || draw_busy;

// Clipping check
wire pixel_in_clip = !clip_enable || 
                    (cur_x >= clip_x0 && cur_x <= clip_x1 && 
                     cur_y >= clip_y0 && cur_y <= clip_y1);

// VRAM address calculation
wire [31:0] pixel_word_addr = 32'h20000000 + ((cur_y << 4) + (cur_x >> 5)) * 4;
wire [4:0] pixel_bit_pos = cur_x[4:0];
wire [31:0] pixel_mask = 32'h00000001 << pixel_bit_pos;

// FIFO management
always @(posedge clk) begin
    if (rst) begin
        fifo_wr_ptr <= 0;
        fifo_rd_ptr <= 0;
        fifo_count <= 0;
    end else begin
        case ({fifo_push, fifo_pop})
            2'b10: begin // Push only
                fifo_mem[fifo_wr_ptr] <= {cpu_color, cpu_y1, cpu_x1, cpu_y0, cpu_x0};
                fifo_wr_ptr <= fifo_wr_ptr + 1;
                fifo_count <= fifo_count + 1;
            end
            2'b01: begin // Pop only
                fifo_rd_ptr <= fifo_rd_ptr + 1;
                fifo_count <= fifo_count - 1;
            end
            2'b11: begin // Push and pop simultaneously
                fifo_mem[fifo_wr_ptr] <= {cpu_color, cpu_y1, cpu_x1, cpu_y0, cpu_x0};
                fifo_wr_ptr <= fifo_wr_ptr + 1;
                fifo_rd_ptr <= fifo_rd_ptr + 1;
                // fifo_count stays the same
            end
            // 2'b00: No change
        endcase
    end
end

// Wishbone register interface
always @(posedge clk) begin
    if (rst) begin
        cpu_x0 <= 9'd0;
        cpu_y0 <= 9'd0;
        cpu_x1 <= 9'd0;
        cpu_y1 <= 9'd0;
        cpu_color <= 1'b0;
        clip_x0 <= 9'd0;
        clip_y0 <= 9'd0;
        clip_x1 <= 9'd511;  // Default to full screen
        clip_y1 <= 9'd511;
        clip_enable <= 1'b0;  // Disabled by default
        wb_ack_o <= 1'b0;
        wb_dat_o <= 32'd0;
    end else begin
        wb_ack_o <= 1'b0;
        
        if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
            wb_ack_o <= 1'b1;
            if (wb_we_i) begin
                case (wb_adr_i[4:0])
                    5'd0: cpu_x0 <= wb_dat_i[8:0];
                    5'd1: cpu_y0 <= wb_dat_i[8:0];
                    5'd2: cpu_x1 <= wb_dat_i[8:0];
                    5'd3: cpu_y1 <= wb_dat_i[8:0];
                    5'd4: cpu_color <= wb_dat_i[0];
                    5'd5: ; // Start command - handled by FIFO push logic
                    5'd11: clip_x0 <= wb_dat_i[8:0];
                    5'd12: clip_y0 <= wb_dat_i[8:0];
                    5'd13: clip_x1 <= wb_dat_i[8:0];
                    5'd14: clip_y1 <= wb_dat_i[8:0];
                    5'd15: clip_enable <= wb_dat_i[0];
                    default: ;
                endcase
            end else begin
                case (wb_adr_i[4:0])
                    5'd0: wb_dat_o <= {23'd0, cpu_x0};
                    5'd1: wb_dat_o <= {23'd0, cpu_y0};
                    5'd2: wb_dat_o <= {23'd0, cpu_x1};
                    5'd3: wb_dat_o <= {23'd0, cpu_y1};
                    5'd4: wb_dat_o <= {31'd0, cpu_color};
                    5'd5: wb_dat_o <= 32'd0; // Start register (write-only)
                    5'd6: wb_dat_o <= {31'd0, busy_signal};
                    5'd7: wb_dat_o <= {16'd0, pixel_count};
                    5'd8: wb_dat_o <= {23'd0, cur_x};
                    5'd9: wb_dat_o <= {23'd0, cur_y};
                    5'd10: wb_dat_o <= {27'd0, fifo_count}; // Debug: FIFO count
                    5'd11: wb_dat_o <= {23'd0, clip_x0};
                    5'd12: wb_dat_o <= {23'd0, clip_y0};
                    5'd13: wb_dat_o <= {23'd0, clip_x1};
                    5'd14: wb_dat_o <= {23'd0, clip_y1};
                    5'd15: wb_dat_o <= {31'd0, clip_enable};
                    default: wb_dat_o <= 32'd0;
                endcase
            end
        end
    end
end

// Main FSM and Bresenham algorithm
always @(posedge clk) begin
    if (rst) begin
        draw_busy <= 1'b0;
        m_cyc_o <= 1'b0;
        m_stb_o <= 1'b0;
        m_we_o <= 1'b0;
        m_sel_o <= 4'b0000;
        m_adr_o <= 32'd0;
        m_dat_o <= 32'd0;
        state <= IDLE;
        cur_x <= 9'd0;
        cur_y <= 9'd0;
        err <= 13'd0;
        pixel_count <= 16'd0;
        x0 <= 9'd0;
        y0 <= 9'd0;
        x1 <= 9'd0;
        y1 <= 9'd0;
        color <= 1'b0;
    end else begin
        case(state)
        IDLE: begin
            draw_busy <= 1'b0;
            m_cyc_o <= 1'b0;
            m_stb_o <= 1'b0;
            m_we_o <= 1'b0;

            // Check if there's a command in the FIFO
            if (!fifo_empty) begin
                // Load command from FIFO
                {color, y1, x1, y0, x0} <= fifo_mem[fifo_rd_ptr];
                draw_busy <= 1'b1;
                state <= SETUP;
            end
        end

        SETUP: begin
            // Initialize starting position and error term
            cur_x <= x0;
            cur_y <= y0;
            err <= err_init;
            pixel_count <= 16'd0;
            state <= READ;
        end

        READ: begin
            // Check if pixel is within clip bounds
            if (pixel_in_clip) begin
                m_cyc_o <= 1'b1;
                m_stb_o <= 1'b1;
                m_we_o <= 1'b0;
                m_sel_o <= 4'b1111;
                m_adr_o <= pixel_word_addr;
                state <= WAIT_READ;
            end else begin
                // Skip this pixel, but continue line algorithm
                pixel_count <= pixel_count + 1;
                if (at_end || pixel_count > 1000) begin
                    state <= DONE;
                end else begin
                    state <= NEXT;
                end
            end
        end

        WAIT_READ: begin
            if (m_ack_i) begin
                m_we_o <= 1'b1;
                if (color)
                    m_dat_o <= m_dat_i | pixel_mask;
                else
                    m_dat_o <= m_dat_i & ~pixel_mask;
                state <= WRITE;
            end
        end

        WRITE: begin
            state <= WAIT_WRITE;
        end

        WAIT_WRITE: begin
            if (m_ack_i) begin
                m_cyc_o <= 1'b0;
                m_stb_o <= 1'b0;
                m_we_o <= 1'b0;
                pixel_count <= pixel_count + 1;
                
                // Check if we've reached the endpoint
                if (at_end || pixel_count > 1000) begin
                    state <= DONE;
                end else begin
                    state <= NEXT;
                end
            end
        end

        NEXT: begin
            // Update coordinates and error using combinatorial logic
            cur_x <= next_x;
            cur_y <= next_y;
            err <= err2;
            state <= READ;
        end

        DONE: begin
            draw_busy <= 1'b0;
            state <= IDLE;  // Will check FIFO again in next cycle
        end

        default: begin
            state <= IDLE;
        end
        endcase
    end
end

endmodule
