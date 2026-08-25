/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * GPU blitter with Wishbone interfaces - COMPLETE FIXED VERSION
 */

module gpu_blit_wb (
    input wire clk,
    input wire rst,

    // Wishbone SLAVE interface (CPU configuration)
    input  wire        wb_cyc_i,
    input  wire        wb_stb_i,
    input  wire        wb_we_i,
    input  wire [3:0]  wb_sel_i,
    input  wire [31:0] wb_adr_i,
    input  wire [31:0] wb_dat_i,
    output reg         wb_ack_o,
    output reg  [31:0] wb_dat_o,

    // Wishbone MASTER interface (to VRAM)
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

    // Control bits
    localparam CTRL_START  = 0;
    localparam CTRL_MODE   = 1;  // 0=linear, 1=block
    localparam CTRL_FILL   = 2;  // 0=copy, 1=fill
    localparam CTRL_RESET  = 3;  // Force reset

    // Configuration registers
    reg [31:0] src_addr_reg, dst_addr_reg, width_reg, height_reg;
    reg [31:0] src_stride_reg, dst_stride_reg, pattern_reg;
    reg mode_reg, fill_reg;

    // State machine
    reg [2:0] state;
    localparam ST_IDLE = 3'd0, ST_READ = 3'd1, ST_WAIT_READ = 3'd2, 
               ST_WRITE = 3'd3, ST_WAIT_WRITE = 3'd4, ST_NEXT = 3'd5;

    // Operation variables
    reg [31:0] work_src_addr, work_dst_addr, work_width, work_height;
    reg [31:0] work_src_stride, work_dst_stride, work_pattern;
    reg work_mode, work_fill;
    reg [31:0] current_src_addr, current_dst_addr;
    reg [31:0] line_src_start, line_dst_start;
    reg [31:0] x_count, y_count, words_per_line;
    reg [31:0] read_data;
    reg draw_busy;

    assign busy = draw_busy;

    // Wishbone slave interface
    always @(posedge clk) begin
        if (rst) begin
            src_addr_reg <= 32'h0;
            dst_addr_reg <= 32'h0;
            width_reg <= 32'h0;
            height_reg <= 32'h0;
            src_stride_reg <= 32'h0;
            dst_stride_reg <= 32'h0;
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
                    case (wb_adr_i[4:0])
                        5'd0: begin  // CTRL - start trigger handled in state machine
                            mode_reg <= wb_dat_i[CTRL_MODE];
                            fill_reg <= wb_dat_i[CTRL_FILL];
                        end
                        5'd1: ; // STATUS - read only
                        5'd2: src_addr_reg <= wb_dat_i;
                        5'd3: dst_addr_reg <= wb_dat_i;
                        5'd4: width_reg <= wb_dat_i;
                        5'd5: height_reg <= wb_dat_i;
                        5'd6: src_stride_reg <= wb_dat_i;
                        5'd7: dst_stride_reg <= wb_dat_i;
                        5'd8: pattern_reg <= wb_dat_i;
                        default: ;
                    endcase
                end else begin
                    case (wb_adr_i[4:0])
                        5'd0: wb_dat_o <= {29'h0, fill_reg, mode_reg, 1'b0};
                        5'd1: wb_dat_o <= {31'h0, draw_busy};
                        5'd2: wb_dat_o <= src_addr_reg;
                        5'd3: wb_dat_o <= dst_addr_reg;
                        5'd4: wb_dat_o <= width_reg;
                        5'd5: wb_dat_o <= height_reg;
                        5'd6: wb_dat_o <= src_stride_reg;
                        5'd7: wb_dat_o <= dst_stride_reg;
                        5'd8: wb_dat_o <= pattern_reg;
                        default: wb_dat_o <= 32'd0;
                    endcase
                end
            end
        end
    end

    // Start trigger detection
    wire start_trigger = wb_cyc_i && wb_stb_i && wb_we_i && 
                        (wb_adr_i[4:0] == 5'd0) && wb_dat_i[CTRL_START] && !draw_busy;

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
            x_count <= 32'h0;
            y_count <= 32'h0;
            words_per_line <= 32'h0;
            read_data <= 32'h0;
            current_src_addr <= 32'h0;
            current_dst_addr <= 32'h0;
            line_src_start <= 32'h0;
            line_dst_start <= 32'h0;
            work_src_addr <= 32'h0;
            work_dst_addr <= 32'h0;
            work_width <= 32'h0;
            work_height <= 32'h0;
            work_src_stride <= 32'h0;
            work_dst_stride <= 32'h0;
            work_pattern <= 32'h0;
            work_mode <= 1'b0;
            work_fill <= 1'b0;
        end else begin
            case (state)
                ST_IDLE: begin
                    draw_busy <= 1'b0;
                    m_cyc_o <= 1'b0;
                    m_stb_o <= 1'b0;
                    m_we_o <= 1'b0;
                    
                    if (start_trigger) begin
                        // Latch working parameters
                        work_src_addr <= src_addr_reg;
                        work_dst_addr <= dst_addr_reg;
                        work_width <= width_reg;
                        work_height <= height_reg;
                        work_src_stride <= (src_stride_reg == 0) ? 32'd64 : src_stride_reg;
                        work_dst_stride <= (dst_stride_reg == 0) ? 32'd64 : dst_stride_reg;
                        work_pattern <= pattern_reg;
                        work_mode <= mode_reg;
                        work_fill <= fill_reg;
                        
                        // Initialize counters
                        x_count <= 32'h0;
                        y_count <= 32'h0;
                        words_per_line <= (width_reg + 31) >> 5;  // Convert pixels to words
                        
                        // Initialize addresses
                        current_src_addr <= src_addr_reg;
                        current_dst_addr <= dst_addr_reg;
                        line_src_start <= src_addr_reg;
                        line_dst_start <= dst_addr_reg;
                        
                        draw_busy <= 1'b1;
                        
                        // Start operation
                        if (fill_reg) begin
                            read_data <= pattern_reg;
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
                    m_adr_o <= VRAM_BASE + current_src_addr;
                    state <= ST_WAIT_READ;
                end

                ST_WAIT_READ: begin
                    if (m_ack_i) begin
                        read_data <= m_dat_i;
                        m_we_o <= 1'b1;
                        m_adr_o <= VRAM_BASE + current_dst_addr;
                        m_dat_o <= m_dat_i;
                        state <= ST_WRITE;
                    end
                end

                ST_WRITE: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b1;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + current_dst_addr;
                    m_dat_o <= read_data;
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
                    if (x_count + 1 >= words_per_line) begin
                        // End of line
                        x_count <= 32'h0;
                        y_count <= y_count + 1;
                        
                        if (y_count + 1 >= work_height) begin
                            // Operation complete
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            // Move to start of next line
                            line_src_start <= line_src_start + work_src_stride;
                            line_dst_start <= line_dst_start + work_dst_stride;
                            current_src_addr <= line_src_start + work_src_stride;
                            current_dst_addr <= line_dst_start + work_dst_stride;
                            
                            if (work_fill) begin
                                state <= ST_WRITE;
                            end else begin
                                state <= ST_READ;
                            end
                        end
                    end else begin
                        // Continue same line - advance to next word
                        x_count <= x_count + 1;
                        current_src_addr <= current_src_addr + 4;
                        current_dst_addr <= current_dst_addr + 4;
                        
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
