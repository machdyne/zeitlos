`timescale 1ns/1ps

module tb_straddle;

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

    // fb slave modeled after the real rtl/mem/vram.v -- see tb_glyph.v
    // for why the un-gated "ack every active cycle" behavior matters
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
                    $display("  [%0t] FB WRITE addr=%0d data=%h",
                        $time, (m_adr_o - 32'h20000000) >> 2, m_dat_o);
                end
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

    integer i;
    reg [31:0] word0, word1;

    // dst_x = 29, width = 5 -> bit_offset=29, offset+width=34>32 --
    // straddles: 3 bits (29,30,31) land in the low word, 2 bits
    // (0,1) land in the next word.
    initial begin
        wb_cyc_i=0; wb_stb_i=0; wb_we_i=0; wb_sel_i=0; wb_adr_i=0; wb_dat_i=0;

        // pre-seed both destination words with a known pattern so we
        // can confirm the write only touches its own 5-bit cell and
        // leaves everything else alone
        for (i = 0; i < 20001; i = i + 1) fb[i] = 32'h00000000;
        fb[0] = 32'h55555555; // word containing bits 29-31 (line 0, word 0)
        fb[1] = 32'hAAAAAAAA; // word containing bits 0-1  (line 0, word 1)

        // single glyph row, byte = 0x80 -> after bit-reverse+width-5
        // mask -> only column 0 (the leftmost pixel) is ink. column0
        // at bit_offset=29 lands on bit 29 of the low word.
        gmem.mem[0] = 8'b10000000;

        rst = 1;
        repeat (4) @(posedge clk);
        rst = 0;
        repeat (4) @(posedge clk);

        wb_write(4'd2, 32'd29);  // dst_x
        wb_write(4'd3, 32'd0);   // dst_y
        wb_write(4'd7, 32'd0);   // glyph_addr
        wb_write(4'd8, 32'd5);  // glyph_w
        wb_write(4'd9, 32'd1);  // glyph_h -- one row is enough to
                                 // exercise the straddle split
        wb_write(4'd10, 32'd1); // fg_color
        wb_write(4'd11, 32'd0); // bg_color
        wb_write(4'd0, (32'd1 | (32'd1<<3))); // CTRL_START | CTRL_GLYPH

        wait (busy == 1);
        wait (busy == 0);
        repeat (2) @(posedge clk);

        word0 = fb[0];
        word1 = fb[1];

        $display("word0 (bits 0-31, cell at 29-31) = %b", word0);
        $display("word1 (bits 0-31, cell at 0-1)    = %b", word1);
        $display("expect word0 bits[31:29] = 001 (col0 ink, cols1-2 bg=0), bits[28:0] preserved from 0x55555555");
        $display("expect word1 bits[1:0]   = 00  (bg=0, no ink reaches here), bits[31:2] preserved from 0xAAAAAAAA");

        if (word0[31:29] === 3'b001 && word0[28:0] === (32'h55555555 & 29'h1FFFFFFF) &&
            word1[1:0] === 2'b00 && word1[31:2] === (32'hAAAAAAAA >> 2))
            $display("RESULT: PASS -- straddling glyph split correctly, no bleed into neighboring bits");
        else
            $display("RESULT: FAIL -- see values above");

        $finish;
    end

endmodule
