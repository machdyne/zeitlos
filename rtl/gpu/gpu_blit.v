/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * High-performance word-level blitter with intelligent clipping support
 * for font rendering and precise graphics.
 *
 * Modes (selected by CTRL_FILL/CTRL_GLYPH):
 *   - fill/copy (original): word-parallel rectangle fill, or copy (copy
 *     mode is currently a no-op stub -- see ST_WRITE, unchanged from
 *     before).
 *   - glyph (new): blits one font glyph from glyph memory (see
 *     rtl/mem/glyph.v) into the framebuffer, with a solid foreground/
 *     background per pixel -- proper terminal-cell semantics, not a
 *     transparent overlay. Unclipped by design: the caller (software)
 *     is expected to only trigger this for glyphs that are already
 *     fully on-screen, and fall back to software rendering for glyphs
 *     that would need partial clipping (e.g. right at a window edge).
 *     See docs/window_manager.md, "hardware glyph blitting".
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

    // Configuration registers
    reg [31:0] dst_x_reg, dst_y_reg, width_reg, height_reg;
    reg [31:0] pattern_reg;
    reg fill_reg, clip_enable_reg, glyph_reg;
    reg [31:0] glyph_addr_reg, glyph_w_reg, glyph_h_reg, fg_color_reg, bg_color_reg;

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

    // Operation variables
    reg [31:0] work_dst_x, work_dst_y, work_width, work_height, work_pattern;
    reg work_fill, work_clip, work_glyph;
    reg [31:0] work_glyph_addr, work_glyph_w, work_glyph_h, work_fg, work_bg;
    reg draw_busy;

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
    wire [31:0] rect_x_end = work_dst_x + work_width;
    wire [31:0] rect_y_end = work_dst_y + work_height;

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
            glyph_addr_reg <= 32'h0;
            glyph_w_reg <= 32'h0;
            glyph_h_reg <= 32'h0;
            fg_color_reg <= 32'h1;
            bg_color_reg <= 32'h0;
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
                        default: ;
                    endcase
                end else begin
                    case (wb_adr_i[3:0])
                        4'd0: wb_dat_o <= {28'h0, glyph_reg, clip_enable_reg, fill_reg, 1'b0};
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

                        draw_busy <= 1'b1;

                        if (wb_dat_i[CTRL_GLYPH]) state <= ST_GLYPH_SETUP;
                        else state <= ST_CLIP;
                    end
                end

                // -- fill/copy path (unchanged from before, aside from
                // the ST_READ/ST_WRITE routing fix below) --

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

                            // clipped fills may need to preserve bits
                            // outside the clip rect within a partial word
                            // (see left_mask/right_mask above), so always
                            // read the existing word first here -- an
                            // unclipped fill (below) never needs to
                            // preserve anything (its masks are always
                            // all-1s) so it's still safe to skip straight
                            // to ST_WRITE there.
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
                        current_word_in_line <= 32'h0;
                        current_line <= current_line + 1;

                        if (current_line + 1 >= total_lines) begin
                            draw_busy <= 1'b0;
                            state <= ST_IDLE;
                        end else begin
                            line_start_addr <= line_start_addr + SCREEN_STRIDE;
                            current_word_addr <= line_start_addr + SCREEN_STRIDE;

                            // clipped fills can land on a partial word
                            // here too (this line's first/last word), so
                            // route through ST_READ the same as ST_CLIP
                            // does -- only an unclipped fill can skip
                            // straight to a blind ST_WRITE.
                            if (work_fill && !work_clip) begin
                                state <= ST_WRITE;
                            end else begin
                                state <= ST_READ;
                            end
                        end
                    end else begin
                        current_word_in_line <= current_word_in_line + 1;
                        current_word_addr <= current_word_addr + 4;

                        // same reasoning as above
                        if (work_fill && !work_clip) begin
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
                // response-routing (rtl/arbiter.v, also registered,
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
