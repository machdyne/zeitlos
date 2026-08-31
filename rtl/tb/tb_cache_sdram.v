/*
 * Integration testbench: rtl/cache.v -> rtl/mem/sdram_kianv.v -> sdram_model
 *
 * rtl/tb/tb_cache.v exercises the cache against a synthetic wishbone
 * slave that acks on demand. That slave has no banks, no open rows, no
 * CAS latency, and no opinion about whether CYC is held -- so a cache
 * that violates the SDRAM protocol passes it. The cache does pass it,
 * in every configuration, while failing to boot on hardware.
 *
 * This connects the real controller and a protocol-checking memory
 * model, so what is simulated is what is built.
 *
 * Run:
 *   iverilog -g2005 -o tb tb_cache_sdram.v ../cache.v \
 *            ../mem/sdram_kianv.v sdram_model.v
 *   ./tb
 */

`timescale 1ns / 1ps

module tb_cache_sdram;

    localparam CACHE_KB   = 4;
    localparam LINE_WORDS = 2;
    parameter  FAST_HIT   = 1;

    localparam MEM_BASE = 32'h4000_0000;

    reg clk = 0;
    reg rst = 1;

    // CPU side of the cache
    reg [31:0] c_adr = 0, c_dat_o = 0;
    wire [31:0] c_dat_i;
    reg c_we = 0, c_stb = 0, c_cyc = 0, c_instr = 0;
    reg [3:0] c_sel = 4'hF;
    wire c_ack, cfg_hit;

    // cache -> controller
    wire [31:0] m_adr, m_dat_o;
    wire [31:0] m_dat_i;
    wire m_we, m_stb, m_cyc, m_ack;
    wire [3:0] m_sel;

    // controller -> SDRAM pins
    wire sdram_clk, sdram_cke, sdram_csn, sdram_wen, sdram_rasn, sdram_casn;
    wire [1:0] sdram_dqm, sdram_ba;
    wire [12:0] sdram_addr;
    wire [15:0] sdram_dq;

    always #5 clk = ~clk;

    wb_icache #(
        .CACHE_KB(CACHE_KB),
        .LINE_WORDS(LINE_WORDS),
        .FAST_HIT(FAST_HIT)
    ) cache (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(c_adr), .c_dat_i(c_dat_o), .c_dat_o(c_dat_i),
        .c_we_i(c_we), .c_sel_i(c_sel), .c_stb_i(c_stb),
        .c_cyc_i(c_cyc), .c_instr_i(c_instr), .c_ack_o(c_ack),
        .m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i),
        .m_we_o(m_we), .m_sel_o(m_sel), .m_stb_o(m_stb),
        .m_cyc_o(m_cyc), .m_ack_i(m_ack),
        .c_cfg_hit(cfg_hit)
    );

    sdram_wb sdram (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(m_adr[24:0]), .wb_dat_i(m_dat_o), .wb_dat_o(m_dat_i),
        .wb_we_i(m_we), .wb_sel_i(m_sel), .wb_stb_i(m_stb),
        .wb_ack_o(m_ack), .wb_cyc_i(m_cyc),
        .sdram_clk(sdram_clk), .sdram_cke(sdram_cke),
        .sdram_dqm(sdram_dqm), .sdram_addr(sdram_addr), .sdram_ba(sdram_ba),
        .sdram_csn(sdram_csn), .sdram_wen(sdram_wen),
        .sdram_rasn(sdram_rasn), .sdram_casn(sdram_casn),
        .sdram_dq(sdram_dq)
    );

    sdram_model #(.CAS_LATENCY(2)) mem (
        .clk(sdram_clk), .cke(sdram_cke), .csn(sdram_csn),
        .rasn(sdram_rasn), .casn(sdram_casn), .wen(sdram_wen),
        .dqm(sdram_dqm), .ba(sdram_ba), .addr(sdram_addr), .dq(sdram_dq)
    );

    // -- CPU-side fetch --------------------------------------------

    reg [31:0] got;
    integer errors, checks, i;
    integer timeout;

    task fetch;
        input [31:0] a;
        begin
            @(posedge clk);
            c_adr <= a; c_we <= 0; c_sel <= 4'hF;
            c_instr <= 1'b1; c_stb <= 1'b1; c_cyc <= 1'b1;
            @(posedge clk);
            timeout = 0;
            while (!c_ack && timeout < 10000) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            if (timeout >= 10000) begin
                $display("FAIL: fetch @%08x TIMED OUT -- no ack", a);
                errors = errors + 1;
            end
            got = c_dat_i;
            c_stb <= 1'b0; c_cyc <= 1'b0; c_instr <= 1'b0;
            @(posedge clk);
        end
    endtask

    // expected value for a word address, matching the preload below
    function [31:0] expect_at;
        input [31:0] byte_addr;
        begin
            expect_at = 32'hC0DE_0000 + (byte_addr >> 2);
        end
    endfunction

    initial begin
        errors = 0; checks = 0;

        // Preload: each 32-bit word is C0DE_xxxx. The controller reads
        // 16 bits at a time, low half first, so seed the model in the
        // same order it will be read back.
        for (i = 0; i < 2048; i = i + 1) begin
            mem.preload(i*2,     (32'hC0DE_0000 + i) & 16'hFFFF);
            mem.preload(i*2 + 1, (32'hC0DE_0000 + i) >> 16);
        end

        repeat (10) @(posedge clk);
        rst = 0;

        // the controller needs its power-up/init sequence to finish
        repeat (20000) @(posedge clk);

        $display("-- sequential fetches through cache + real controller --");
        for (i = 0; i < 32; i = i + 1) begin
            fetch(MEM_BASE + i*4);
            checks = checks + 1;
            if (got !== expect_at(i*4)) begin
                $display("FAIL @%08x: got %08x expected %08x",
                    MEM_BASE + i*4, got, expect_at(i*4));
                errors = errors + 1;
            end
        end

        $display("-- re-fetch (should hit, no bus traffic) --");
        for (i = 0; i < 32; i = i + 1) begin
            fetch(MEM_BASE + i*4);
            checks = checks + 1;
            if (got !== expect_at(i*4)) begin
                $display("FAIL(warm) @%08x: got %08x expected %08x",
                    MEM_BASE + i*4, got, expect_at(i*4));
                errors = errors + 1;
            end
        end

        $display("-- scattered fetches (force misses across rows) --");
        for (i = 0; i < 32; i = i + 1) begin
            fetch(MEM_BASE + ((i * 397) & 2047) * 4);
            checks = checks + 1;
            if (got !== expect_at(((i * 397) & 2047) * 4)) begin
                $display("FAIL(scatter) got %08x", got);
                errors = errors + 1;
            end
        end

        $display("");
        $display("=====================================");
        $display(" checks       : %0d", checks);
        $display(" cache errors : %0d", errors);
        $display(" sdram errors : %0d", mem.errors);
        $display(" RESULT       : %s",
            (errors == 0 && mem.errors == 0) ? "PASS" : "FAIL");
        $display("=====================================");
        $finish;
    end

    initial begin
        #50_000_000;
        $display("FAIL: global timeout");
        $finish;
    end

endmodule
