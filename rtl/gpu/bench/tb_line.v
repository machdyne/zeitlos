`timescale 1ns/1ps

module tb_line;

    reg clk = 0;
    reg rst = 1;
    always #5 clk = ~clk;

    reg         wb_cyc_i, wb_stb_i, wb_we_i;
    reg  [3:0]  wb_sel_i;
    reg  [31:0] wb_adr_i, wb_dat_i;
    wire        wb_ack_o;
    wire [31:0] wb_dat_o;

    wire        m_cyc_o, m_stb_o, m_we_o;
    wire [3:0]  m_sel_o;
    wire [31:0] m_adr_o, m_dat_o;
    reg  [31:0] m_dat_i;
    reg         m_ack_i;

    wire [11:0] glyph_addr_o;
    wire [7:0]  glyph_data_i;

    wire busy;

    gpu_blit_wb #(.GLYPH_ADDR_WIDTH(12)) dut (
        .clk(clk), .rst(rst),
        .wb_cyc_i(wb_cyc_i), .wb_stb_i(wb_stb_i), .wb_we_i(wb_we_i),
        .wb_sel_i(wb_sel_i), .wb_adr_i(wb_adr_i), .wb_dat_i(wb_dat_i),
        .wb_ack_o(wb_ack_o), .wb_dat_o(wb_dat_o),
        .m_cyc_o(m_cyc_o), .m_stb_o(m_stb_o), .m_we_o(m_we_o),
        .m_sel_o(m_sel_o), .m_adr_o(m_adr_o), .m_dat_o(m_dat_o),
        .m_dat_i(m_dat_i), .m_ack_i(m_ack_i),
        .glyph_addr_o(glyph_addr_o), .glyph_data_i(glyph_data_i),
        .busy(busy)
    );

    glyph_mem #(.ADDR_WIDTH(12)) gmem (
        .clk(clk),
        .wb_cyc_i(1'b0), .wb_stb_i(1'b0), .wb_we_i(1'b0),
        .wb_sel_i(4'b0), .wb_adr_i(32'b0), .wb_dat_i(32'b0),
        .wb_ack_o(), .wb_dat_o(),
        .blit_addr(glyph_addr_o), .blit_data(glyph_data_i)
    );

    // fb slave modeled after the real rtl/mem/vram.v
    reg [31:0] fb [0:20000];
    wire fb_active = m_cyc_o && m_stb_o;
    always @(posedge clk) begin
        if (rst) begin
            m_ack_i <= 0;
        end else begin
            m_ack_i <= 0;
            if (fb_active) begin
                m_dat_i <= fb[(m_adr_o - 32'h20000000) >> 2];
                if (m_we_o) fb[(m_adr_o - 32'h20000000) >> 2] <= m_dat_o;
                m_ack_i <= 1;
            end
        end
    end

    task wb_write(input [3:0] reg_idx, input [31:0] data);
    begin
        @(posedge clk);
        wb_cyc_i = 1; wb_stb_i = 1; wb_we_i = 1;
        wb_sel_i = 4'hF; wb_adr_i = {28'b0, reg_idx}; wb_dat_i = data;
        @(posedge clk);
        while (!wb_ack_o) @(posedge clk);
        wb_cyc_i = 0; wb_stb_i = 0; wb_we_i = 0;
        @(posedge clk);
    end
    endtask

    // draws one glyph at (x, 0), height 1, using glyph memory address
    // `gaddr` (already loaded), and mimics z_fb_draw_char2's solid
    // cell (fg=1, bg=0)
    task draw_glyph(input [31:0] x, input [31:0] gaddr, input [31:0] w);
    begin
        wb_write(4'd2, x);
        wb_write(4'd3, 32'd0);
        wb_write(4'd7, gaddr);
        wb_write(4'd8, w);
        wb_write(4'd9, 32'd1);   // height 1 -- isolates per-character
                                  // horizontal placement, which is what
                                  // we're stress-testing here
        wb_write(4'd10, 32'd1);  // fg
        wb_write(4'd11, 32'd0);  // bg
        wb_write(4'd0, (32'd1 | (32'd1<<3)));
        wait (busy == 1);
        wait (busy == 0);
    end
    endtask

    integer i, col, errors;
    reg [7:0] expect_byte;
    reg [4:0] expect_bits;   // width-5 masked, bit-reversed
    reg [31:0] got_word;
    reg [4:0] offset;
    reg [4:0] got_bits;

    localparam N_CHARS = 48;   // > 32 so every 5px-pitch bit-offset
                                // (period 32, since gcd(32,5)=1) gets
                                // covered at least once, several
                                // offsets twice

    initial begin
        wb_cyc_i=0; wb_stb_i=0; wb_we_i=0; wb_sel_i=0; wb_adr_i=0; wb_dat_i=0;
        errors = 0;

        for (i = 0; i < 20001; i = i + 1) fb[i] = 32'h00000000;

        // one distinct byte per "character" (address = character
        // index), each with a unique 5-bit ink pattern so a wrong
        // character landing in the wrong place is unambiguous
        for (i = 0; i < N_CHARS; i = i + 1)
            gmem.mem[i] = { (i[4:0] ^ 5'h15), 3'b000 };  // top 5 bits used

        rst = 1;
        repeat (4) @(posedge clk);
        rst = 0;
        repeat (4) @(posedge clk);

        // draw N_CHARS glyphs left to right at real term pitch: 5px
        // wide, 5px pitch (no gap), exactly like TERM_FONT (z_font_5x8)
        for (i = 0; i < N_CHARS; i = i + 1)
            draw_glyph(i * 5, i, 32'd5);

        repeat (4) @(posedge clk);

        // now verify every character landed in exactly its own 5-bit
        // slot, nowhere else
        for (i = 0; i < N_CHARS; i = i + 1) begin
            expect_byte = { (i[4:0] ^ 5'h15), 3'b000 };
            // bit-reverse + width-5 mask, matching gpu_blit.v's own
            // g_byte_rev/g_glyph_bits derivation
            expect_bits = { expect_byte[3], expect_byte[4], expect_byte[5],
                             expect_byte[6], expect_byte[7] };

            offset = (i * 5) & 5'h1F;
            col = (i * 5) >> 5;   // word index (line stride here is
                                   // just word-index directly, y=0)

            if (offset + 5 <= 32) begin
                got_word = fb[col];
                got_bits = got_word[offset +: 5];
                if (got_bits !== expect_bits) begin
                    errors = errors + 1;
                    $display("MISMATCH char %0d (x=%0d, word=%0d, offset=%0d): got %b expect %b",
                        i, i*5, col, offset, got_bits, expect_bits);
                end
            end else begin
                // straddles col and col+1 -- build bit-by-bit to avoid
                // variable-width part-selects
                begin : straddle_extract
                    integer b;
                    reg [4:0] tmp;
                    for (b = 0; b < 5; b = b + 1) begin
                        if (offset + b < 32)
                            tmp[b] = fb[(i*5)>>5][offset + b];
                        else
                            tmp[b] = fb[((i*5)>>5)+1][offset + b - 32];
                    end
                    got_bits = tmp;
                end
                if (got_bits !== expect_bits) begin
                    errors = errors + 1;
                    $display("MISMATCH(straddle) char %0d (x=%0d, word=%0d/%0d, offset=%0d): got %b expect %b",
                        i, i*5, col, col+1, offset, got_bits, expect_bits);
                end
            end
        end

        if (errors == 0)
            $display("RESULT: PASS -- all %0d characters landed correctly, no cross-contamination", N_CHARS);
        else
            $display("RESULT: FAIL -- %0d mismatches (see above)", errors);

        $finish;
    end

endmodule
