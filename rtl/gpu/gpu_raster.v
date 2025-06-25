/*
 * Zeitlos SOC GPU
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Line rasterizer.
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

// Registers  
reg [8:0] x0, x1, y0, y1;
reg [8:0] cur_x, cur_y;
reg color;
reg busy_reg;
reg start_req;
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

// Start pulse generation
wire start_pulse = start_req && !busy_reg;

// VRAM address calculation
wire [31:0] pixel_word_addr = 32'h20000000 + ((cur_y << 4) + (cur_x >> 5)) * 4;
wire [4:0] pixel_bit_pos = cur_x[4:0];
wire [31:0] pixel_mask = 32'h00000001 << pixel_bit_pos;

// Wishbone register interface
always @(posedge clk) begin
    if (rst) begin
        x0 <= 9'd0;
        y0 <= 9'd0;
        x1 <= 9'd0;
        y1 <= 9'd0;
        color <= 1'b0;
        start_req <= 1'b0;
        wb_ack_o <= 1'b0;
        wb_dat_o <= 32'd0;
    end else begin
        wb_ack_o <= 1'b0;
        
        if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
            wb_ack_o <= 1'b1;
            if (wb_we_i) begin
                case (wb_adr_i[3:0])
                    4'd0: x0 <= wb_dat_i[8:0];
                    4'd1: y0 <= wb_dat_i[8:0];
                    4'd2: x1 <= wb_dat_i[8:0];
                    4'd3: y1 <= wb_dat_i[8:0];
                    4'd4: color <= wb_dat_i[0];
                    4'd5: if (!busy_reg) start_req <= 1'b1;
                    default: ;
                endcase
            end else begin
                case (wb_adr_i[3:0])
                    4'd0: wb_dat_o <= {23'd0, x0};
                    4'd1: wb_dat_o <= {23'd0, y0};
                    4'd2: wb_dat_o <= {23'd0, x1};
                    4'd3: wb_dat_o <= {23'd0, y1};
                    4'd4: wb_dat_o <= {31'd0, color};
                    4'd5: wb_dat_o <= {31'd0, start_req};
                    4'd6: wb_dat_o <= {31'd0, busy_reg};
                    4'd7: wb_dat_o <= {16'd0, pixel_count};
                    4'd8: wb_dat_o <= {23'd0, cur_x};
                    4'd9: wb_dat_o <= {23'd0, cur_y};
                    default: wb_dat_o <= 32'd0;
                endcase
            end
        end

        if (start_pulse)
            start_req <= 1'b0;
    end
end

// Main FSM and Bresenham algorithm
always @(posedge clk) begin
    if (rst) begin
        busy_reg <= 1'b0;
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
    end else begin
        case(state)
        IDLE: begin
            busy_reg <= 1'b0;
            m_cyc_o <= 1'b0;
            m_stb_o <= 1'b0;
            m_we_o <= 1'b0;

            if (start_pulse) begin
                busy_reg <= 1'b1;
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
            m_cyc_o <= 1'b1;
            m_stb_o <= 1'b1;
            m_we_o <= 1'b0;
            m_sel_o <= 4'b1111;
            m_adr_o <= pixel_word_addr;
            state <= WAIT_READ;
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
            busy_reg <= 1'b0;
            state <= IDLE;
        end

        default: begin
            state <= IDLE;
        end
        endcase
    end
end

endmodule
