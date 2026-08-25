/*
 * Zeitlos SOC - Hybrid Performance Blitter
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Smart blitter that uses word-level operations when possible,
 * pixel-level operations when needed for precision.
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
    localparam CTRL_START     = 0;
    localparam CTRL_MODE      = 1;  // 0=word-aligned, 1=pixel-precise
    localparam CTRL_FILL      = 2;  // 0=copy, 1=fill

    // Configuration registers
    reg [31:0] dst_x_reg, dst_y_reg, width_reg, height_reg;
    reg [31:0] pattern_reg;
    reg mode_reg, fill_reg;

    // State machine
    reg [3:0] state;
    localparam ST_IDLE = 4'd0, ST_ANALYZE = 4'd1, ST_WORD_SETUP = 4'd2, 
               ST_WORD_FILL = 4'd3, ST_WORD_WAIT = 4'd4, ST_WORD_NEXT = 4'd5,
               ST_PIXEL_READ = 4'd6, ST_PIXEL_WAIT_READ = 4'd7, 
               ST_PIXEL_WRITE = 4'd8, ST_PIXEL_WAIT_WRITE = 4'd9, ST_PIXEL_NEXT = 4'd10;

    // Operation variables
    reg [31:0] work_dst_x, work_dst_y, work_width, work_height, work_pattern;
    reg work_mode, work_fill;
    reg draw_busy;

    // Word-level variables
    reg [31:0] word_addr, words_remaining, lines_remaining;
    reg [31:0] line_start_addr;

    // Pixel-level variables  
    reg [31:0] pixel_x, pixel_y, current_pixel_x, current_pixel_y;
    reg [31:0] read_data;

    // Analysis results
    reg use_word_mode;
    reg [31:0] left_edge_pixels, right_edge_pixels, middle_words;
    reg [31:0] left_mask, right_mask;

    // Current word address and pixel mask calculations
    wire [31:0] current_word_addr = work_dst_y * SCREEN_STRIDE + current_pixel_y * SCREEN_STRIDE + (current_pixel_x >> 5) * 4;
    wire [4:0] bit_offset = current_pixel_x[4:0];
    wire [31:0] pixel_mask = 32'h1 << bit_offset;

    assign busy = draw_busy;

    // Wishbone slave interface
    always @(posedge clk) begin
        if (rst) begin
            dst_x_reg <= 32'h0;
            dst_y_reg <= 32'h0;
            width_reg <= 32'h0;
            height_reg <= 32'h0;
            pattern_reg <= 32'h0;
            mode_reg <= 1'b0;
            fill_reg <= 1'b0;
            wb_ack_o <= 1'b0;
            wb_dat_o <= 32'd0;
        end else begin
            wb_ack_o <= 1'b0;
            
            if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
                wb_ack_o <= 1'b1;
                if (wb_we_i) begin
                    case (wb_adr_i[3:0])
                        4'd0: begin  // CTRL
                            mode_reg <= wb_dat_i[CTRL_MODE];
                            fill_reg <= wb_dat_i[CTRL_FILL];
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
                        4'd0: wb_dat_o <= {29'h0, fill_reg, mode_reg, 1'b0};
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
            use_word_mode <= 1'b0;
            // Initialize all other registers...
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
                        work_mode <= mode_reg;
                        work_fill <= fill_reg;
                        
                        draw_busy <= 1'b1;
                        state <= ST_ANALYZE;
                    end
                end

                ST_ANALYZE: begin
                    // Analyze operation to choose optimal strategy
                    if (work_mode == 1'b0) begin
                        // Force word mode
                        use_word_mode <= 1'b1;
                        state <= ST_WORD_SETUP;
                    end else if (work_width >= 32 && (work_dst_x & 31) == 0) begin
                        // Large operation, word-aligned: use word mode
                        use_word_mode <= 1'b1;
                        state <= ST_WORD_SETUP;
                    end else begin
                        // Small or unaligned: use pixel mode
                        use_word_mode <= 1'b0;
                        pixel_x <= 32'h0;
                        pixel_y <= 32'h0;
                        current_pixel_x <= work_dst_x;
                        current_pixel_y <= work_dst_y;
                        state <= ST_PIXEL_READ;
                    end
                end

                ST_WORD_SETUP: begin
                    // Set up word-level operation
                    word_addr <= work_dst_y * SCREEN_STRIDE + (work_dst_x >> 5) * 4;
                    words_remaining <= (work_width + 31) >> 5;
                    lines_remaining <= work_height;
                    line_start_addr <= work_dst_y * SCREEN_STRIDE + (work_dst_x >> 5) * 4;
                    state <= ST_WORD_FILL;
                end

                ST_WORD_FILL: begin
                    // Fast word-level fill
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b1;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + word_addr;
                    m_dat_o <= work_pattern;
                    state <= ST_WORD_WAIT;
                end

                ST_WORD_WAIT: begin
                    if (m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        m_we_o <= 1'b0;
                        state <= ST_WORD_NEXT;
                    end
                end

                ST_WORD_NEXT: begin
                    if (words_remaining == 1) begin
                        // End of line
                        words_remaining <= (work_width + 31) >> 5;
                        lines_remaining <= lines_remaining - 1;
                        
                        if (lines_remaining == 1) begin
                            // Done
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            // Next line
                            line_start_addr <= line_start_addr + SCREEN_STRIDE;
                            word_addr <= line_start_addr + SCREEN_STRIDE;
                            state <= ST_WORD_FILL;
                        end
                    end else begin
                        // Next word in line
                        words_remaining <= words_remaining - 1;
                        word_addr <= word_addr + 4;
                        state <= ST_WORD_FILL;
                    end
                end

                // Pixel-level states (similar to previous version)
                ST_PIXEL_READ: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b0;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + current_word_addr;
                    state <= ST_PIXEL_WAIT_READ;
                end

                ST_PIXEL_WAIT_READ: begin
                    if (m_ack_i) begin
                        read_data <= m_dat_i;
                        state <= ST_PIXEL_WRITE;
                    end
                end

                ST_PIXEL_WRITE: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b1;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + current_word_addr;
                    
                    if (work_pattern[0]) begin
                        m_dat_o <= read_data | pixel_mask;
                    end else begin
                        m_dat_o <= read_data & ~pixel_mask;
                    end
                    
                    state <= ST_PIXEL_WAIT_WRITE;
                end

                ST_PIXEL_WAIT_WRITE: begin
                    if (m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        m_we_o <= 1'b0;
                        state <= ST_PIXEL_NEXT;
                    end
                end

                ST_PIXEL_NEXT: begin
                    if (pixel_x + 1 >= work_width) begin
                        // End of line
                        pixel_x <= 32'h0;
                        pixel_y <= pixel_y + 1;
                        current_pixel_x <= work_dst_x;
                        current_pixel_y <= current_pixel_y + 1;
                        
                        if (pixel_y + 1 >= work_height) begin
                            // Done
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            state <= ST_PIXEL_READ;
                        end
                    end else begin
                        // Next pixel
                        pixel_x <= pixel_x + 1;
                        current_pixel_x <= current_pixel_x + 1;
                        state <= ST_PIXEL_READ;
                    end
                end

                default: begin
                    state <= ST_IDLE;
                end
            endcase
        end
    end

endmodule
