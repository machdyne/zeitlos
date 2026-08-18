/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * 32-bit dual-port VRAM
 */

module vram_wb #()
(
   input wb_clk_i,
   input wb_rst_i,
   input [14:0] wb_adr_i,
   input [31:0] wb_dat_i,
   output reg [31:0] wb_dat_o,
   input wb_we_i,
   input [3:0] wb_sel_i,
   input wb_stb_i,
   output reg wb_ack_o,
   input wb_cyc_i,

   input [14:0] gb_adr_i,
   output reg [31:0] gb_dat_o
);

    // 640 * 480 / 32 = 9600 words -- native resolution. GPU_PIXEL_DOUBLE
    // (rendering at half this and doubling both axes on scanout, the
    // scheme that used to let a 512x384 framebuffer fill a 1024x768
    // signal) is gone -- no board defines it anymore, and
    // rtl/gpu/gpu_video.v no longer has any conditional branch on it
    // either, so keeping one here too would just be a latent
    // VRAM-size mismatch waiting to happen if it were ever redefined.
    reg [31:0] vram [0:9599];

    wire wb_active = wb_cyc_i && wb_stb_i;

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            wb_ack_o <= 1'b0;
            wb_dat_o <= 32'b0;
        end else begin
            // Default: no acknowledge
            wb_ack_o <= 1'b0;

            if (wb_active) begin
                // Always provide read data
                wb_dat_o <= vram[wb_adr_i];
                
                // Perform write if requested
                if (wb_we_i) begin
                    if (wb_sel_i[0]) vram[wb_adr_i][7:0] <= wb_dat_i[7:0];
                    if (wb_sel_i[1]) vram[wb_adr_i][15:8] <= wb_dat_i[15:8];
                    if (wb_sel_i[2]) vram[wb_adr_i][23:16] <= wb_dat_i[23:16];
                    if (wb_sel_i[3]) vram[wb_adr_i][31:24] <= wb_dat_i[31:24];
                end
                
                // Acknowledge the transaction
                wb_ack_o <= 1'b1;
            end
        end
    end

    // Graphics port (for display controller)
    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            gb_dat_o <= 32'b0;
        end else begin
            gb_dat_o <= vram[gb_adr_i];
        end
    end

endmodule
