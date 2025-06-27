/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Memory Translation Unit
 *
 */

module wb_mtu (
    input wire clk_i,
    input wire rst_i,
    
    // Address translation
    input wire [31:0] addr_in,
    output wire [31:0] addr_out,
    
    // Wishbone slave interface for configuration
    input wire [31:0] cfg_adr_i,
    input wire [31:0] cfg_dat_i,
    output reg [31:0] cfg_dat_o,
    input wire [3:0] cfg_sel_i,
    input wire cfg_we_i,
    input wire cfg_stb_i,
    input wire cfg_cyc_i,
    output reg cfg_ack_o
);

    // Translation register
    reg [31:0] translation_base;
    
    // Address translation constants
    parameter TRANSLATE_ADDR = 32'h8000_0000;
    parameter TRANSLATE_MASK = 32'hF000_0000; // Match upper 4 bits
    
    // Configuration register access
    always @(posedge clk_i) begin
        if (rst_i) begin
            translation_base <= 32'h0000_0000;
            cfg_ack_o <= 1'b0;
            cfg_dat_o <= 32'h0000_0000;
        end else begin
            cfg_ack_o <= 1'b0;
            
            if (cfg_cyc_i && cfg_stb_i && !cfg_ack_o) begin
                cfg_ack_o <= 1'b1;
                
                if (cfg_we_i) begin
                    // Write to translation register
                    if (cfg_sel_i[0]) translation_base[7:0]   <= cfg_dat_i[7:0];
                    if (cfg_sel_i[1]) translation_base[15:8]  <= cfg_dat_i[15:8];
                    if (cfg_sel_i[2]) translation_base[23:16] <= cfg_dat_i[23:16];
                    if (cfg_sel_i[3]) translation_base[31:24] <= cfg_dat_i[31:24];
                end else begin
                    // Read translation register
                    cfg_dat_o <= translation_base;
                end
            end
        end
    end
    
    // Address translation logic (combinatorial)
    wire address_needs_translation;
    wire [31:0] offset_address;
    
    assign address_needs_translation = (translation_base != 32'h0000_0000) && 
        ((addr_in & TRANSLATE_MASK) == (TRANSLATE_ADDR & TRANSLATE_MASK));
    
    assign offset_address = addr_in & ~TRANSLATE_MASK; // Keep lower bits as offset

    assign addr_out = address_needs_translation ? 
        (translation_base + offset_address) : addr_in;

endmodule
