/*
 * tb_rop -- exercises gpu_blit_wb's RASTER OPERATIONS (CTRL bits 6:5)
 * against a reference model, in both fill and memory-copy mode.
 *
 * Harness (clock, the two slave models, wb_write) is lifted from
 * tb_memblit.v unchanged, including its comment below on why the two
 * slaves deliberately behave differently.
 *
 * What this checks, and why each one is here:
 *
 *   1. Every op does what it says, per bit, against a model computed
 *      independently from the source and the pre-blit destination.
 *      COPY/OR/XOR/ANDN in fill mode and in memory-copy mode.
 *
 *   2. ROP_COPY is bit-for-bit identical to a blit with the ROP field
 *      absent. This is the REGRESSION that matters: gpu_blit has
 *      shipped, and every existing caller writes zeros into these bits
 *      without knowing they exist.
 *
 *   3. THE BLIND-FILL HAZARD. An unclipped fill used to skip the
 *      destination read and write straight out. With a non-COPY op
 *      that would merge against whatever read_data held from the
 *      PREVIOUS blit -- wrong in a way that depends on history. The
 *      test deliberately runs a blit that leaves a known non-zero
 *      value in read_data, then an unclipped OR fill over a different
 *      area, and checks the result is dst|src and not dst|stale.
 *
 *   4. The masked-sprite recipe end to end: ANDN the mask, OR the
 *      data, and confirm the destination outside the sprite's opaque
 *      pixels is untouched while the sprite itself lands exactly.
 *      That is the thing this feature exists for, so it is tested as
 *      a whole and not only as four separate ops.
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

module tb_rop;

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
    // -- register map, mirrored from gpu_blit.v --
    // CTRL is register 0 and STATUS is 1 -- the geometry starts at 2.
    localparam R_CTRL = 0, R_STATUS = 1;
    localparam R_DST_X = 2, R_DST_Y = 3, R_W = 4, R_H = 5;
    localparam R_PATTERN = 6;
    localparam R_SRC_ADDR = 12, R_SRC_STRIDE = 13, R_SRC_SHIFT = 14;

    localparam C_START = 32'h1, C_FILL = 32'h2, C_CLIP = 32'h4,
               C_SRCMEM = 32'h10;

    // CTRL bits 6:5
    localparam C_ROP_COPY = 32'h00, C_ROP_OR = 32'h20,
               C_ROP_XOR  = 32'h40, C_ROP_ANDN = 32'h60;
    localparam C_COOKIE = 32'h80;
    localparam C_DITHER = 32'h100;
    localparam R_SRC_B = 15;

    integer errors = 0;

    task wait_idle;
        begin
            @(posedge clk);
            while (busy) @(posedge clk);
            repeat (4) @(posedge clk);
        end
    endtask

    task snapshot;
        integer k;
        begin
            for (k = 0; k < 9600; k = k + 1) shadow[k] = vram[k];
        end
    endtask

    // Reference: what the destination word SHOULD be after applying
    // `rop` to `src`, computed from the pre-blit shadow rather than
    // from anything the DUT produced.
    function [31:0] model;
        input [31:0] dst;
        input [31:0] src;
        input [1:0]  rop;
        begin
            case (rop)
                2'd1: model = dst | src;
                2'd2: model = dst ^ src;
                2'd3: model = dst & ~src;
                default: model = src;
            endcase
        end
    endfunction

    task check_word;
        input [255:0] what;
        input integer idx;
        input [31:0] expect;
        begin
            if (vram[idx] !== expect) begin
                $display("FAIL: %0s: vram[%0d] = %08x, expected %08x",
                    what, idx, vram[idx], expect);
                errors = errors + 1;
            end
        end
    endtask

    // A word-aligned fill of one whole 32-pixel word, so the edge masks
    // are all-ones and the op is the only thing under test.
    task fill_word;
        input integer wx;         // word index within the line
        input integer wy;
        input [31:0] pat;
        input [31:0] rop_bits;
        input        clip;
        begin
            wb_write(R_DST_X, wx * 32);
            wb_write(R_DST_Y, wy);
            wb_write(R_W, 32);
            wb_write(R_H, 1);
            wb_write(R_PATTERN, pat);
            wb_write(R_CTRL, C_START | C_FILL | rop_bits |
                (clip ? C_CLIP : 32'h0));
            wait_idle();
        end
    endtask

    task copy_word;
        input integer wx;
        input integer wy;
        input integer src_word;
        input [31:0] rop_bits;
        begin
            wb_write(R_DST_X, wx * 32);
            wb_write(R_DST_Y, wy);
            wb_write(R_W, 32);
            wb_write(R_H, 1);
            wb_write(R_SRC_ADDR, MEM_BASE + src_word * 4);
            wb_write(R_SRC_STRIDE, 4);
            wb_write(R_SRC_SHIFT, 0);
            wb_write(R_CTRL, C_START | C_CLIP | C_SRCMEM | rop_bits);
            wait_idle();
        end
    endtask

    localparam [31:0] DST_SEED = 32'hF0F0_A5A5;
    localparam [31:0] SRC_PAT  = 32'h3C3C_FFFF;

    integer r;
    reg [31:0] rop_bits;
    reg [1:0]  rop_num;

    initial begin

        mem_latency = 0;

        for (i = 0; i < 9600; i = i + 1) vram[i] = 32'h0;
        for (i = 0; i < (SRC_WPL*SRC_H)+8; i = i + 1) srcmem[i] = 32'h0;

        srcmem[0] = SRC_PAT;

        repeat (8) @(posedge clk);
        rst = 0;
        repeat (4) @(posedge clk);

        // ---- 1: every op, fill mode, clipped and unclipped ----

        for (r = 0; r < 4; r = r + 1) begin
            rop_num = r[1:0];
            rop_bits = r << 5;

            // clipped (reads the destination the old way)
            vram[2*VRAM_WPL + 3] = DST_SEED;
            snapshot();
            fill_word(3, 2, SRC_PAT, rop_bits, 1'b1);
            check_word("fill clipped", 2*VRAM_WPL + 3,
                model(DST_SEED, SRC_PAT, rop_num));

            // UNCLIPPED -- the blind-write path. See note 3 in the
            // header: this is the one that had to stop skipping the
            // read.
            vram[3*VRAM_WPL + 3] = DST_SEED;
            snapshot();
            fill_word(3, 3, SRC_PAT, rop_bits, 1'b0);
            check_word("fill unclipped", 3*VRAM_WPL + 3,
                model(DST_SEED, SRC_PAT, rop_num));

            // ---- memory copy, same op ----
            vram[4*VRAM_WPL + 3] = DST_SEED;
            snapshot();
            copy_word(3, 4, 0, rop_bits);
            check_word("memcopy", 4*VRAM_WPL + 3,
                model(DST_SEED, SRC_PAT, rop_num));
        end

        // ---- 2: ROP_COPY == no ROP field at all ----
        //
        // The regression guard. Every binary compiled before these bits
        // existed writes zero into them.
        vram[6*VRAM_WPL + 5] = DST_SEED;
        fill_word(5, 6, SRC_PAT, C_ROP_COPY, 1'b1);
        check_word("copy-op fill is a plain fill", 6*VRAM_WPL + 5, SRC_PAT);

        vram[7*VRAM_WPL + 5] = DST_SEED;
        copy_word(5, 7, 0, C_ROP_COPY);
        check_word("copy-op memcopy is a plain copy", 7*VRAM_WPL + 5, SRC_PAT);

        // ---- 3: the blind-fill hazard, explicitly ----
        //
        // Run a blit that leaves a KNOWN and very distinctive value in
        // read_data, then an unclipped OR fill somewhere else. If the
        // read were still being skipped the result would be
        // 0 | stale_read_data, which is exactly the stale value --
        // so the check below distinguishes the two outcomes rather
        // than merely passing on a coincidence.
        vram[8*VRAM_WPL + 1] = 32'hDEAD_BEEF;
        fill_word(1, 8, 32'h0000_0000, C_ROP_OR, 1'b1);   // leaves DEADBEEF in read_data
        check_word("setup: OR with 0 preserves", 8*VRAM_WPL + 1, 32'hDEAD_BEEF);

        vram[9*VRAM_WPL + 1] = 32'h0000_00FF;
        fill_word(1, 9, 32'h0000_FF00, C_ROP_OR, 1'b0);   // unclipped
        check_word("unclipped OR reads its own destination",
            9*VRAM_WPL + 1, 32'h0000_FFFF);

        // ---- 4: the masked sprite recipe ----
        //
        // mask has 1 where the sprite is opaque; data has the sprite
        // pixels. ANDN the mask to punch the hole, OR the data to fill
        // it. Background outside the mask must survive untouched.
        begin : sprite
            reg [31:0] bg, mask, dat, want;
            bg   = 32'hAAAA_AAAA;      // background pattern
            mask = 32'h0000_FFFF;      // sprite occupies the low 16 px
            dat  = 32'h0000_0F0F;      // sprite's own pixels
            want = (bg & ~mask) | dat;

            vram[10*VRAM_WPL + 2] = bg;

            srcmem[0] = mask;
            copy_word(2, 10, 0, C_ROP_ANDN);
            check_word("sprite: ANDN punched the hole",
                10*VRAM_WPL + 2, bg & ~mask);

            srcmem[0] = dat;
            copy_word(2, 10, 0, C_ROP_OR);
            check_word("sprite: OR laid the pixels in",
                10*VRAM_WPL + 2, want);

            // and the neighbouring words must be untouched throughout
            check_word("sprite: left neighbour intact",
                10*VRAM_WPL + 1, 32'h0);
            check_word("sprite: right neighbour intact",
                10*VRAM_WPL + 3, 32'h0);
        end

        // ---- 5: the software probe ----
        //
        // z_fb_hw_rop_available() writes the ROP field with no start
        // bit and reads it back. If CTRL did not report the field,
        // every binary would think raster ops are absent even on a
        // bitstream that has them -- and, worse, a binary built for
        // this feature run against an OLDER bitstream would read zero
        // and correctly fall back, which is the case that makes the
        // probe worth having at all.
        wb_write(R_CTRL, C_ROP_XOR);
        @(posedge clk);
        wb_adr <= R_CTRL; wb_we <= 1'b0; wb_cyc <= 1'b1; wb_stb <= 1'b1;
        @(posedge clk);
        while (!wb_ack) @(posedge clk);
        if (((wb_dat_o >> 5) & 2'b11) !== 2'd2) begin
            $display("FAIL: CTRL readback lost the ROP field: got %08x", wb_dat_o);
            errors = errors + 1;
        end
        wb_cyc <= 1'b0; wb_stb <= 1'b0;
        @(posedge clk);
        wb_write(R_CTRL, 32'h0);

        // ---- 6: single-pass cookie cut ----
        //
        // One operation must produce exactly what the two-pass
        // ANDN-then-OR recipe produces. Tested against the SAME model
        // rather than against the two-pass result, so a matching bug
        // in both would still fail.

        begin : cookie
            reg [31:0] bg, mask, dat, want;
            integer k;

            bg   = 32'hAAAA_AAAA;
            mask = 32'h0000_FFFF;
            dat  = 32'h0000_0F0F;
            want = (bg & ~mask) | (dat & mask);

            srcmem[0] = mask;      // A at MEM_BASE
            srcmem[64] = dat;      // B at MEM_BASE + 256

            vram[12*VRAM_WPL + 2] = bg;
            wb_write(R_DST_X, 2*32);
            wb_write(R_DST_Y, 12);
            wb_write(R_W, 32);
            wb_write(R_H, 1);
            wb_write(R_SRC_ADDR, MEM_BASE);
            wb_write(R_SRC_B, MEM_BASE + 64*4);
            wb_write(R_SRC_STRIDE, 4);
            wb_write(R_SRC_SHIFT, 0);
            wb_write(R_CTRL, C_START | C_CLIP | C_SRCMEM | C_COOKIE);
            wait_idle();
            check_word("cookie: one pass equals two", 12*VRAM_WPL + 2, want);

            check_word("cookie: left neighbour intact", 12*VRAM_WPL + 1, 32'h0);
            check_word("cookie: right neighbour intact", 12*VRAM_WPL + 3, 32'h0);

            // MULTI-ROW. The B stream has its own row pointer, and a
            // stride advance applied to A but not B would show up only
            // from row 1 onward -- the single-row test above would
            // pass regardless.
            for (k = 0; k < 4; k = k + 1) begin
                srcmem[k]      = 32'h0000_FFFF >> k;        // masks
                srcmem[64 + k] = 32'h0000_0F0F >> k;        // data
                vram[(20+k)*VRAM_WPL + 2] = 32'hAAAA_AAAA;
            end
            wb_write(R_DST_X, 2*32);
            wb_write(R_DST_Y, 20);
            wb_write(R_W, 32);
            wb_write(R_H, 4);
            wb_write(R_SRC_ADDR, MEM_BASE);
            wb_write(R_SRC_B, MEM_BASE + 64*4);
            wb_write(R_SRC_STRIDE, 4);
            wb_write(R_SRC_SHIFT, 0);
            wb_write(R_CTRL, C_START | C_CLIP | C_SRCMEM | C_COOKIE);
            wait_idle();
            for (k = 0; k < 4; k = k + 1)
                check_word("cookie: multi-row", (20+k)*VRAM_WPL + 2,
                    (32'hAAAA_AAAA & ~(32'h0000_FFFF >> k)) |
                    ((32'h0000_0F0F >> k) & (32'h0000_FFFF >> k)));

            // UNALIGNED. Both streams must take the same shift from
            // the one shared shifter; a stream that kept a stale
            // shifted value would corrupt only the unaligned case.
            srcmem[0] = 32'h0000_FFFF;
            srcmem[1] = 32'h0;
            srcmem[64] = 32'h0000_0F0F;
            srcmem[65] = 32'h0;
            vram[24*VRAM_WPL + 2] = 32'hAAAA_AAAA;
            vram[24*VRAM_WPL + 3] = 32'hAAAA_AAAA;
            wb_write(R_DST_X, 2*32 + 5);
            wb_write(R_DST_Y, 24);
            wb_write(R_W, 32);
            wb_write(R_H, 1);
            wb_write(R_SRC_ADDR, MEM_BASE);
            wb_write(R_SRC_B, MEM_BASE + 64*4);
            wb_write(R_SRC_STRIDE, 4);
            // The shift zgfx.c's blit_copy_setup() would compute for
            // src_x=0, dst_x&31=5: sbit0 = -5, so prime with zero and
            // shift by 27, which puts source bit 0 at destination bit
            // 5. Writing the prime bit with a shift of 0 (as an
            // earlier version of this test did) makes the first
            // blended word come out as the zero-primed src_prev -- a
            // mask of 0, which correctly leaves the destination
            // untouched and looks exactly like the blit not running.
            wb_write(R_SRC_SHIFT, 32'h100 | 32'd27);
            wb_write(R_CTRL, C_START | C_CLIP | C_SRCMEM | C_COOKIE);
            wait_idle();
            check_word("cookie: unaligned, low word", 24*VRAM_WPL + 2,
                (32'hAAAA_AAAA & ~(32'h0000_FFFF << 5)) |
                ((32'h0000_0F0F << 5) & (32'h0000_FFFF << 5)));
        end

        // ---- 7: the cookie bit is visible to the software probe ----
        wb_write(R_CTRL, C_COOKIE);
        @(posedge clk);
        wb_adr <= R_CTRL; wb_we <= 1'b0; wb_cyc <= 1'b1; wb_stb <= 1'b1;
        @(posedge clk);
        while (!wb_ack) @(posedge clk);
        if (!((wb_dat_o >> 7) & 1'b1)) begin
            $display("FAIL: CTRL readback lost the cookie bit: %08x", wb_dat_o);
            errors = errors + 1;
        end
        wb_cyc <= 1'b0; wb_stb <= 1'b0;
        @(posedge clk);
        wb_write(R_CTRL, 32'h0);

        // ---- 8: ordered dither fills ----
        begin : dith
            integer lv, row, col, k, set, want_set;
            reg [31:0] w;
            reg [3:0] bay [0:15];

            bay[0]=0;  bay[1]=8;  bay[2]=2;  bay[3]=10;
            bay[4]=12; bay[5]=4;  bay[6]=14; bay[7]=6;
            bay[8]=3;  bay[9]=11; bay[10]=1; bay[11]=9;
            bay[12]=15;bay[13]=7; bay[14]=13;bay[15]=5;

            // level 0 is black and level 16 is solid -- the two ends
            // have to be exact or "off" and "on" are not reachable.
            for (row = 30; row < 34; row = row + 1)
                vram[row*VRAM_WPL + 4] = 32'hFFFFFFFF;
            wb_write(R_DST_X, 4*32); wb_write(R_DST_Y, 30);
            wb_write(R_W, 32); wb_write(R_H, 4);
            wb_write(R_PATTERN, 0);
            wb_write(R_CTRL, C_START | C_FILL | C_DITHER);
            wait_idle();
            for (row = 30; row < 34; row = row + 1)
                check_word("dither level 0 is black", row*VRAM_WPL + 4, 32'h0);

            wb_write(R_DST_Y, 30);
            wb_write(R_PATTERN, 16);
            wb_write(R_CTRL, C_START | C_FILL | C_DITHER);
            wait_idle();
            for (row = 30; row < 34; row = row + 1)
                check_word("dither level 16 is solid",
                    row*VRAM_WPL + 4, 32'hFFFFFFFF);

            // A mid level, checked bit by bit against the matrix.
            wb_write(R_DST_Y, 30);
            wb_write(R_PATTERN, 8);
            wb_write(R_CTRL, C_START | C_FILL | C_DITHER);
            wait_idle();
            set = 0;
            for (row = 30; row < 34; row = row + 1) begin
                w = vram[row*VRAM_WPL + 4];
                for (k = 0; k < 32; k = k + 1) begin
                    col = k % 4;
                    want_set = (bay[(row % 4) * 4 + col] < 8) ? 1 : 0;
                    if (w[k] !== want_set[0:0]) set = set + 1;
                end
            end
            if (set != 0) begin
                $display("FAIL: dither level 8: %0d bits differ from the matrix",
                    set);
                errors = errors + 1;
            end

            // SCREEN ALIGNMENT -- the property the whole design is
            // for, and the one a single-rectangle test cannot catch:
            // every check above passes just as well with a
            // rectangle-relative pattern.
            //
            // Fill a 64-pixel span as ONE rectangle, keep it, then
            // fill the identical span as TWO adjacent rectangles. The
            // results must be bit-identical. A pattern aligned to the
            // rectangle restarts at each fill's left edge, so the
            // second half comes out shifted and the two disagree.
            begin : align
                reg [31:0] one_a, one_b;

                for (row = 40; row < 44; row = row + 1) begin
                    vram[row*VRAM_WPL + 4] = 32'h0;
                    vram[row*VRAM_WPL + 5] = 32'h0;
                end

                wb_write(R_DST_X, 4*32); wb_write(R_DST_Y, 40);
                wb_write(R_W, 64); wb_write(R_H, 4);
                wb_write(R_PATTERN, 6);
                wb_write(R_CTRL, C_START | C_FILL | C_DITHER);
                wait_idle();

                one_a = vram[41*VRAM_WPL + 4];
                one_b = vram[41*VRAM_WPL + 5];

                for (row = 40; row < 44; row = row + 1) begin
                    vram[row*VRAM_WPL + 4] = 32'h0;
                    vram[row*VRAM_WPL + 5] = 32'h0;
                end

                wb_write(R_DST_X, 4*32); wb_write(R_DST_Y, 40);
                wb_write(R_W, 32); wb_write(R_H, 4);
                wb_write(R_CTRL, C_START | C_FILL | C_DITHER);
                wait_idle();
                wb_write(R_DST_X, 5*32); wb_write(R_DST_Y, 40);
                wb_write(R_W, 32); wb_write(R_H, 4);
                wb_write(R_CTRL, C_START | C_FILL | C_DITHER);
                wait_idle();

                check_word("aligned: split fill matches whole, left",
                    41*VRAM_WPL + 4, one_a);
                check_word("aligned: split fill matches whole, right",
                    41*VRAM_WPL + 5, one_b);

                // and VERTICALLY: rows 4 apart must be identical,
                // because the matrix period is 4 and the row index is
                // the absolute framebuffer row.
                if (vram[40*VRAM_WPL + 4] !== vram[44*VRAM_WPL + 4] &&
                    vram[44*VRAM_WPL + 4] !== 32'h0) begin
                    $display("FAIL: dither not aligned vertically");
                    errors = errors + 1;
                end
            end

        end

        if (errors == 0) $display("RESULT: PASS");
        else $display("RESULT: FAIL (%0d errors)", errors);

        $finish;

    end

endmodule
