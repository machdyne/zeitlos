/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * High-performance word-level blitter with intelligent clipping support
 * for font rendering and precise graphics.
 *
 */

module gpu_blit_wb (
    input wire clk,
    input wire rst,

    // Wishbone SLAVE interface
    input  wire        wb_cyc_i,
    input  wire        wb_stb_i,
    input  wire        wb_we_i,
    input  wire [3:0]  wb_sel_i,
    input  wire [31:0] wb_adr_i,
    input  wire [31:0] wb_dat_i,
    output reg         wb_ack_o,
    output reg  [31:0] wb_dat_o,

    // Wishbone MASTER interface
    output reg         m_cyc_o,
    output reg         m_stb_o,
    output reg         m_we_o,
    output reg  [3:0]  m_sel_o,
    output reg  [31:0] m_adr_o,
    output reg  [31:0] m_dat_o,
    input  wire [31:0] m_dat_i,
    input  wire        m_ack_i,

    output wire        busy
);

    localparam VRAM_BASE = 32'h20000000;

    // Screen parameters
`ifdef GPU_PIXEL_DOUBLE
    localparam SCREEN_STRIDE = 64;
    localparam SCREEN_WIDTH = 512;
    localparam SCREEN_HEIGHT = 384;
`else
    localparam SCREEN_STRIDE = 128;
    localparam SCREEN_WIDTH = 1024;
    localparam SCREEN_HEIGHT = 768;
`endif

    // Control bits
    localparam CTRL_START = 0;
    localparam CTRL_FILL  = 1;
    localparam CTRL_CLIP  = 2;

    // Configuration registers
    reg [31:0] dst_x_reg, dst_y_reg, width_reg, height_reg;
    reg [31:0] src_x_reg, src_y_reg;
    reg [31:0] pattern_reg;
    reg fill_reg, clip_enable_reg;

    // State machine
    reg [2:0] state;
    localparam ST_IDLE = 3'd0, ST_SETUP = 3'd1, ST_READ_DST = 3'd2, 
               ST_WAIT_READ_DST = 3'd3, ST_READ_SRC = 3'd4, 
               ST_WAIT_READ_SRC = 3'd5, ST_WRITE = 3'd6, ST_WAIT_WRITE = 3'd7;

    // Operation variables
    reg [31:0] work_dst_x, work_dst_y, work_width, work_height, work_pattern;
    reg [31:0] work_src_x, work_src_y;
    reg work_fill, work_clip;
    reg draw_busy;

    // Current position
    reg [31:0] current_x, current_y;
    reg [31:0] rect_left, rect_right, rect_top, rect_bottom;
    
    // Current word processing
    reg [31:0] dst_word, src_word, current_word_addr;
    reg [31:0] result_word;

    // Start trigger
    reg start_delayed;
    reg start_fill, start_clip;

    // Calculate current word address and boundaries
    wire [31:0] word_x = current_x >> 5;
    wire [31:0] word_left = word_x << 5;        // Left edge of current word (pixels)
    wire [31:0] word_right = word_left + 32;    // Right edge of current word (pixels)
    
`ifdef GPU_PIXEL_DOUBLE
    wire [31:0] word_addr = (current_y >> 1) * SCREEN_STRIDE + word_x * 4;
    wire [31:0] src_word_addr = ((work_src_y + current_y - work_dst_y) >> 1) * SCREEN_STRIDE + 
                               ((work_src_x + current_x - work_dst_x) >> 5) * 4;
