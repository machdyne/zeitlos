/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * High-performance word-level blitter with intelligent clipping support
 * for font rendering and precise graphics.
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
    localparam SCREEN_STRIDE = 64; // 512 pixels / 8 = 64 bytes per line

    // Control bits
    localparam CTRL_START = 0;
    localparam CTRL_FILL  = 1;  // 0=copy, 1=fill
    localparam CTRL_CLIP  = 2;  // 0=no clipping, 1=enable clipping

    // Configuration registers
    reg [31:0] dst_x_reg, dst_y_reg, width_reg, height_reg;
    reg [31:0] pattern_reg;
    reg fill_reg, clip_enable_reg;

    // State machine
    reg [2:0] state;
    localparam ST_IDLE = 3'd0, ST_CLIP = 3'd1, ST_READ = 3'd2, 
               ST_WAIT_READ = 3'd3, ST_WRITE = 3'd4, ST_WAIT_WRITE = 3'd5, ST_NEXT = 3'd6;

    // Operation variables
    reg [31:0] work_dst_x, work_dst_y, work_width, work_height, work_pattern;
    reg work_fill, work_clip;
    reg draw_busy;

    // Clipped rectangle coordinates
    reg [31:0] clip_x, clip_y, clip_width, clip_height;
    reg [31:0] clip_x_end, clip_y_end;

    // Word-level iteration
    reg [31:0] current_line, current_word_in_line;
    reg [31:0] words_per_line, total_lines;
    reg [31:0] line_start_addr, current_word_addr;
    reg [31:0] left_word_x, right_word_x;
    reg [31:0] left_mask, right_mask;
    reg [31:0] read_data;

    // Clipping calculations
    wire [31:0] screen_clip_x_end = 32'd512;
    wire [31:0] screen_clip_y_end = 32'd384;
    wire [31:0] rect_x_end = work_dst_x + work_width;
    wire [31:0] rect_y_end = work_dst_y + work_height;

    // Clipped bounds
    wire [31:0] final_x = work_dst_x;
    wire [31:0] final_y = work_dst_y;
    wire [31:0] final_x_end = (rect_x_end > screen_clip_x_end) ? screen_clip_x_end : rect_x_end;
    wire [31:0] final_y_end = (rect_y_end > screen_clip_y_end) ? screen_clip_y_end : rect_y_end;
    wire [31:0] final_width = final_x_end - final_x;
    wire [31:0] final_height = final_y_end - final_y;

    // Word boundary calculations
    wire [31:0] left_word_boundary = (final_x >> 5) << 5;  // Round down to word boundary
    wire [31:0] right_word_boundary = ((final_x_end + 31) >> 5) << 5;  // Round up to word boundary
    wire [31:0] word_span_width = right_word_boundary - left_word_boundary;
    wire [31:0] word_span_words = word_span_width >> 5;

    // Pixel masks for partial words
    wire [31:0] left_pixel_start = final_x - left_word_boundary;
    wire [31:0] right_pixel_end = final_x_end - ((final_x_end >> 5) << 5);
    wire [31:0] left_pixel_mask = (32'hFFFFFFFF << left_pixel_start);
    wire [31:0] right_pixel_mask = (right_pixel_end == 0) ? 32'hFFFFFFFF : 
                                   (32'hFFFFFFFF >> (32 - right_pixel_end));

    assign busy = draw_busy;

    // Wishbone slave interface
    always @(posedge clk) begin
        if (rst) begin
            dst_x_reg <= 32'h0;
            dst_y_reg <= 32'h0;
            width_reg <= 32'h0;
            height_reg <= 32'h0;
            pattern_reg <= 32'h0;
            fill_reg <= 1'b0;
            clip_enable_reg <= 1'b1;  // Enable clipping by default
            wb_ack_o <= 1'b0;
            wb_dat_o <= 32'd0;
        end else begin
            wb_ack_o <= 1'b0;
            
            if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
                wb_ack_o <= 1'b1;
                if (wb_we_i) begin
                    case (wb_adr_i[3:0])
                        4'd0: begin  // CTRL
                            fill_reg <= wb_dat_i[CTRL_FILL];
                            clip_enable_reg <= wb_dat_i[CTRL_CLIP];
                        end
                        4'd1: ; // STATUS - read only
                        4'd2: dst_x_reg <= wb_dat_i;
                        4'd3: dst_y_reg <= wb_dat_i;
                        4'd4: width_reg <= wb_dat_i;
                        4'd5: height_reg <= wb_dat_i;
                        4'd6: pattern_reg <= wb_dat_i;
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
                        default: wb_dat_o <= 32'd0;
                    endcase
                end
            end
        end
    end

    // Start trigger
    wire start_trigger = wb_cyc_i && wb_stb_i && wb_we_i && 
                        (wb_adr_i[3:0] == 4'd0) && wb_dat_i[CTRL_START] && !draw_busy;

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
                    
                    if (start_trigger) begin
                        // Latch parameters
                        work_dst_x <= dst_x_reg;
                        work_dst_y <= dst_y_reg;
                        work_width <= width_reg;
                        work_height <= height_reg;
                        work_pattern <= pattern_reg;
                        work_fill <= fill_reg;
                        work_clip <= clip_enable_reg;
                        
                        draw_busy <= 1'b1;
                        state <= ST_CLIP;
                    end
                end

                ST_CLIP: begin
                    if (work_clip) begin
                        // Apply clipping
                        clip_x <= final_x;
                        clip_y <= final_y;
                        clip_width <= final_width;
                        clip_height <= final_height;
                        
                        // Check if rectangle is completely clipped
                        if (final_width == 0 || final_height == 0 || 
                            final_x >= screen_clip_x_end || final_y >= screen_clip_y_end) begin
                            // Nothing to draw
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            // Set up word-level iteration
                            current_line <= 32'h0;
                            current_word_in_line <= 32'h0;
                            total_lines <= final_height;
                            words_per_line <= word_span_words;
                            line_start_addr <= final_y * SCREEN_STRIDE + (left_word_boundary >> 3);
                            current_word_addr <= final_y * SCREEN_STRIDE + (left_word_boundary >> 3);
                            
                            // Calculate masks
                            left_mask <= left_pixel_mask;
                            right_mask <= right_pixel_mask;
                            
                            if (work_fill) begin
                                state <= ST_WRITE;
                            end else begin
                                state <= ST_READ;
                            end
                        end
                    end else begin
                        // No clipping - use original dimensions
                        clip_x <= work_dst_x;
                        clip_y <= work_dst_y;
                        clip_width <= work_width;
                        clip_height <= work_height;
                        
                        // Set up word-level iteration
                        current_line <= 32'h0;
                        current_word_in_line <= 32'h0;
                        total_lines <= work_height;
                        words_per_line <= (work_width + 31) >> 5;
                        line_start_addr <= work_dst_y * SCREEN_STRIDE + (work_dst_x >> 5) * 4;
                        current_word_addr <= work_dst_y * SCREEN_STRIDE + (work_dst_x >> 5) * 4;
                        
                        // No masks needed for unclipped
                        left_mask <= 32'hFFFFFFFF;
                        right_mask <= 32'hFFFFFFFF;
                        
                        if (work_fill) begin
                            state <= ST_WRITE;
                        end else begin
                            state <= ST_READ;
                        end
                    end
                end

                ST_READ: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b0;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + current_word_addr;
                    state <= ST_WAIT_READ;
                end

                ST_WAIT_READ: begin
                    if (m_ack_i) begin
                        read_data <= m_dat_i;
                        state <= ST_WRITE;
                    end
                end

                ST_WRITE: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b1;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + current_word_addr;
                    
                    // Apply pixel masks for clipping
                    if (work_fill) begin
                        if (work_clip && words_per_line > 1) begin
                            // Apply masks for partial words
                            if (current_word_in_line == 0) begin
                                // First word in line
                                m_dat_o <= (read_data & ~left_mask) | (work_pattern & left_mask);
                            end else if (current_word_in_line == words_per_line - 1) begin
                                // Last word in line
                                m_dat_o <= (read_data & ~right_mask) | (work_pattern & right_mask);
                            end else begin
                                // Middle word
                                m_dat_o <= work_pattern;
                            end
                        end else if (work_clip && words_per_line == 1) begin
                            // Single word spans entire width
                            m_dat_o <= (read_data & ~(left_mask & right_mask)) | 
                                       (work_pattern & (left_mask & right_mask));
                        end else begin
                            // No clipping needed
                            m_dat_o <= work_pattern;
                        end
                    end else begin
                        // Copy mode - would need source logic
                        m_dat_o <= read_data;
                    end
                    
                    state <= ST_WAIT_WRITE;
                end

                ST_WAIT_WRITE: begin
                    if (m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        m_we_o <= 1'b0;
                        state <= ST_NEXT;
                    end
                end

                ST_NEXT: begin
                    if (current_word_in_line + 1 >= words_per_line) begin
                        // End of line
                        current_word_in_line <= 32'h0;
                        current_line <= current_line + 1;
                        
                        if (current_line + 1 >= total_lines) begin
                            // Operation complete
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            // Next line
                            line_start_addr <= line_start_addr + SCREEN_STRIDE;
                            current_word_addr <= line_start_addr + SCREEN_STRIDE;
                            
                            if (work_fill) begin
                                state <= ST_WRITE;
                            end else begin
                                state <= ST_READ;
                            end
                        end
                    end else begin
                        // Next word in line
                        current_word_in_line <= current_word_in_line + 1;
                        current_word_addr <= current_word_addr + 4;
                        
                        if (work_fill) begin
                            state <= ST_WRITE;
                        end else begin
                            state <= ST_READ;
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
