/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * High-performance word-level blitter with intelligent clipping support
 * for font rendering and precise graphics.
 *
 * Modes (selected by CTRL_FILL/CTRL_GLYPH/CTRL_SRCMEM):
 *   - fill: word-parallel rectangle fill from BLIT_PATTERN.
 *   - glyph: blits one font glyph from glyph memory (see
 *     rtl/mem/glyph.v) into the framebuffer, with a solid foreground/
 *     background per pixel -- proper terminal-cell semantics, not a
 *     transparent overlay. Unclipped by design: the caller (software)
 *     is expected to only trigger this for glyphs that are already
 *     fully on-screen, and fall back to software rendering for glyphs
 *     that would need partial clipping (e.g. right at a window edge).
 *     See docs/window_manager.md, "hardware glyph blitting".
 *   - copy (CTRL_FILL=0): copies a 1bpp bitmap into the framebuffer,
 *     with arbitrary bit alignment between source and destination. One
 *     engine, two possible sources, chosen by CTRL_SRCMEM:
 *
 *       CTRL_SRCMEM=1  source is MAIN MEMORY, read through the s_*
 *                      port. This is what makes an offscreen document
 *                      buffer practical (sw/apps/draw).
 *       CTRL_SRCMEM=0  source is VRAM, read through the same m_* port
 *                      the destination uses -- offscreen VRAM to
 *                      on-screen VRAM, i.e. sprites, once there is
 *                      spare VRAM to keep them in.
 *
 *     Both share the shifter, the masking, the clipping and the state
 *     machine; the ONLY difference is which port issues the source
 *     read. VRAM-to-VRAM copy was a no-op stub from the day this module
 *     was written (ST_WRITE wrote the destination straight back); it
 *     works now as a side effect of building the memory path properly.
 *     See docs/gpu_blitter.md, "Copy modes".
 *
 * -- two master ports, and why --
 *
 * The framebuffer master (m_*) reaches VRAM through the 3-way arbiter
 * in rtl/arbiter_vram.v, which is wired only to VRAM. Main memory is on
 * the separate main bus, which the CPU owns. So a main-memory source
 * needs a SECOND master port (s_*) on that bus, and rtl/arbiter_main.v
 * puts the blitter and the CPU on it together. A VRAM source needs no
 * such thing, which is why it reuses m_*.
 *
 * The two ports are never active at the same time. Every s_* read
 * happens with m_cyc_o deasserted, and every m_* access happens with
 * s_cyc_o deasserted. That is not merely tidy: both arbiters release a
 * grant only when the winning master drops cyc, so a blitter that held
 * VRAM while waiting for main memory -- with the CPU holding main
 * memory while waiting for VRAM -- would deadlock the machine outright.
 * Keeping the ports strictly alternating makes that cycle impossible to
 * form.
 */

