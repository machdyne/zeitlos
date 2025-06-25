/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Wishbone bus arbiter
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

    // Master 1 (GPU)
    input  wire [31:0] m1_adr_i,
    input  wire [31:0] m1_dat_i,
    output reg  [31:0] m1_dat_o,
    input  wire        m1_we_i,
    input  wire [3:0]  m1_sel_i,
    input  wire        m1_stb_i,
    input  wire        m1_cyc_i,
    output reg         m1_ack_o,

    // Shared slave interface
    output reg  [31:0] s_adr_o,
    output reg  [31:0] s_dat_o,
    input  wire [31:0] s_dat_i,
    output reg         s_we_o,
    output reg  [3:0]  s_sel_o,
    output reg         s_stb_o,
    output reg         s_cyc_o,
    input  wire        s_ack_i,

    // Current bus master: 0 = CPU, 1 = GPU
    output reg         master
);

    // Master requests
    wire m0_req = m0_cyc_i && m0_stb_i;
    wire m1_req = m1_cyc_i && m1_stb_i;

    // State machine for proper arbitration
    reg [1:0] state;
    localparam IDLE = 2'd0,
               CPU_ACTIVE = 2'd1,
               GPU_ACTIVE = 2'd2;

    // Arbitration state machine
    always @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            master <= 1'b0;
        end else begin
            case (state)
                IDLE: begin
                    // Priority: GPU first (for atomic read-modify-write), then CPU
                    if (m1_req) begin
                        state <= GPU_ACTIVE;
                        master <= 1'b1;
                    end else if (m0_req) begin
                        state <= CPU_ACTIVE;
                        master <= 1'b0;
                    end
                end
                
                CPU_ACTIVE: begin
                    // Stay active while CPU needs the bus
                    if (!m0_cyc_i) begin
                        state <= IDLE;
                    end
                end
                
                GPU_ACTIVE: begin
                    // Stay active while GPU needs the bus
                    if (!m1_cyc_i) begin
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
            
            GPU_ACTIVE: begin
                // Route GPU to slave
                s_adr_o = m1_adr_i;
                s_dat_o = m1_dat_i;
                s_we_o  = m1_we_i;
                s_sel_o = m1_sel_i;
                s_stb_o = m1_stb_i;
                s_cyc_o = m1_cyc_i;
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
            m0_ack_o <= 1'b0;
            m1_ack_o <= 1'b0;
        end else begin
            // Route slave responses back to active master
            case (state)
                CPU_ACTIVE: begin
                    m0_dat_o <= s_dat_i;
                    m0_ack_o <= s_ack_i;
                    m1_dat_o <= 32'h00000000;
                    m1_ack_o <= 1'b0;
                end
                
                GPU_ACTIVE: begin
                    m1_dat_o <= s_dat_i;
                    m1_ack_o <= s_ack_i;
                    m0_dat_o <= 32'h00000000;
                    m0_ack_o <= 1'b0;
                end
                
                default: begin  // IDLE
                    m0_dat_o <= 32'h00000000;
                    m1_dat_o <= 32'h00000000;
                    m0_ack_o <= 1'b0;
                    m1_ack_o <= 1'b0;
                end
            endcase
        end
    end

endmodule
