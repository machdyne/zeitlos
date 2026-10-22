/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * 32-bit dual-port VRAM
 */

module vram_wb
#(
    // 9600 words: one 640x480x1bpp sheet, the desktop and four
    // game-mode bitplanes. GAME_PLANES 6 adds half a second sheet
    // (14400, rows 480..719) and 8 a whole one (19200, rows 480..959):
    // rtl/sysctl.v sets it, docs/color.md "More than four planes".
    //
    // How yosys builds the array (0.70; 0.33 the same) depends on its
    // depth. 9600 words: 20 DP16KD several deep, and a read
    // multiplexer on each port (about 220 LUT4). 14400: the 16,384 x 1 shape, 32 DP16KD side by side
    // and no multiplexer at all, two blocks more than the tightest
    // packing and about 210 LUT4 fewer than 9600 -- on the 25F boards,
    // whose LUTs are the scarcer resource, the better trade. Packings
    // of 30 were tried (banks of 1,024 words, a 14,336 + 64 split):
    // yosys builds them with 15:1 multiplexers, 1,000 LUT4 and more.
    // 19200: 40 DP16KD and the same multiplexer as 9600.
    parameter integer WORDS = 9600
)
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
    //
    // -- no_rw_check: 40 block RAMs down to 20 on ECP5 --
    //
    // 300Kbit needs about 20 DP16KD. Without this attribute yosys
    // builds TWO full copies, 40 blocks -- 40 of Lakritz's 56. The
    // cause is read-during-write semantics, not size. As written, a
    // graphics-port read on the same edge as a CPU write to the same
    // word returns the OLD data. A DP16KD only guarantees that for a
    // read on the SAME physical port as the write (READBEFOREWRITE),
    // so yosys keeps every read on the write's port -- and two readers
    // on one port means two copies (memory_libmap's debug output:
    // "replicates (for ports): 2").
    //
    // no_rw_check tells yosys that collision result is don't-care.
    // The CPU's read and write then share one port and the graphics
    // port gets the other: one copy. Measured on a full Lakritz build:
    // 55 -> 35 DP16KD, Fmax unchanged (the critical path is in the
    // blitter either way).
    //
    // What it gives up: on that exact collision the hardware returns
    // unknown data on the graphics port (ECP5 memory guide,
    // FPGA-TN-02204, WRITEMODE: "the read data may be unknown").
    // Stored data is never at risk -- that needs two WRITERS, and this
    // RAM has one. The only graphics-port reader is gpu_video's
    // scanline refill, so the worst case is one 32-pixel word of one
    // line wrong for one frame, where before it was stale for one
    // frame. CPU-side reads are unaffected: they share the write's
    // port, in READBEFOREWRITE, exactly as before.
    //
    // Simulation ignores the attribute; an older yosys without it
    // ignores it too and simply uses 40 blocks again.
    (* no_rw_check *)
    reg [31:0] vram [0:WORDS-1];

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