module gpu_blit_wb #(
    parameter GLYPH_ADDR_WIDTH = 12   // must match glyph_mem's ADDR_WIDTH
)
(
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

    // Wishbone MASTER interface (framebuffer access)
    output reg         m_cyc_o,
    output reg         m_stb_o,
    output reg         m_we_o,
    output reg  [3:0]  m_sel_o,
    output reg  [31:0] m_adr_o,
    output reg  [31:0] m_dat_o,
    input  wire [31:0] m_dat_i,
    input  wire        m_ack_i,

    // Wishbone MASTER interface #2 (main memory source reads, memory
    // copy mode only). Read-only: we_o is tied low and dat_o doesn't
    // exist, because nothing in this module ever writes to main memory.
    // Idle (cyc/stb low) in every other mode, so a bitstream that never
    // uses memory copy behaves exactly as it did before this port
    // existed.
    output reg         s_cyc_o,
    output reg         s_stb_o,
    output wire        s_we_o,
    output wire [3:0]  s_sel_o,
    output reg  [31:0] s_adr_o,
    input  wire [31:0] s_dat_i,
    input  wire        s_ack_i,

    // direct (non-Wishbone) glyph memory read port -- see rtl/mem/glyph.v.
    // registered/synchronous: glyph_data_i is valid one cycle after
    // glyph_addr_o is presented.
    output reg  [GLYPH_ADDR_WIDTH-1:0] glyph_addr_o,
    input  wire [7:0]                  glyph_data_i,

    output wire        busy
);

    localparam VRAM_BASE = 32'h20000000;
    localparam SCREEN_STRIDE = 80; // 640 pixels / 8 = 80 bytes per line

    // Control bits
    localparam CTRL_START = 0;
    localparam CTRL_FILL  = 1;  // 0=copy, 1=fill (ignored in glyph mode)
    localparam CTRL_CLIP  = 2;  // 0=no clipping, 1=enable clipping (ignored in glyph mode)
    localparam CTRL_GLYPH = 3;  // 0=normal fill/copy, 1=glyph blit
    localparam CTRL_SRCMEM = 4; // 1=copy source is main memory (needs CTRL_FILL=0)

    // Configuration registers
    reg [31:0] dst_x_reg, dst_y_reg, width_reg, height_reg;
    reg [31:0] pattern_reg;
    reg fill_reg, clip_enable_reg, glyph_reg, srcmem_reg;
    reg [31:0] glyph_addr_reg, glyph_w_reg, glyph_h_reg, fg_color_reg, bg_color_reg;

    // -- memory copy source registers --
    //
    // src_addr_reg is a PHYSICAL byte address, word aligned. The
    // blitter is its own bus master and does not go through the MTU
    // (rtl/mtu.v), so a virtual 0x8000_0000 app address means nothing
    // here -- software translates before writing this. See
    // z_fb_hw_blit_mem() in sw/common/zgfx.c.
    //
    // src_shift_reg[4:0] is the bit offset, within the word at
    // src_addr_reg, of the pixel that lands on bit 0 of the FIRST
    // destination word. src_shift_reg[8] ("prime with zero") covers the
    // one case where that pixel would fall in the word BEFORE the
    // source buffer: see ST_MEM_ROW.
    reg [31:0] src_addr_reg, src_stride_reg, src_shift_reg;

    // State machine
    reg [4:0] state;
    localparam ST_IDLE = 5'd0, ST_CLIP = 5'd1, ST_READ = 5'd2,
               ST_WAIT_READ = 5'd3, ST_WRITE = 5'd4, ST_WAIT_WRITE = 5'd5, ST_NEXT = 5'd6;
    localparam ST_GLYPH_SETUP = 5'd7, ST_GLYPH_FETCH = 5'd8, ST_GLYPH_FETCH_WAIT = 5'd9,
               ST_GLYPH_READ_LO = 5'd10, ST_GLYPH_WAIT_READ_LO = 5'd11,
               ST_GLYPH_WRITE_LO = 5'd12, ST_GLYPH_WAIT_WRITE_LO = 5'd13,
               ST_GLYPH_READ_HI = 5'd14, ST_GLYPH_WAIT_READ_HI = 5'd15,
               ST_GLYPH_WRITE_HI = 5'd16, ST_GLYPH_WAIT_WRITE_HI = 5'd17,
               ST_GLYPH_ROW_DONE = 5'd18, ST_GLYPH_FETCH_WAIT2 = 5'd19,
               ST_GLYPH_HI_SETTLE1 = 5'd20, ST_GLYPH_HI_SETTLE2 = 5'd21;
    localparam ST_MEM_ROW = 5'd22, ST_MEM_PRIME = 5'd23,
               ST_MEM_PRIME_WAIT = 5'd24, ST_MEM_READ = 5'd25,
               ST_MEM_READ_WAIT = 5'd26;
    // One idle cycle between latching work_* and clipping against it,
    // so the registered rect_x_end/rect_y_end above are settled. See
    // their declaration.
    localparam ST_CLIP_CALC = 5'd27;

    // Operation variables
    reg [31:0] work_dst_x, work_dst_y, work_width, work_height, work_pattern;
    reg work_fill, work_clip, work_glyph;
    reg [31:0] work_glyph_addr, work_glyph_w, work_glyph_h, work_fg, work_bg;
    reg work_srcmem, work_src_prime;
    reg [31:0] work_src_addr, work_src_stride;
    reg [4:0]  work_src_shift;
    reg draw_busy;

    // -- memory copy iteration state --
    // mem_row_addr: byte address of the current row's first source word
    // mem_next_addr: read pointer walking along that row
    // src_prev: the previously fetched source word, one half of the
    //   64-bit window the shifter slides along (see mem_blend below)
    // src_word_out: the assembled destination word, held across the
    //   framebuffer read-modify-write that follows
    reg [31:0] mem_row_addr, mem_next_addr, src_prev, src_word_out;

    // Clipped rectangle coordinates
    reg [31:0] clip_x, clip_y, clip_width, clip_height;
    reg [31:0] clip_x_end, clip_y_end;

    // Word-level iteration (fill/copy path)
    reg [31:0] current_line, current_word_in_line;
    reg [31:0] words_per_line, total_lines;
    reg [31:0] line_start_addr, current_word_addr;
    reg [31:0] left_word_x, right_word_x;
    reg [31:0] left_mask, right_mask;
    reg [31:0] read_data;

    // Glyph iteration state
    reg [4:0]  g_bit_offset;    // work_dst_x mod 32 -- bit position of the glyph's leftmost pixel within its word
    reg [31:0] g_line_addr;     // current row's line-start byte address (word containing dst_x)
    reg [31:0] g_row;           // current row within the glyph, 0..work_glyph_h-1
    reg [7:0]  g_glyph_byte;    // glyph row byte, as fetched from glyph memory (still MSB-first at this point)

    // Clipping calculations (fill/copy path, unchanged) -- 640x480
    // native resolution now, was 512x384
    wire [31:0] screen_clip_x_end = 32'd640;
    wire [31:0] screen_clip_y_end = 32'd480;
    // Registered, to split the clip chain.
    //
    // rect_x_end -> final_x_end -> final_width -> word_span -> the
    // ST_CLIP decision that gates m_stb_o was six chained 32-bit
    // operations, and it was the whole SOC's critical path at 48MHz.
    // Breaking it here is worth about +15% Fmax at zero area cost.
    //
    // THIS TRAILS work_dst_x BY ONE CYCLE, which is why ST_CLIP_CALC
    // exists below. work_dst_x is not written by the wishbone slave --
    // it is latched from dst_x_reg in ST_IDLE on the same edge that
    // start_trigger fires -- so at the state immediately after
    // ST_IDLE these registers still hold the PREVIOUS operation's
    // sum. Going straight to ST_CLIP clipped every blit against stale
    // coordinates, which hung the state machine and blanked the
    // screen.
    //
    // ST_IDLE's own comment on work_fill/work_clip/work_glyph warns
    // about exactly this hazard one level down. Same trap, same
    // module.
    reg [31:0] rect_x_end;
    reg [31:0] rect_y_end;
    always @(posedge clk) begin
        rect_x_end <= work_dst_x + work_width;
        rect_y_end <= work_dst_y + work_height;
    end

    wire [31:0] final_x = work_dst_x;
    wire [31:0] final_y = work_dst_y;
    wire [31:0] final_x_end = (rect_x_end > screen_clip_x_end) ? screen_clip_x_end : rect_x_end;
    wire [31:0] final_y_end = (rect_y_end > screen_clip_y_end) ? screen_clip_y_end : rect_y_end;
    wire [31:0] final_width = final_x_end - final_x;
    wire [31:0] final_height = final_y_end - final_y;

    wire [31:0] left_word_boundary = (final_x >> 5) << 5;
    wire [31:0] right_word_boundary = ((final_x_end + 31) >> 5) << 5;
    wire [31:0] word_span_width = right_word_boundary - left_word_boundary;
    wire [31:0] word_span_words = word_span_width >> 5;

    wire [31:0] left_pixel_start = final_x - left_word_boundary;
    wire [31:0] right_pixel_end = final_x_end - ((final_x_end >> 5) << 5);
    wire [31:0] left_pixel_mask = (32'hFFFFFFFF << left_pixel_start);
    wire [31:0] right_pixel_mask = (right_pixel_end == 0) ? 32'hFFFFFFFF :
                                   (32'hFFFFFFFF >> (32 - right_pixel_end));

    // -- glyph blit combinational logic --

    // bit-reverse: framebuffer words have increasing x = increasing bit
    // position (bit N of a word = pixel column N within that word --
    // see zgfx.c's z_fb_set_pixel), but glyph bytes are stored
    // MSB-first (bit 7 = leftmost pixel, bit (7-k) = column k -- see
    // sw/common/zfont_data.c). Opposite conventions, so reverse the
    // byte here: after this, g_byte_rev[k] = pixel column k, matching
    // the framebuffer's own bit order.
    wire [7:0] g_byte_rev = {
        g_glyph_byte[0], g_glyph_byte[1], g_glyph_byte[2], g_glyph_byte[3],
        g_glyph_byte[4], g_glyph_byte[5], g_glyph_byte[6], g_glyph_byte[7]
    };

    // does this glyph's row span two words? (only possible when
    // g_bit_offset + width > 32, since glyphs are always <= 8px wide)
    wire [5:0] g_offset_plus_w = {1'b0, g_bit_offset} + work_glyph_w[5:0];
    wire g_straddle = (g_offset_plus_w > 6'd32);

    // low-word-relative bit pattern/cell mask, before the word-crossing split
    wire [31:0] g_glyph_bits = ({24'h0, g_byte_rev}) & ((32'h1 << work_glyph_w) - 32'h1);
    wire [31:0] g_cell_bits  = (32'h1 << work_glyph_w) - 32'h1;

    wire [63:0] g_shifted_bits = ({32'h0, g_glyph_bits}) << g_bit_offset;
    wire [63:0] g_shifted_cell = ({32'h0, g_cell_bits})  << g_bit_offset;

    wire [31:0] g_bits_lo = g_shifted_bits[31:0];
    wire [31:0] g_bits_hi = g_shifted_bits[63:32];
    wire [31:0] g_cell_lo = g_shifted_cell[31:0];
    wire [31:0] g_cell_hi = g_shifted_cell[63:32];

    wire [31:0] g_fg_word = {32{work_fg[0]}};
    wire [31:0] g_bg_word = {32{work_bg[0]}};

    // -- memory copy combinational logic --

    // The shifter. Destination word k is assembled from source words k-1
    // and k, so that bit 0 of the destination word is bit
    // work_src_shift of source word k-1. Sliding a 64-bit window one
    // word at a time is what lets an arbitrary source-to-destination
    // bit offset cost the same as an aligned one.
    //
    // The shift == 0 case is separated out rather than folded into the
    // general expression because (32 - shift) is then 32, and a 32-bit
    // value shifted by 32 is zero here -- the general form would
    // silently drop the whole high half instead of passing the word
    // through.
    // Which port the source read came back on. Everything downstream of
    // this pair -- the shifter, the masking, the state machine -- is
    // identical for a main-memory source and a VRAM source, so the mode
    // difference is confined to these two wires and to which port the
    // read states assert.
    wire [31:0] mem_src_dat = work_srcmem ? s_dat_i : m_dat_i;
    wire        mem_src_ack = work_srcmem ? s_ack_i : m_ack_i;

    wire [5:0] mem_shift_hi = 6'd32 - {1'b0, work_src_shift};
    wire [31:0] mem_blend = (work_src_shift == 5'd0) ? src_prev :
                            ((src_prev >> work_src_shift) |
                             (mem_src_dat << mem_shift_hi));

    // Which bits of the destination word this operation may modify.
    // Deliberately a SEPARATE expression from the equivalent selection
    // inlined in ST_WRITE's fill branch: that branch is load-bearing,
    // long-tested code, and rewriting it to share this wire would put
    // every existing fill at risk to save a few lines.
    wire [31:0] mem_edge_mask =
        (work_clip && words_per_line > 1) ?
            ((current_word_in_line == 0) ? left_mask :
             (current_word_in_line == words_per_line - 1) ? right_mask :
             32'hFFFFFFFF) :
        (work_clip && words_per_line == 1) ? (left_mask & right_mask) :
        32'hFFFFFFFF;

    // read-only master: tie off the write-side signals rather than
    // leaving them undriven
    assign s_we_o = 1'b0;
    assign s_sel_o = 4'b1111;

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
            glyph_reg <= 1'b0;
            srcmem_reg <= 1'b0;
            glyph_addr_reg <= 32'h0;
            glyph_w_reg <= 32'h0;
            glyph_h_reg <= 32'h0;
            fg_color_reg <= 32'h1;
            bg_color_reg <= 32'h0;
            src_addr_reg <= 32'h0;
            src_stride_reg <= 32'h0;
            src_shift_reg <= 32'h0;
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
                            glyph_reg <= wb_dat_i[CTRL_GLYPH];
                            srcmem_reg <= wb_dat_i[CTRL_SRCMEM];
                        end
                        4'd1: ; // STATUS - read only
                        4'd2: dst_x_reg <= wb_dat_i;
                        4'd3: dst_y_reg <= wb_dat_i;
                        4'd4: width_reg <= wb_dat_i;
                        4'd5: height_reg <= wb_dat_i;
                        4'd6: pattern_reg <= wb_dat_i;
                        4'd7: glyph_addr_reg <= wb_dat_i;
                        4'd8: glyph_w_reg <= wb_dat_i;
                        4'd9: glyph_h_reg <= wb_dat_i;
                        4'd10: fg_color_reg <= wb_dat_i;
                        4'd11: bg_color_reg <= wb_dat_i;
                        4'd12: src_addr_reg <= wb_dat_i;
                        4'd13: src_stride_reg <= wb_dat_i;
                        4'd14: src_shift_reg <= wb_dat_i;
                        default: ;
                    endcase
                end else begin
                    case (wb_adr_i[3:0])
                        4'd0: wb_dat_o <= {27'h0, srcmem_reg, glyph_reg, clip_enable_reg, fill_reg, 1'b0};
                        4'd1: wb_dat_o <= {31'h0, draw_busy};
                        4'd2: wb_dat_o <= dst_x_reg;
                        4'd3: wb_dat_o <= dst_y_reg;
                        4'd4: wb_dat_o <= width_reg;
                        4'd5: wb_dat_o <= height_reg;
                        4'd6: wb_dat_o <= pattern_reg;
                        4'd7: wb_dat_o <= glyph_addr_reg;
                        4'd8: wb_dat_o <= glyph_w_reg;
                        4'd9: wb_dat_o <= glyph_h_reg;
                        4'd10: wb_dat_o <= fg_color_reg;
                        4'd11: wb_dat_o <= bg_color_reg;
                        4'd12: wb_dat_o <= src_addr_reg;
                        4'd13: wb_dat_o <= src_stride_reg;
                        4'd14: wb_dat_o <= src_shift_reg;
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
            glyph_addr_o <= {GLYPH_ADDR_WIDTH{1'b0}};
            s_cyc_o <= 1'b0;
            s_stb_o <= 1'b0;
            s_adr_o <= 32'd0;
            state <= ST_IDLE;
        end else begin
            case (state)
                ST_IDLE: begin
                    draw_busy <= 1'b0;
                    m_cyc_o <= 1'b0;
                    m_stb_o <= 1'b0;
                    m_we_o <= 1'b0;
                    s_cyc_o <= 1'b0;
                    s_stb_o <= 1'b0;

                    if (start_trigger) begin
                        // Latch parameters
                        work_dst_x <= dst_x_reg;
                        work_dst_y <= dst_y_reg;
                        work_width <= width_reg;
                        work_height <= height_reg;
                        work_pattern <= pattern_reg;
                        // latch from wb_dat_i (the value being written
                        // THIS cycle) rather than fill_reg/
                        // clip_enable_reg/glyph_reg -- those are
                        // updated by the separate wishbone-slave
                        // always block on this SAME clock edge, so
                        // reading them here would see their PRE-edge
                        // (stale) value, not the one this trigger
                        // write is actually setting.
                        work_fill <= wb_dat_i[CTRL_FILL];
                        work_clip <= wb_dat_i[CTRL_CLIP];
                        work_glyph <= wb_dat_i[CTRL_GLYPH];
                        work_glyph_addr <= glyph_addr_reg;
                        work_glyph_w <= glyph_w_reg;
                        work_glyph_h <= glyph_h_reg;
                        work_fg <= fg_color_reg;
                        work_bg <= bg_color_reg;
                        // same reason as work_fill/work_clip/work_glyph
                        // above: read the bit out of the value being
                        // written this cycle, not out of srcmem_reg,
                        // which the slave block updates on this same edge
                        work_srcmem <= wb_dat_i[CTRL_SRCMEM];
                        work_src_addr <= src_addr_reg;
                        work_src_stride <= src_stride_reg;
                        work_src_shift <= src_shift_reg[4:0];
                        work_src_prime <= src_shift_reg[8];

                        draw_busy <= 1'b1;

                        if (wb_dat_i[CTRL_GLYPH]) state <= ST_GLYPH_SETUP;
                        else state <= ST_CLIP_CALC;
                    end
                end

                // -- fill/copy path (unchanged from before, aside from
                // the ST_READ/ST_WRITE routing fix below) --

                // Nothing to do but let rect_x_end/rect_y_end catch up
                // with the work_* registers latched last cycle.
                ST_CLIP_CALC: state <= ST_CLIP;

                ST_CLIP: begin
                    if (work_clip) begin
                        clip_x <= final_x;
                        clip_y <= final_y;
                        clip_width <= final_width;
                        clip_height <= final_height;

                        if (final_width == 0 || final_height == 0 ||
                            final_x >= screen_clip_x_end || final_y >= screen_clip_y_end) begin
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            current_line <= 32'h0;
                            current_word_in_line <= 32'h0;
                            total_lines <= final_height;
                            words_per_line <= word_span_words;
                            line_start_addr <= final_y * SCREEN_STRIDE + (left_word_boundary >> 3);
                            current_word_addr <= final_y * SCREEN_STRIDE + (left_word_boundary >> 3);

                            left_mask <= left_pixel_mask;
                            right_mask <= right_pixel_mask;

                            // memory copy starts its own per-row source
                            // walk instead of the framebuffer read
                            // below; everything set up above (masks,
                            // word counts, destination addresses) is
                            // shared with it unchanged.
                            mem_row_addr <= work_src_addr;

                            // clipped fills may need to preserve bits
                            // outside the clip rect within a partial word
                            // (see left_mask/right_mask above), so always
                            // read the existing word first here -- an
                            // unclipped fill (below) never needs to
                            // preserve anything (its masks are always
                            // all-1s) so it's still safe to skip straight
                            // to ST_WRITE there.
                            // any copy -- from main memory or from
                            // VRAM -- goes through the source walk now.
                            // Only a fill skips it.
                            if (!work_fill)
                                state <= ST_MEM_ROW;
                            else
                                state <= ST_READ;
                        end
                    end else begin
                        clip_x <= work_dst_x;
                        clip_y <= work_dst_y;
                        clip_width <= work_width;
                        clip_height <= work_height;

                        current_line <= 32'h0;
                        current_word_in_line <= 32'h0;
                        total_lines <= work_height;
                        words_per_line <= (work_width + 31) >> 5;
                        line_start_addr <= work_dst_y * SCREEN_STRIDE + (work_dst_x >> 5) * 4;
                        current_word_addr <= work_dst_y * SCREEN_STRIDE + (work_dst_x >> 5) * 4;

                        left_mask <= 32'hFFFFFFFF;
                        right_mask <= 32'hFFFFFFFF;

                        mem_row_addr <= work_src_addr;

                        if (work_fill) begin
                            state <= ST_WRITE;
                        end else begin
                            state <= ST_MEM_ROW;
                        end
                    end
                end

                // -- memory copy source walk --
                //
                // One row's worth of source words is streamed through a
                // two-word window (src_prev, plus the word just read),
                // producing one destination word per read. Each
                // destination word then goes through the same
                // ST_READ/ST_WRITE read-modify-write the fill path uses,
                // so partial words at the left and right edges preserve
                // whatever was already there.

                ST_MEM_ROW: begin
                    // Both ports idle before the next source read
                    // asserts one of them. For a main-memory source this
                    // is the invariant the header comment describes --
                    // the framebuffer port must be released before the
                    // main-bus port is taken, or the two arbiters can
                    // deadlock against each other. For a VRAM source it
                    // is simply the gap between two transactions on the
                    // same port.
                    m_cyc_o <= 1'b0;
                    m_stb_o <= 1'b0;
                    m_we_o <= 1'b0;
                    s_cyc_o <= 1'b0;
                    s_stb_o <= 1'b0;

                    mem_next_addr <= mem_row_addr;
                    src_prev <= 32'h0;

                    // work_src_prime handles the one case where the
                    // pixel landing on bit 0 of the first destination
                    // word lies in the word BEFORE the source buffer.
                    // That happens whenever the source x is smaller than
                    // the destination's offset within its own word, and
                    // it can only ever be one word early: the offset is
                    // at most 31 pixels. Rather than read out of bounds,
                    // software sets this bit and the window simply
                    // starts with zeros -- correct, because every pixel
                    // sourced from that phantom word is masked out of
                    // the destination anyway.
                    if (work_src_prime)
                        state <= ST_MEM_READ;
                    else
                        state <= ST_MEM_PRIME;
                end

                ST_MEM_PRIME: begin
                    if (work_srcmem) begin
                        s_cyc_o <= 1'b1;
                        s_stb_o <= 1'b1;
                        s_adr_o <= mem_next_addr;
                    end else begin
                        m_cyc_o <= 1'b1;
                        m_stb_o <= 1'b1;
                        m_we_o <= 1'b0;
                        m_sel_o <= 4'b1111;
                        m_adr_o <= mem_next_addr;
                    end
                    state <= ST_MEM_PRIME_WAIT;
                end

                ST_MEM_PRIME_WAIT: begin
                    if (mem_src_ack) begin
                        src_prev <= mem_src_dat;
                        mem_next_addr <= mem_next_addr + 4;
                        s_cyc_o <= 1'b0;
                        s_stb_o <= 1'b0;
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        state <= ST_MEM_READ;
                    end
                end

                ST_MEM_READ: begin
                    if (work_srcmem) begin
                        s_cyc_o <= 1'b1;
                        s_stb_o <= 1'b1;
                        s_adr_o <= mem_next_addr;
                    end else begin
                        m_cyc_o <= 1'b1;
                        m_stb_o <= 1'b1;
                        m_we_o <= 1'b0;
                        m_sel_o <= 4'b1111;
                        m_adr_o <= mem_next_addr;
                    end
                    state <= ST_MEM_READ_WAIT;
                end

                ST_MEM_READ_WAIT: begin
                    if (mem_src_ack) begin
                        // mem_blend reads the source data wire directly,
                        // so it is only valid in this cycle -- latch the
                        // result rather than recomputing it later from a
                        // src_cur register that would need its own
                        // sequencing.
                        src_word_out <= mem_blend;
                        src_prev <= mem_src_dat;
                        mem_next_addr <= mem_next_addr + 4;
                        s_cyc_o <= 1'b0;
                        s_stb_o <= 1'b0;
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        state <= ST_READ;
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

                    if (work_fill) begin
                        if (work_clip && words_per_line > 1) begin
                            if (current_word_in_line == 0) begin
                                m_dat_o <= (read_data & ~left_mask) | (work_pattern & left_mask);
                            end else if (current_word_in_line == words_per_line - 1) begin
                                m_dat_o <= (read_data & ~right_mask) | (work_pattern & right_mask);
                            end else begin
                                m_dat_o <= work_pattern;
                            end
                        end else if (work_clip && words_per_line == 1) begin
                            m_dat_o <= (read_data & ~(left_mask & right_mask)) |
                                       (work_pattern & (left_mask & right_mask));
                        end else begin
                            m_dat_o <= work_pattern;
                        end
                    end else if (!work_fill) begin
                        // copy: merge the assembled source word in,
                        // preserving whatever lies outside the clipped
                        // rect within a partial edge word. Identical for
                        // both source kinds -- by this point the source
                        // word has already been fetched and shifted, and
                        // where it came from no longer matters.
                        m_dat_o <= (read_data & ~mem_edge_mask) |
                                   (src_word_out & mem_edge_mask);
                    end else begin
                        // unreachable: with CTRL_FILL clear, ST_CLIP
                        // always routes into the copy path above,
                        // whichever source it is reading from. Kept as a
                        // harmless identity write rather than removed,
                        // so a malformed control word can't leave
                        // m_dat_o holding a stale value from a previous
                        // operation.
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
                        current_word_in_line <= 32'h0;
                        current_line <= current_line + 1;

                        if (current_line + 1 >= total_lines) begin
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            line_start_addr <= line_start_addr + SCREEN_STRIDE;
                            current_word_addr <= line_start_addr + SCREEN_STRIDE;

                            // next source row. Kept as a running add
                            // rather than base + line*stride so there is
                            // no per-row multiplier in this path.
                            mem_row_addr <= mem_row_addr + work_src_stride;

                            // clipped fills can land on a partial word
                            // here too (this line's first/last word), so
                            // route through ST_READ the same as ST_CLIP
                            // does -- only an unclipped fill can skip
                            // straight to a blind ST_WRITE.
                            if (!work_fill) begin
                                state <= ST_MEM_ROW;
                            end else if (work_fill && !work_clip) begin
                                state <= ST_WRITE;
                            end else begin
                                state <= ST_READ;
                            end
                        end
                    end else begin
                        current_word_in_line <= current_word_in_line + 1;
                        current_word_addr <= current_word_addr + 4;

                        // same reasoning as above
                        if (!work_fill) begin
                            state <= ST_MEM_READ;
                        end else if (work_fill && !work_clip) begin
                            state <= ST_WRITE;
                        end else begin
                            state <= ST_READ;
                        end
                    end
                end

                // -- glyph blit path --

                ST_GLYPH_SETUP: begin
                    g_bit_offset <= work_dst_x[4:0];
                    g_line_addr <= work_dst_y * SCREEN_STRIDE + ((work_dst_x >> 5) * 4);
                    g_row <= 32'h0;
                    state <= ST_GLYPH_FETCH;
                end

                ST_GLYPH_FETCH: begin
                    glyph_addr_o <= work_glyph_addr[GLYPH_ADDR_WIDTH-1:0] + g_row[GLYPH_ADDR_WIDTH-1:0];
                    state <= ST_GLYPH_FETCH_WAIT;
                end

                ST_GLYPH_FETCH_WAIT: begin
                    // glyph_addr_o (set last state) has only just now
                    // become visible to glyph_mem's blit_addr input this
                    // cycle -- glyph_mem's own port B is a registered
                    // (synchronous) BRAM read, so its blit_data output
                    // won't reflect THIS address until the cycle after
                    // that. One wait state here is not enough: this state
                    // must only wait, not sample glyph_data_i yet (see
                    // ST_GLYPH_FETCH_WAIT2 below for the actual capture).
                    state <= ST_GLYPH_FETCH_WAIT2;
                end

                ST_GLYPH_FETCH_WAIT2: begin
                    // NOW glyph_data_i reflects mem[glyph_addr_o] as
                    // presented two cycles ago -- see ST_GLYPH_FETCH_WAIT.
                    g_glyph_byte <= glyph_data_i;
                    state <= ST_GLYPH_READ_LO;
                end

                ST_GLYPH_READ_LO: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b0;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + g_line_addr;
                    state <= ST_GLYPH_WAIT_READ_LO;
                end

                ST_GLYPH_WAIT_READ_LO: begin
                    if (m_ack_i) begin
                        read_data <= m_dat_i;
                        state <= ST_GLYPH_WRITE_LO;
                    end
                end

                ST_GLYPH_WRITE_LO: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b1;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + g_line_addr;
                    // every pixel within the glyph's cell gets fg or bg
                    // (solid cell, not a transparent overlay); anything
                    // outside the cell (other bits of this word) is
                    // preserved from the read above.
                    m_dat_o <= (read_data & ~g_cell_lo) | (g_fg_word & g_bits_lo) |
                               (g_bg_word & (g_cell_lo & ~g_bits_lo));
                    state <= ST_GLYPH_WAIT_WRITE_LO;
                end

                ST_GLYPH_WAIT_WRITE_LO: begin
                    if (m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        m_we_o <= 1'b0;
                        if (g_straddle) state <= ST_GLYPH_HI_SETTLE1;
                        else state <= ST_GLYPH_ROW_DONE;
                    end
                end

                // -- bus settle states between the low-word write and
                // the high-word read of a straddling glyph --
                //
                // this used to be a single cycle (straight from
                // ST_GLYPH_WAIT_WRITE_LO's ack into ST_GLYPH_READ_HI,
                // which re-asserted m_cyc_o/m_stb_o on the very next
                // cycle) -- confirmed via simulation (Icarus Verilog
                // 12.0, a testbench driving the real gpu_blit_wb +
                // glyph_mem + vram_wb + arbiter together, checking
                // actual framebuffer content after a real multi-
                // character string draw) to be too short. Both
                // vram_wb's own ack (registered, and NOT edge-gated --
                // rtl/mem/vram.v re-asserts wb_ack_o<=1 every single
                // cycle wb_active is high, with no "already acked"
                // guard the way glyph_mem has) and the arbiter's own
                // response-routing (rtl/arbiter_vram.v, also registered,
                // one cycle behind its own state transitions) each
                // need a full cycle to actually clear back to 0 once
                // m_cyc_o drops -- one cycle of m_cyc_o=0 isn't enough
                // for BOTH to settle before this state machine checks
                // m_ack_i again for the high-word read, so that read
                // could see a stale, already-consumed ack left over
                // from the low-word write, and capture the LOW word's
                // data into read_data instead of the high word's own.
                // On real hardware this produced exactly the reported
                // "horizontal garbage/duplicated characters" symptom
                // (docs/window_manager.md) -- deterministically, no
                // multi-process contention needed, any time a
                // straddling character followed others that had
                // already put non-zero ink in the low word (which is
                // why it reproduced reliably with real text but was
                // easy to miss testing a straddle in isolation with
                // nothing drawn before it, since a stale read of an
                // all-zero word looks identical to a correct one).
                //
                // The row-to-row transition (ST_GLYPH_WAIT_WRITE_HI ->
                // ST_GLYPH_ROW_DONE -> ST_GLYPH_FETCH -> ST_GLYPH_
                // FETCH_WAIT -> ST_GLYPH_FETCH_WAIT2 -> ST_GLYPH_
                // READ_LO) was never affected by this -- it already
                // has three full cycles of m_cyc_o=0 in between,
                // which turns out to be exactly the margin needed.
                // These two extra states give the low-to-high word
                // transition that same margin, rather than trying to
                // rely on the smallest gap that happens to work.
                ST_GLYPH_HI_SETTLE1: begin
                    state <= ST_GLYPH_HI_SETTLE2;
                end

                ST_GLYPH_HI_SETTLE2: begin
                    state <= ST_GLYPH_READ_HI;
                end

                ST_GLYPH_READ_HI: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b0;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + g_line_addr + 32'd4;
                    state <= ST_GLYPH_WAIT_READ_HI;
                end

                ST_GLYPH_WAIT_READ_HI: begin
                    if (m_ack_i) begin
                        read_data <= m_dat_i;
                        state <= ST_GLYPH_WRITE_HI;
                    end
                end

                ST_GLYPH_WRITE_HI: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    m_we_o <= 1'b1;
                    m_sel_o <= 4'b1111;
                    m_adr_o <= VRAM_BASE + g_line_addr + 32'd4;
                    m_dat_o <= (read_data & ~g_cell_hi) | (g_fg_word & g_bits_hi) |
                               (g_bg_word & (g_cell_hi & ~g_bits_hi));
                    state <= ST_GLYPH_WAIT_WRITE_HI;
                end

                ST_GLYPH_WAIT_WRITE_HI: begin
                    if (m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        m_we_o <= 1'b0;
                        state <= ST_GLYPH_ROW_DONE;
                    end
                end

                ST_GLYPH_ROW_DONE: begin
                    if (g_row + 1 >= work_glyph_h) begin
                        draw_busy <= 1'b0;
                        state <= ST_IDLE;
                    end else begin
                        g_row <= g_row + 1;
                        g_line_addr <= g_line_addr + SCREEN_STRIDE;
                        state <= ST_GLYPH_FETCH;
                    end
                end

                default: begin
                    state <= ST_IDLE;
                end
            endcase
        end
    end

endmodule