`else
    wire [31:0] word_addr = current_y * SCREEN_STRIDE + word_x * 4;
    wire [31:0] src_word_addr = (work_src_y + current_y - work_dst_y) * SCREEN_STRIDE + 
                               ((work_src_x + current_x - work_dst_x) >> 5) * 4;
`endif

    // Clipping calculations for current word
    wire [31:0] clip_left = (rect_left > word_left) ? rect_left : word_left;
    wire [31:0] clip_right = (rect_right < word_right) ? rect_right : word_right;
    wire word_has_pixels = (clip_left < clip_right) && (current_y >= rect_top) && (current_y < rect_bottom);
    
    // Screen boundary check
    wire word_on_screen = (word_left < SCREEN_WIDTH) && (current_y < SCREEN_HEIGHT);
    
    // Optimization: can we write the entire word without read-modify-write?
    wire full_word_fill = work_fill && (!work_clip || 
                         (clip_left <= word_left && clip_right >= word_right));

    // Generate pixel mask for partial word operations
    // FIXED: Bit 0 = leftmost pixel, bit 31 = rightmost pixel
    reg [31:0] pixel_mask;
    integer i;
    always @(*) begin
        pixel_mask = 32'h0;
        for (i = 0; i < 32; i = i + 1) begin
            if (word_left + i >= clip_left && word_left + i < clip_right) begin
                pixel_mask[i] = 1'b1;  // Bit 0 = leftmost pixel (pixel 0 of word)
            end
        end
    end

    assign busy = draw_busy;

    // Wishbone slave interface
    always @(posedge clk) begin
        if (rst) begin
            dst_x_reg <= 32'h0;
            dst_y_reg <= 32'h0;
            src_x_reg <= 32'h0;
            src_y_reg <= 32'h0;
            width_reg <= 32'h0;
            height_reg <= 32'h0;
            pattern_reg <= 32'h0;
            fill_reg <= 1'b0;
            clip_enable_reg <= 1'b1;
            wb_ack_o <= 1'b0;
            wb_dat_o <= 32'd0;
            start_delayed <= 1'b0;
            start_fill <= 1'b0;
            start_clip <= 1'b1;
        end else begin
            wb_ack_o <= 1'b0;
            start_delayed <= 1'b0;
            
            if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
                wb_ack_o <= 1'b1;
                if (wb_we_i) begin
                    case (wb_adr_i[3:0])
                        4'd0: begin  // CTRL
                            fill_reg <= wb_dat_i[CTRL_FILL];
                            clip_enable_reg <= wb_dat_i[CTRL_CLIP];
                            if (wb_dat_i[CTRL_START] && !draw_busy) begin
                                start_delayed <= 1'b1;
                                start_fill <= wb_dat_i[CTRL_FILL];
                                start_clip <= wb_dat_i[CTRL_CLIP];
                            end
                        end
                        4'd1: ; // STATUS - read only
                        4'd2: dst_x_reg <= wb_dat_i;
                        4'd3: dst_y_reg <= wb_dat_i;
                        4'd4: width_reg <= wb_dat_i;
                        4'd5: height_reg <= wb_dat_i;
                        4'd6: pattern_reg <= wb_dat_i;
                        4'd7: src_x_reg <= wb_dat_i;
                        4'd8: src_y_reg <= wb_dat_i;
                        default: ;
                    endcase
                end else begin
                    case (wb_adr_i[3:0])
                        4'd0: wb_dat_o <= {29'h0, clip_enable_reg, fill_reg, 1'b0};
                        4'd1: wb_dat_o <= {31'h0, draw_busy};
                        4'd2: wb_dat_o <= dst_x_reg;
                        4'd3: wb_dat_o <= dst_y_reg;
                        4'd4: wb_dat_o <= width_reg;
                        4'd5: wb_dat_o <= height_reg;
                        4'd6: wb_dat_o <= pattern_reg;
                        4'd7: wb_dat_o <= src_x_reg;
                        4'd8: wb_dat_o <= src_y_reg;
                        default: wb_dat_o <= 32'd0;
                    endcase
                end
            end
        end
    end

    // Main state machine
    always @(posedge clk) begin
        if (rst) begin
            draw_busy <= 1'b0;
            m_cyc_o <= 1'b0;
            m_stb_o <= 1'b0;
            m_we_o <= 1'b0;
            m_sel_o <= 4'b0000;
            m_adr_o <= 32'd0;
            m_dat_o <= 32'd0;
            state <= ST_IDLE;
        end else begin
            case (state)
                ST_IDLE: begin
                    draw_busy <= 1'b0;
                    m_cyc_o <= 1'b0;
                    m_stb_o <= 1'b0;
                    m_we_o <= 1'b0;
                    
                    if (start_delayed) begin
                        work_dst_x <= dst_x_reg;
                        work_dst_y <= dst_y_reg;
                        work_src_x <= src_x_reg;
                        work_src_y <= src_y_reg;
                        work_width <= width_reg;
                        work_height <= height_reg;
                        work_pattern <= pattern_reg;
                        work_fill <= start_fill;
                        work_clip <= start_clip;
                        
                        draw_busy <= 1'b1;
                        state <= ST_SETUP;
                    end
                end

                ST_SETUP: begin
                    // Calculate rectangle bounds
                    rect_left <= work_dst_x;
                    rect_right <= work_dst_x + work_width;
                    rect_top <= work_dst_y;
                    rect_bottom <= work_dst_y + work_height;
                    
                    // Start at top-left word that might contain pixels
                    current_x <= work_dst_x & ~32'h1F;  // Round down to word boundary
                    current_y <= work_dst_y;
                    
                    state <= ST_READ_DST;
                end

                ST_READ_DST: begin
                    if (word_on_screen && word_has_pixels) begin
                        current_word_addr <= word_addr;
                        
                        if (full_word_fill) begin
                            // Optimization: full word fill, skip read
                            result_word <= work_pattern;
                            state <= ST_WRITE;
                        end else begin
                            // Need to read current word for merge
                            m_cyc_o <= 1'b1;
                            m_stb_o <= 1'b1;
                            m_we_o <= 1'b0;
                            m_sel_o <= 4'b1111;
                            m_adr_o <= VRAM_BASE + word_addr;
                            state <= ST_WAIT_READ_DST;
                        end
                    end else begin
                        // Skip this word
                        state <= ST_WRITE;  // Will advance to next word
                    end
                end

                ST_WAIT_READ_DST: begin
                    if (m_ack_i) begin
                        dst_word <= m_dat_i;
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        
                        if (work_fill) begin
                            // Fill: merge pattern with existing data using mask
                            result_word <= (work_pattern & pixel_mask) | (m_dat_i & ~pixel_mask);
                            state <= ST_WRITE;
                        end else begin
                            state <= ST_READ_SRC;  // Copy mode: read source
                        end
                    end
                end

                ST_READ_SRC: begin
                    // Read source word for copy operation
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b0;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + src_word_addr;
                    state <= ST_WAIT_READ_SRC;
                end

                ST_WAIT_READ_SRC: begin
                    if (m_ack_i) begin
                        src_word <= m_dat_i;
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        
                        // Copy: merge source with existing data using mask
                        result_word <= (m_dat_i & pixel_mask) | (dst_word & ~pixel_mask);
                        state <= ST_WRITE;
                    end
                end

                ST_WRITE: begin
                    if (word_on_screen && word_has_pixels) begin
                        // Write the word
                        m_cyc_o <= 1'b1;
                        m_stb_o <= 1'b1;
                        m_we_o <= 1'b1;
                        m_sel_o <= 4'b1111;
                        m_adr_o <= VRAM_BASE + current_word_addr;
                        m_dat_o <= result_word;
                        state <= ST_WAIT_WRITE;
                    end else begin
                        // Skip write, advance to next word
                        state <= ST_WAIT_WRITE;  // Will advance position
                    end
                end

                ST_WAIT_WRITE: begin
                    if (!word_on_screen || !word_has_pixels || m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        m_we_o <= 1'b0;
                        
                        // Advance to next word
                        current_x <= current_x + 32;
                        
                        // Check if we need to move to next line
                        if (current_x + 32 >= SCREEN_WIDTH || current_x + 32 >= rect_right) begin
                            // Move to next line
                            current_y <= current_y + 1;
                            current_x <= work_dst_x & ~32'h1F;
                            
                            if (current_y + 1 >= SCREEN_HEIGHT || current_y + 1 >= rect_bottom) begin
                                // Done!
                                draw_busy <= 1'b0;
                                state <= ST_IDLE;
                            end else begin
                                state <= ST_READ_DST;
                            end
                        end else begin
                            // Continue on same line
                            state <= ST_READ_DST;
                        end
                    end
                end

                default: begin
                    state <= ST_IDLE;
                end
            endcase
        end
    end

endmodule
