`timescale 1ns/1ps

module tb_arbiter_stress;

    reg clk = 0;
    reg rst = 1;
    always #5 clk = ~clk;

    // -- gpu_blit_wb (master 2) CPU-facing wishbone slave port --
    reg         wb_cyc_i, wb_stb_i, wb_we_i;
    reg  [3:0]  wb_sel_i;
    reg  [31:0] wb_adr_i, wb_dat_i;
    wire        wb_ack_o;
    wire [31:0] wb_dat_o;

    // -- gpu_blit_wb master port (m2 on the arbiter) --
    wire        m2_cyc_o, m2_stb_o, m2_we_o;
    wire [3:0]  m2_sel_o;
    wire [31:0] m2_adr_o, m2_dat_o;
    wire [31:0] m2_dat_i;
    wire        m2_ack_i;

    wire [11:0] glyph_addr_o;
    wire [7:0]  glyph_data_i;
    wire        busy;

    gpu_blit_wb #(.GLYPH_ADDR_WIDTH(12)) dut_blit (
        .clk(clk), .rst(rst),
        .wb_cyc_i(wb_cyc_i), .wb_stb_i(wb_stb_i), .wb_we_i(wb_we_i),
        .wb_sel_i(wb_sel_i), .wb_adr_i(wb_adr_i), .wb_dat_i(wb_dat_i),
        .wb_ack_o(wb_ack_o), .wb_dat_o(wb_dat_o),
        .m_cyc_o(m2_cyc_o), .m_stb_o(m2_stb_o), .m_we_o(m2_we_o),
        .m_sel_o(m2_sel_o), .m_adr_o(m2_adr_o), .m_dat_o(m2_dat_o),
        .m_dat_i(m2_dat_i), .m_ack_i(m2_ack_i),
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

    // -- synthetic master 1 (stand-in for gpu_raster_wb) --
    // mimics the SAME "hold cyc across a read-then-write, drop
    // between ops" pattern real gpu_blit.v/gpu_raster.v both use,
    // continuously read-modify-writing a canary word at a FIXED
    // address far from where the glyph line is drawn, as aggressively
    // as possible to maximize arbiter contention windows.
    reg         m1_cyc_o, m1_stb_o, m1_we_o;
    reg  [3:0]  m1_sel_o;
    reg  [31:0] m1_adr_o, m1_dat_o;
    wire [31:0] m1_dat_i;
    wire        m1_ack_i;

    localparam CANARY_WORD_ADDR = 32'h20000000 + (400*80); // line 400, word 0
    reg [31:0] canary_expected;
    integer canary_writes, canary_errors;

    reg [2:0] m1_state;
    localparam M1_IDLE=0, M1_READ=1, M1_WAIT_READ=2, M1_WRITE=3, M1_WAIT_WRITE=4, M1_GAP=5;
    reg [7:0] m1_gap_cnt;

    always @(posedge clk) begin
        if (rst) begin
            m1_cyc_o<=0; m1_stb_o<=0; m1_we_o<=0; m1_state<=M1_IDLE;
            canary_writes<=0; canary_errors<=0; m1_gap_cnt<=0;
        end else begin
            case (m1_state)
                M1_IDLE: begin
                    m1_cyc_o<=1; m1_stb_o<=1; m1_we_o<=0; m1_sel_o<=4'hF;
                    m1_adr_o<=CANARY_WORD_ADDR;
                    m1_state<=M1_WAIT_READ;
                end
                M1_WAIT_READ: begin
                    if (m1_ack_i) m1_state<=M1_WRITE;
                end
                M1_WRITE: begin
                    m1_we_o<=1;
                    m1_dat_o<= canary_writes[31:0] ^ 32'hC0FFEE00; // new canary value
                    m1_state<=M1_WAIT_WRITE;
                end
                M1_WAIT_WRITE: begin
                    if (m1_ack_i) begin
                        m1_cyc_o<=0; m1_stb_o<=0; m1_we_o<=0;
                        canary_expected <= canary_writes[31:0] ^ 32'hC0FFEE00;
                        canary_writes <= canary_writes + 1;
                        m1_gap_cnt <= 2; // brief gap, like gpu_blit's
                                          // glyph-fetch gap between rows
                        m1_state<=M1_GAP;
                    end
                end
                M1_GAP: begin
                    if (m1_gap_cnt == 0) m1_state<=M1_IDLE;
                    else m1_gap_cnt <= m1_gap_cnt - 1;
                end
            endcase
        end
    end

    // -- arbiter (the real one) --
    wire [31:0] s_adr_o, s_dat_o, s_dat_i;
    wire [3:0]  s_sel_o;
    wire        s_we_o, s_stb_o, s_cyc_o, s_ack_i;
    wire [1:0]  varb_master;

    wb_arbiter arb (
        .clk(clk), .rst(rst),
        .m0_adr_i(32'b0), .m0_dat_i(32'b0), .m0_dat_o(), .m0_we_i(1'b0),
        .m0_sel_i(4'b0), .m0_stb_i(1'b0), .m0_cyc_i(1'b0), .m0_ack_o(),
        .m1_adr_i(m1_adr_o), .m1_dat_i(m1_dat_o), .m1_dat_o(m1_dat_i),
        .m1_we_i(m1_we_o), .m1_sel_i(m1_sel_o), .m1_stb_i(m1_stb_o),
        .m1_cyc_i(m1_cyc_o), .m1_ack_o(m1_ack_i),
        .m2_adr_i(m2_adr_o), .m2_dat_i(m2_dat_o), .m2_dat_o(m2_dat_i),
        .m2_we_i(m2_we_o), .m2_sel_i(m2_sel_o), .m2_stb_i(m2_stb_o),
        .m2_cyc_i(m2_cyc_o), .m2_ack_o(m2_ack_i),
        .s_adr_o(s_adr_o), .s_dat_o(s_dat_o), .s_dat_i(s_dat_i),
        .s_we_o(s_we_o), .s_sel_o(s_sel_o), .s_stb_o(s_stb_o),
        .s_cyc_o(s_cyc_o), .s_ack_i(s_ack_i),
        .master(varb_master)
    );

    // -- real vram_wb slave --
    wire [14:0] s_adr_word = s_adr_o[16:2]; // byte->word, matching
                                              // sysctl.v's wbm_vram_adr_sel_word
    vram_wb vram (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(s_adr_word), .wb_dat_i(s_dat_o), .wb_dat_o(s_dat_i),
        .wb_we_i(s_we_o), .wb_sel_i(s_sel_o), .wb_stb_i(s_stb_o),
        .wb_ack_o(s_ack_i), .wb_cyc_i(s_cyc_o),
        .gb_adr_i(15'b0), .gb_dat_o()
    );

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

    task draw_glyph(input [31:0] x, input [31:0] gaddr, input [31:0] w);
    begin
        wb_write(4'd2, x);
        wb_write(4'd3, 32'd0);
        wb_write(4'd7, gaddr);
        wb_write(4'd8, w);
        wb_write(4'd9, 32'd1);
        wb_write(4'd10, 32'd1);
        wb_write(4'd11, 32'd0);
        wb_write(4'd0, (32'd1 | (32'd1<<3)));
        wait (busy == 1);
        wait (busy == 0);
    end
    endtask

    integer i;
    reg [7:0] expect_byte;
    reg [4:0] expect_bits, got_bits;
    reg [31:0] canary_final_check;
    integer errors, offset, wordidx;

    localparam N_CHARS = 48;

    initial begin
        wb_cyc_i=0; wb_stb_i=0; wb_we_i=0; wb_sel_i=0; wb_adr_i=0; wb_dat_i=0;
        errors = 0;

        for (i = 0; i < N_CHARS; i = i + 1)
            gmem.mem[i] = { (i[4:0] ^ 5'h15), 3'b000 };

        rst = 1;
        repeat (6) @(posedge clk);
        rst = 0;
        repeat (6) @(posedge clk);

        // draw the 48-char line at row 0 WHILE the synthetic master 1
        // is continuously, aggressively contending for the bus with
        // its own read-modify-write canary cycles
        for (i = 0; i < N_CHARS; i = i + 1)
            draw_glyph(i * 5, i, 32'd5);

        repeat (20) @(posedge clk);

        // -- check 1: every glyph landed correctly (row 0) --
        for (i = 0; i < N_CHARS; i = i + 1) begin
            expect_byte = { (i[4:0] ^ 5'h15), 3'b000 };
            expect_bits = { expect_byte[3], expect_byte[4], expect_byte[5],
                             expect_byte[6], expect_byte[7] };
            offset = (i * 5) & 5'h1F;
            wordidx = (i * 5) >> 5;

            if (offset + 5 <= 32) begin
                got_bits = vram.vram[wordidx][offset +: 5];
            end else begin
                begin : sx
                    integer b;
                    reg [4:0] tmp;
                    for (b = 0; b < 5; b = b + 1) begin
                        if (offset + b < 32)
                            tmp[b] = vram.vram[wordidx][offset + b];
                        else
                            tmp[b] = vram.vram[wordidx+1][offset + b - 32];
                    end
                    got_bits = tmp;
                end
            end

            if (got_bits !== expect_bits) begin
                errors = errors + 1;
                $display("GLYPH MISMATCH char %0d (x=%0d): got %b expect %b",
                    i, i*5, got_bits, expect_bits);
            end
        end

        // -- check 2: canary word (master 1's own region) never got
        // corrupted by the blitter, and master 1 got to make real
        // progress (contention didn't starve or corrupt it either) --
        canary_final_check = vram.vram[400*80/4];
        $display("canary writes completed: %0d, last value=%h, vram holds=%h",
            canary_writes, canary_expected, canary_final_check);
        if (canary_writes < 5) begin
            errors = errors + 1;
            $display("CANARY STARVED -- master 1 barely made progress (%0d writes)", canary_writes);
        end
        if (canary_final_check !== canary_expected) begin
            errors = errors + 1;
            $display("CANARY CORRUPTED -- vram has %h, expected %h", canary_final_check, canary_expected);
        end

        if (errors == 0)
            $display("RESULT: PASS -- %0d chars correct, canary made %0d clean writes, no cross-master corruption",
                N_CHARS, canary_writes);
        else
            $display("RESULT: FAIL -- %0d errors (see above)", errors);

        $finish;
    end

    // safety timeout
    initial begin
        #200000;
        $display("RESULT: FAIL -- simulation timed out (deadlock/starvation?)");
        $finish;
    end

endmodule
