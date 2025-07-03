/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * 3-Port Wishbone bus arbiter (CPU, GPU Raster, GPU Blit)
 * 
 */

module wb_arbiter (
    input  wire        clk,
    input  wire        rst,

    // Master 0 (CPU)
    input  wire [31:0] m0_adr_i,
    input  wire [31:0] m0_dat_i,
    output reg  [31:0] m0_dat_o,
    input  wire        m0_we_i,
    input  wire [3:0]  m0_sel_i,
    input  wire        m0_stb_i,
    input  wire        m0_cyc_i,
    output reg         m0_ack_o,

    // Master 1 (GPU Raster)
    input  wire [31:0] m1_adr_i,
    input  wire [31:0] m1_dat_i,
    output reg  [31:0] m1_dat_o,
    input  wire        m1_we_i,
    input  wire [3:0]  m1_sel_i,
    input  wire        m1_stb_i,
    input  wire        m1_cyc_i,
    output reg         m1_ack_o,

    // Master 2 (GPU Blit)
    input  wire [31:0] m2_adr_i,
    input  wire [31:0] m2_dat_i,
    output reg  [31:0] m2_dat_o,
    input  wire        m2_we_i,
    input  wire [3:0]  m2_sel_i,
    input  wire        m2_stb_i,
    input  wire        m2_cyc_i,
    output reg         m2_ack_o,

    // Shared slave interface
    output reg  [31:0] s_adr_o,
    output reg  [31:0] s_dat_o,
    input  wire [31:0] s_dat_i,
    output reg         s_we_o,
    output reg  [3:0]  s_sel_o,
    output reg         s_stb_o,
    output reg         s_cyc_o,
    input  wire        s_ack_i,

    // Current bus master: 0 = CPU, 1 = GPU Raster, 2 = GPU Blit
    output reg  [1:0]  master
);

    // Master requests
    wire m0_req = m0_cyc_i && m0_stb_i;
    wire m1_req = m1_cyc_i && m1_stb_i;
    wire m2_req = m2_cyc_i && m2_stb_i;

    // State machine for proper arbitration
    reg [1:0] state;
    localparam IDLE = 2'd0,
               CPU_ACTIVE = 2'd1,
               GPU_RASTER_ACTIVE = 2'd2,
               GPU_BLIT_ACTIVE = 2'd3;

    // Arbitration state machine
    // Priority: GPU Blit > GPU Raster > CPU (for performance)
    always @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            master <= 2'b00;
        end else begin
            case (state)
                IDLE: begin
                    if (m2_req) begin
                        state <= GPU_BLIT_ACTIVE;
                        master <= 2'b10;
                    end else if (m1_req) begin
                        state <= GPU_RASTER_ACTIVE;
                        master <= 2'b01;
                    end else if (m0_req) begin
                        state <= CPU_ACTIVE;
                        master <= 2'b00;
                    end
                end
                
                CPU_ACTIVE: begin
                    // Stay active while CPU needs the bus
                    if (!m0_cyc_i) begin
                        state <= IDLE;
                    end
                end
                
                GPU_RASTER_ACTIVE: begin
                    // Stay active while GPU Raster needs the bus
                    if (!m1_cyc_i) begin
                        state <= IDLE;
                    end
                end

                GPU_BLIT_ACTIVE: begin
                    // Stay active while GPU Blit needs the bus
                    if (!m2_cyc_i) begin
                        state <= IDLE;
                    end
                end
                
                default: begin
                    state <= IDLE;
                end
            endcase
        end
    end

    // Master selection and bus routing
    always @(*) begin
        // Default values
        s_adr_o = 32'h00000000;
        s_dat_o = 32'h00000000;
        s_we_o = 1'b0;
        s_sel_o = 4'b0000;
        s_stb_o = 1'b0;
        s_cyc_o = 1'b0;

        case (state)
            CPU_ACTIVE: begin
                // Route CPU to slave
                s_adr_o = m0_adr_i;
                s_dat_o = m0_dat_i;
                s_we_o  = m0_we_i;
                s_sel_o = m0_sel_i;
                s_stb_o = m0_stb_i;
                s_cyc_o = m0_cyc_i;
            end
            
            GPU_RASTER_ACTIVE: begin
                // Route GPU Raster to slave
                s_adr_o = m1_adr_i;
                s_dat_o = m1_dat_i;
                s_we_o  = m1_we_i;
                s_sel_o = m1_sel_i;
                s_stb_o = m1_stb_i;
                s_cyc_o = m1_cyc_i;
            end

            GPU_BLIT_ACTIVE: begin
                // Route GPU Blit to slave
                s_adr_o = m2_adr_i;
                s_dat_o = m2_dat_i;
                s_we_o  = m2_we_i;
                s_sel_o = m2_sel_i;
                s_stb_o = m2_stb_i;
                s_cyc_o = m2_cyc_i;
            end
            
            default: begin
                // IDLE - no connections
            end
        endcase
    end

    // Response routing - registered for better timing
    always @(posedge clk) begin
        if (rst) begin
            m0_dat_o <= 32'h00000000;
            m1_dat_o <= 32'h00000000;
            m2_dat_o <= 32'h00000000;
            m0_ack_o <= 1'b0;
            m1_ack_o <= 1'b0;
            m2_ack_o <= 1'b0;
        end else begin
            // Route slave responses back to active master
            case (state)
                CPU_ACTIVE: begin
                    m0_dat_o <= s_dat_i;
                    m0_ack_o <= s_ack_i;
                    m1_dat_o <= 32'h00000000;
                    m1_ack_o <= 1'b0;
                    m2_dat_o <= 32'h00000000;
                    m2_ack_o <= 1'b0;
                end
                
                GPU_RASTER_ACTIVE: begin
                    m1_dat_o <= s_dat_i;
                    m1_ack_o <= s_ack_i;
                    m0_dat_o <= 32'h00000000;
                    m0_ack_o <= 1'b0;
                    m2_dat_o <= 32'h00000000;
                    m2_ack_o <= 1'b0;
                end

                GPU_BLIT_ACTIVE: begin
                    m2_dat_o <= s_dat_i;
                    m2_ack_o <= s_ack_i;
                    m0_dat_o <= 32'h00000000;
                    m0_ack_o <= 1'b0;
                    m1_dat_o <= 32'h00000000;
                    m1_ack_o <= 1'b0;
                end
                
                default: begin  // IDLE
                    m0_dat_o <= 32'h00000000;
                    m1_dat_o <= 32'h00000000;
                    m2_dat_o <= 32'h00000000;
                    m0_ack_o <= 1'b0;
                    m1_ack_o <= 1'b0;
                    m2_ack_o <= 1'b0;
                end
            endcase
        end
    end

endmodule
