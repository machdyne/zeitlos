/*
 * tb_memblit -- exercises gpu_blit_wb's memory-copy mode against a
 * reference model, plus a fill-mode regression so the new source port
 * can't quietly break what was already working.
 *
 * The two slave models deliberately behave DIFFERENTLY, because the
 * real ones do:
 *
 *   - rtl/mem/vram.v acks on every cycle cyc && stb are high, with no
 *     "one ack per transaction" guard. The pre-existing fill path
 *     depends on that: it holds cyc asserted across its read and its
 *     write, so a stricter slave would ack the read twice and swallow
 *     the write entirely. Modelling VRAM strictly made this testbench
 *     report failures in fill mode that hardware does not actually
 *     have.
 *
 *   - rtl/mem/sram.v (and the SDRAM controller) issue exactly one ack
 *     per transaction with a gap afterwards. Main memory is modelled
 *     that way, plus optional extra latency, since the source port has
 *     to be correct against the stricter of the two.
 */

`timescale 1ns/1ps

module tb_memblit;

    reg clk = 0;
    reg rst = 1;

    always #5 clk = ~clk;

    // -- blitter slave (config) side --
    reg         wb_cyc, wb_stb, wb_we;
    reg  [31:0] wb_adr, wb_dat;
    wire [31:0] wb_dat_o;
    wire        wb_ack;

    // -- framebuffer master --
    wire        m_cyc, m_stb, m_we;
    wire [3:0]  m_sel;
    wire [31:0] m_adr, m_dat_o;
    reg  [31:0] m_dat_i;
    reg         m_ack;

    // -- main memory master (source reads) --
    wire        s_cyc, s_stb, s_we;
    wire [3:0]  s_sel;
    wire [31:0] s_adr;
    reg  [31:0] s_dat_i;
    reg         s_ack;

    wire [11:0] glyph_addr;
    wire        busy;

    gpu_blit_wb #(.GLYPH_ADDR_WIDTH(12)) dut (
        .clk(clk), .rst(rst),
        .wb_cyc_i(wb_cyc), .wb_stb_i(wb_stb), .wb_we_i(wb_we),
        .wb_sel_i(4'b1111), .wb_adr_i(wb_adr), .wb_dat_i(wb_dat),
        .wb_ack_o(wb_ack), .wb_dat_o(wb_dat_o),
        .m_cyc_o(m_cyc), .m_stb_o(m_stb), .m_we_o(m_we), .m_sel_o(m_sel),
        .m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i), .m_ack_i(m_ack),
        .s_cyc_o(s_cyc), .s_stb_o(s_stb), .s_we_o(s_we), .s_sel_o(s_sel),
        .s_adr_o(s_adr), .s_dat_i(s_dat_i), .s_ack_i(s_ack),
        .glyph_addr_o(glyph_addr), .glyph_data_i(8'h00),
        .busy(busy)
    );

    // -- memories --
    localparam VRAM_BASE = 32'h20000000;
    localparam MEM_BASE  = 32'h40000000;

    localparam SCREEN_W = 640;
    localparam VRAM_WPL = 20;          // 640 / 32
    localparam SRC_W    = 512;
    localparam SRC_WPL  = 16;          // 512 / 32
    localparam SRC_H    = 64;

    reg [31:0] vram [0:9599];
    reg [31:0] shadow [0:9599];        // what VRAM held before the blit
    // one guard word past the end: the shifter reads one source word
    // beyond the last one it needs whenever the copy is unaligned. That
    // read is harmless (the bits are masked away) but the model has to
    // provide something for it.
    reg [31:0] srcmem [0:(SRC_WPL*SRC_H)+8];

    integer i, j;

    // -- framebuffer slave model: a faithful copy of rtl/mem/vram.v --
    always @(posedge clk) begin
        if (rst) begin
            m_ack <= 1'b0;
            m_dat_i <= 32'b0;
        end else begin
            m_ack <= 1'b0;
            if (m_cyc && m_stb) begin
                m_dat_i <= vram[(m_adr - VRAM_BASE) >> 2];
                if (m_we) vram[(m_adr - VRAM_BASE) >> 2] <= m_dat_o;
                m_ack <= 1'b1;
            end
        end
    end

    // -- main memory slave model: one ack per transaction, with a gap,
    // like rtl/mem/sram.v. mem_latency adds wait states so the source
    // port is exercised against something slower than an ideal slave --
    integer mem_latency;
    integer mem_wait;
    reg     mem_pending;

    always @(posedge clk) begin
        if (rst) begin
            s_ack <= 1'b0;
            mem_pending <= 1'b0;
            mem_wait <= 0;
            s_dat_i <= 32'b0;
        end else begin
            s_ack <= 1'b0;
            if (s_ack) begin
                mem_pending <= 1'b0;
            end else if (mem_pending) begin
                if (mem_wait > 0) begin
                    mem_wait <= mem_wait - 1;
                end else begin
                    s_dat_i <= srcmem[(s_adr - MEM_BASE) >> 2];
                    s_ack <= 1'b1;
                    mem_pending <= 1'b0;
                end
            end else if (s_cyc && s_stb) begin
                mem_pending <= 1'b1;
                mem_wait <= mem_latency;
            end
        end
    end

    // -- overlap check: the two master ports must never both be active,
    // or the deadlock the header comment describes becomes possible --
    always @(posedge clk)
        if (!rst && m_cyc && s_cyc) begin
            $display("FAIL: both master ports asserted simultaneously");
            $finish;
        end

    // -- helpers --

    task wb_write(input [31:0] a, input [31:0] d);
        begin
            @(posedge clk);
            wb_adr <= a; wb_dat <= d; wb_we <= 1'b1;
            wb_cyc <= 1'b1; wb_stb <= 1'b1;
            @(posedge clk);
            while (!wb_ack) @(posedge clk);
            wb_cyc <= 1'b0; wb_stb <= 1'b0; wb_we <= 1'b0;
            @(posedge clk);
        end
    endtask

    function src_pixel(input integer x, input integer y);
        begin
            if (x < 0 || x >= SRC_W || y < 0 || y >= SRC_H)
                src_pixel = 1'b0;
            else
                src_pixel = srcmem[y*SRC_WPL + (x >> 5)][x & 31];
        end
    endfunction

    function vram_pixel(input integer x, input integer y);
        begin
            vram_pixel = vram[y*VRAM_WPL + (x >> 5)][x & 31];
        end
    endfunction

    function shadow_pixel(input integer x, input integer y);
        begin
            shadow_pixel = shadow[y*VRAM_WPL + (x >> 5)][x & 31];
        end
    endfunction

    integer errors;
    integer sbit0, sword, sshift, sprime, saddr;
    integer tx, ty, expect_bit, got_bit;
    integer test_num;

    // Runs one memory-copy and checks every pixel on screen.
    task do_memcopy(input integer src_x, input integer src_y,
                    input integer dst_x, input integer dst_y,
                    input integer w,     input integer h);
        begin
            for (i = 0; i < 9600; i = i + 1) shadow[i] = vram[i];

            // -- this is the same arithmetic z_fb_hw_blit_mem() does in
            // software; testing it here is testing that too --
            sbit0 = src_x - (dst_x % 32);
            if (sbit0 >= 0) begin
                sword  = sbit0 / 32;
                sshift = sbit0 % 32;
                sprime = 0;
            end else begin
                // sbit0 can only ever be in [-31,-1], so this is exactly
                // one word early and never more
                sword  = 0;
                sshift = sbit0 + 32;
                sprime = 1;
            end
            saddr = MEM_BASE + src_y*(SRC_WPL*4) + sword*4;

            wb_write(32'd2,  dst_x);
            wb_write(32'd3,  dst_y);
            wb_write(32'd4,  w);
            wb_write(32'd5,  h);
            wb_write(32'd12, saddr);
            wb_write(32'd13, SRC_WPL*4);
            wb_write(32'd14, (sprime << 8) | sshift);
            // START | CLIP | SRCMEM, FILL clear
            wb_write(32'd0,  (1 << 0) | (1 << 2) | (1 << 4));

            @(posedge clk);
            while (busy) @(posedge clk);

            for (ty = 0; ty < 480; ty = ty + 1) begin
                for (tx = 0; tx < SCREEN_W; tx = tx + 1) begin
                    got_bit = vram_pixel(tx, ty);
                    if (tx >= dst_x && tx < dst_x + w &&
                        ty >= dst_y && ty < dst_y + h)
                        expect_bit = src_pixel(src_x + (tx - dst_x),
                                               src_y + (ty - dst_y));
                    else
                        expect_bit = shadow_pixel(tx, ty);

                    if (got_bit !== expect_bit) begin
                        errors = errors + 1;
                        if (errors < 8)
                            $display("  MISMATCH test %0d at (%0d,%0d): got %0d want %0d  [src=(%0d,%0d) dst=(%0d,%0d) %0dx%0d shift=%0d prime=%0d]",
                                test_num, tx, ty, got_bit, expect_bit,
                                src_x, src_y, dst_x, dst_y, w, h, sshift, sprime);
                    end
                end
            end
        end
    endtask

    // Runs one VRAM-to-VRAM copy and checks every pixel. Same engine and
    // the same software-side arithmetic as do_memcopy(), the only
    // difference being that the source lives in the framebuffer and is
    // read through the m_* port, so CTRL_SRCMEM stays clear.
    //
    // The source rectangle is addressed as an offscreen region: rows
    // beyond the visible 480, which is where sprite data would live once
    // VRAM is enlarged past 640x480.
    task do_vramcopy(input integer src_x, input integer src_y,
                     input integer dst_x, input integer dst_y,
                     input integer w,     input integer h);
        begin
            for (i = 0; i < 9600; i = i + 1) shadow[i] = vram[i];

            sbit0 = src_x - (dst_x % 32);
            if (sbit0 >= 0) begin
                sword  = sbit0 / 32;
                sshift = sbit0 % 32;
                sprime = 0;
            end else begin
                sword  = 0;
                sshift = sbit0 + 32;
                sprime = 1;
            end
            saddr = VRAM_BASE + src_y*(VRAM_WPL*4) + sword*4;

            wb_write(32'd2,  dst_x);
            wb_write(32'd3,  dst_y);
            wb_write(32'd4,  w);
            wb_write(32'd5,  h);
            wb_write(32'd12, saddr);
            wb_write(32'd13, VRAM_WPL*4);
            wb_write(32'd14, (sprime << 8) | sshift);
            // START | CLIP, with FILL and SRCMEM both clear
            wb_write(32'd0,  (1 << 0) | (1 << 2));

            @(posedge clk);
            while (busy) @(posedge clk);

            for (ty = 0; ty < 480; ty = ty + 1) begin
                for (tx = 0; tx < SCREEN_W; tx = tx + 1) begin
                    got_bit = vram_pixel(tx, ty);
                    if (tx >= dst_x && tx < dst_x + w &&
                        ty >= dst_y && ty < dst_y + h)
                        // read the SHADOW for the source: a copy must
                        // see the pre-copy contents, which also catches
                        // an engine that clobbers its own source
                        expect_bit = shadow_pixel(src_x + (tx - dst_x),
                                                  src_y + (ty - dst_y));
                    else
                        expect_bit = shadow_pixel(tx, ty);

                    if (got_bit !== expect_bit) begin
                        errors = errors + 1;
                        if (errors < 8)
                            $display("  VRAMCOPY MISMATCH test %0d at (%0d,%0d): got %0d want %0d  [src=(%0d,%0d) dst=(%0d,%0d) %0dx%0d shift=%0d prime=%0d]",
                                test_num, tx, ty, got_bit, expect_bit,
                                src_x, src_y, dst_x, dst_y, w, h, sshift, sprime);
                    end
                end
            end
        end
    endtask

    integer seed;

    initial begin
        wb_cyc = 0; wb_stb = 0; wb_we = 0; wb_adr = 0; wb_dat = 0;
        m_ack = 0; s_ack = 0; m_dat_i = 0; s_dat_i = 0;
        errors = 0;
        test_num = 0;
        seed = 32'h1234_5678;
        mem_latency = 0;
        mem_wait = 0;
        mem_pending = 0;

        // pseudorandom source bitmap
        for (i = 0; i < SRC_WPL*SRC_H + 8; i = i + 1)
            srcmem[i] = $random(seed);

        // recognisable background so an over-write outside the rect is
        // caught rather than blending in
        for (i = 0; i < 9600; i = i + 1) vram[i] = 32'hA5A5_A5A5;

        repeat (4) @(posedge clk);
        rst = 0;
        repeat (2) @(posedge clk);

        // -- alignment sweep: every combination of source and
        // destination bit offset is what actually exercises the shifter --
        for (i = 0; i < 8; i = i + 1) begin
            for (j = 0; j < 8; j = j + 1) begin
                test_num = test_num + 1;
                do_memcopy(i*7, 3, 100 + j*9, 20 + i, 61, 3);
            end
        end

        // -- the prime-with-zero corner: src_x smaller than the
        // destination's own offset within its word --
        test_num = test_num + 1;  do_memcopy(0,  0, 33, 40, 64, 4);
        test_num = test_num + 1;  do_memcopy(1,  0, 63, 50, 40, 4);
        test_num = test_num + 1;  do_memcopy(5,  2, 37, 60, 90, 5);

        // -- exactly aligned (shift 0) --
        test_num = test_num + 1;  do_memcopy(32, 1, 64, 70, 96, 4);

        // -- single-word and sub-word widths --
        test_num = test_num + 1;  do_memcopy(3,  4, 200, 80, 1, 1);
        test_num = test_num + 1;  do_memcopy(3,  4, 201, 90, 7, 2);
        test_num = test_num + 1;  do_memcopy(17, 5, 210, 100, 31, 3);
        test_num = test_num + 1;  do_memcopy(17, 5, 216, 110, 32, 3);
        test_num = test_num + 1;  do_memcopy(17, 5, 216, 120, 33, 3);

        // -- large, tall copy --
        test_num = test_num + 1;  do_memcopy(0, 0, 61, 200, 500, 40);

        // -- right-edge clipping against the 640px screen --
        test_num = test_num + 1;  do_memcopy(0, 8, 600, 300, 100, 4);

        // -- repeat a few cases with a slow source, since a blitter
        // that only works against a zero-wait-state memory is not much
        // use on a board with SDRAM --
        mem_latency = 3;
        test_num = test_num + 1;  do_memcopy(0,  0, 33, 400, 64, 4);
        test_num = test_num + 1;  do_memcopy(17, 5, 216, 410, 33, 3);
        test_num = test_num + 1;  do_memcopy(32, 1, 64, 420, 96, 4);
        mem_latency = 0;

        $display("memcopy: %0d tests, %0d pixel errors", test_num, errors);

        // -- VRAM to VRAM: the same engine reading through m_* instead.
        // Fill a band of the framebuffer with known content first, then
        // copy it elsewhere at assorted alignments. --
        for (i = 0; i < 9600; i = i + 1) vram[i] = 32'hA5A5_A5A5;
        for (ty = 0; ty < 24; ty = ty + 1)
            for (i = 0; i < VRAM_WPL; i = i + 1)
                vram[ty*VRAM_WPL + i] = $random(seed);

        test_num = test_num + 1;  do_vramcopy(0,  0, 64,  120, 128, 8);
        test_num = test_num + 1;  do_vramcopy(0,  0, 67,  140, 128, 8);
        test_num = test_num + 1;  do_vramcopy(5,  2, 100, 160, 61,  6);
        test_num = test_num + 1;  do_vramcopy(37, 3, 201, 180, 33,  4);
        test_num = test_num + 1;  do_vramcopy(64, 4, 96,  240, 96,  4);
        test_num = test_num + 1;  do_vramcopy(3,  5, 600, 260, 100, 4);

        $display("after vramcopy: %0d tests, %0d pixel errors", test_num, errors);

        // -- fill regression: the pre-existing path must still work --
        for (i = 0; i < 9600; i = i + 1) vram[i] = 32'h0000_0000;
        wb_write(32'd2, 32'd35);
        wb_write(32'd3, 32'd10);
        wb_write(32'd4, 32'd70);
        wb_write(32'd5, 32'd5);
        wb_write(32'd6, 32'hFFFF_FFFF);
        wb_write(32'd0, (1 << 0) | (1 << 1) | (1 << 2));  // START|FILL|CLIP
        @(posedge clk);
        while (busy) @(posedge clk);

        for (ty = 0; ty < 480; ty = ty + 1)
            for (tx = 0; tx < SCREEN_W; tx = tx + 1) begin
                expect_bit = (tx >= 35 && tx < 105 && ty >= 10 && ty < 15);
                if (vram_pixel(tx, ty) !== expect_bit[0]) begin
                    errors = errors + 1;
                    if (errors < 12)
                        $display("  FILL MISMATCH at (%0d,%0d)", tx, ty);
                end
            end

        if (errors == 0) $display("ALL TESTS PASSED");
        else $display("FAILURES: %0d", errors);

        $finish;
    end

endmodule
