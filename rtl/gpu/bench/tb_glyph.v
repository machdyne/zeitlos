`timescale 1ns/1ps

module tb_glyph;

    reg clk = 0;
    reg rst = 1;
    always #5 clk = ~clk;

    // Wishbone slave side (CPU -> blitter) -- we drive this directly
    reg         wb_cyc_i, wb_stb_i, wb_we_i;
    reg  [3:0]  wb_sel_i;
    reg  [31:0] wb_adr_i, wb_dat_i;
    wire        wb_ack_o;
    wire [31:0] wb_dat_o;

    // Wishbone master side (blitter -> fake framebuffer)
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

    // real glyph_mem, port B wired straight to the blitter -- exactly
    // as sysctl.v does
    glyph_mem #(.ADDR_WIDTH(12)) gmem (
        .clk(clk),
        .wb_cyc_i(1'b0), .wb_stb_i(1'b0), .wb_we_i(1'b0),
        .wb_sel_i(4'b0), .wb_adr_i(32'b0), .wb_dat_i(32'b0),
        .wb_ack_o(), .wb_dat_o(),
        .blit_addr(glyph_addr_o), .blit_data(glyph_data_i)
    );

    // fake framebuffer: word-addressable RAM modeled directly after
    // the REAL slave (rtl/mem/vram.v) -- crucially, wb_active there is
    // NOT gated by "!wb_ack_o": it acks (and performs the access using
    // whatever adr/we/dat are presented THAT cycle) on every single
    // cycle cyc&&stb are held, not just the first. gpu_blit.v's master
    // side (both fill/copy and glyph paths) never drops m_cyc_o/
    // m_stb_o between a read and the write that follows it in the
    // same row/word -- only vram.v's un-gated design makes that
    // pattern land correctly; a naive "only ack once" slave model
    // (an earlier version of this testbench) mis-times it and produces
    // false failures that have nothing to do with the DUT.
    reg [31:0] fb [0:20000];
    wire fb_active = m_cyc_o && m_stb_o;
    always @(posedge clk) begin
        if (rst) begin
            m_ack_i <= 0;
        end else begin
            m_ack_i <= 0;
            if (fb_active) begin
                m_dat_i <= fb[(m_adr_o - 32'h20000000) >> 2];
                if (m_we_o) begin
                    fb[(m_adr_o - 32'h20000000) >> 2] <= m_dat_o;
                    $display("  [%0t] FB WRITE addr=%0d data=%b glyph_addr_o=%0d g_row=%0d g_glyph_byte=%b",
                        $time, (m_adr_o - 32'h20000000) >> 2, m_dat_o[4:0], glyph_addr_o, dut.g_row, dut.g_glyph_byte);
                end
                m_ack_i <= 1;
            end
        end
    end

    // -- wishbone helper task for writing blitter registers --
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

    integer i;
    reg [31:0] w0, w1, w2, w3;

    initial begin
        wb_cyc_i=0; wb_stb_i=0; wb_we_i=0; wb_sel_i=0; wb_adr_i=0; wb_dat_i=0;
        for (i = 0; i < 20001; i = i + 1) fb[i] = 32'hDEADBEEF;

        // synthetic 4-row-high, 5-px-wide font, 2 glyphs, loaded
        // directly into glyph memory (hierarchical poke -- simulation
        // only, mirrors what z_gfx_hw_font_load() does via the real
        // wishbone port A in hardware)
        // FOUR ROWS PER WORD, not one per element.
        //
        // rtl/mem/glyph.v stores `reg [31:0] mem[]` and the blit port
        // extracts mem[addr>>2][addr[1:0]*8 +: 8] -- byte lanes, low
        // lane first. This bench used to poke one 8-bit row per array
        // element, which was right when the array was byte-wide and
        // silently wrong once it became word-wide to match
        // rtl/mem/vram.v's shape. Every read then returned row N's
        // byte from word N, so only row 0 of each glyph had any data
        // and the test had been failing ever since.
        //
        // Rows are MSB-first within the byte, top 5 bits used.
        //
        // glyph 0: one pixel per row, marching right
        gmem.mem[0] = {8'b00010000,   // row3: col3
                       8'b00100000,   // row2: col2
                       8'b01000000,   // row1: col1
                       8'b10000000};  // row0: col0
        // glyph 1 -- deliberately different so any bleed from glyph
        // 0's last row is obvious
        gmem.mem[1] = {8'b00000000,   // row3: all off
                       8'b11111000,   // row2: all 5 cols on
                       8'b00000000,   // row1: all off
                       8'b11111000};  // row0: all 5 cols on

        rst = 1;
        repeat (4) @(posedge clk);
        rst = 0;
        repeat (4) @(posedge clk);

        $monitor("[%0t] state=%0d busy=%0b m_cyc=%0b m_stb=%0b m_we=%0b m_adr=%0d m_ack=%0b",
            $time, dut.state, busy, m_cyc_o, m_stb_o, m_we_o, m_adr_o, m_ack_i);

        // -- draw glyph 0 at (0,0) --
        wb_write(4'd2, 32'd0);   // dst_x
        wb_write(4'd3, 32'd0);   // dst_y
        wb_write(4'd7, 32'd0);   // glyph_addr
        wb_write(4'd8, 32'd5);   // glyph_w
        wb_write(4'd9, 32'd4);   // glyph_h
        wb_write(4'd10, 32'd1);  // fg_color
        wb_write(4'd11, 32'd0);  // bg_color
        wb_write(4'd0, (32'd1 | (32'd1<<3))); // CTRL_START | CTRL_GLYPH

        wait (busy == 1);
        wait (busy == 0);
        repeat (2) @(posedge clk);

        // -- draw glyph 1 immediately after, at (0,0) again (so we
        // can directly see whether glyph 0's tail bled into it) --
        wb_write(4'd2, 32'd0);
        wb_write(4'd3, 32'd0);
        wb_write(4'd7, 32'd4);   // glyph_addr = glyph 1
        wb_write(4'd8, 32'd5);
        wb_write(4'd9, 32'd4);
        wb_write(4'd10, 32'd1);
        wb_write(4'd11, 32'd0);
        wb_write(4'd0, (32'd1 | (32'd1<<3)));

        wait (busy == 1);
        wait (busy == 0);
        repeat (2) @(posedge clk);

        w0 = fb[0];   // line 0 word (row0 of glyph1, since glyph1 overwrote)
        w1 = fb[20];  // line1 = word 80 bytes = 20 words in
        w2 = fb[40];  // line2
        w3 = fb[60];  // line3

        $display("glyph1 rows written to framebuffer (5 lsbs = cols 0..4):");
        $display("row0 = %b  (expect 11111)", w0[4:0]);
        $display("row1 = %b  (expect 00000)", w1[4:0]);
        $display("row2 = %b  (expect 11111)", w2[4:0]);
        $display("row3 = %b  (expect 00000)", w3[4:0]);

        if (w0[4:0] === 5'b11111 && w1[4:0] === 5'b00000 &&
            w2[4:0] === 5'b11111 && w3[4:0] === 5'b00000)
            $display("RESULT: PASS -- glyph rendered correctly, no row shift/bleed");
        else
            $display("RESULT: FAIL -- glyph rows shifted/corrupted (see above)");

        $finish;
    end

endmodule
