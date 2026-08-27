/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Performance benchmark for rtl/cache.v (wb_icache)
 *
 * Measures cycles for a loop-structured instruction fetch trace with
 * the cache enabled vs disabled, against a fixed-latency slave that
 * models rtl/mem/sdram.v (~11 cycles/word) or rtl/mem/qqspi.v
 * (~63 cycles/word).
 *
 * The trace is loops, not a linear sweep, because a linear sweep
 * measures only fill bandwidth and would understate a cache badly --
 * real code spends most of its fetches re-executing the same
 * instructions, which is the entire thing a cache exploits.
 *
 * Run: iverilog -g2005 -o tb_perf rtl/tb/tb_cache_perf.v rtl/cache.v
 *      ./tb_perf
 */

`timescale 1ns / 1ps

module tb_cache_perf;

    localparam CACHE_KB   = 8;
    localparam LINE_WORDS = 4;
    localparam MEM_WORDS  = 8192;
    localparam MEM_BASE   = 32'h4000_0000;

    // slave latency in cycles: 11 ~= sdram.v, 63 ~= qqspi.v
    parameter SLAVE_LAT = 11;

    reg clk;
    reg rst;

    reg [31:0] c_adr;
    reg [31:0] c_dat_o;
    wire [31:0] c_dat_i;
    reg c_we;
    reg [3:0] c_sel;
    reg c_stb;
    reg c_cyc;
    reg c_instr;
    wire c_ack;

    wire [31:0] m_adr;
    wire [31:0] m_dat_o;
    reg [31:0] m_dat_i;
    wire m_we;
    wire [3:0] m_sel;
    wire m_stb;
    wire m_cyc;
    reg m_ack;

    reg [31:0] cfg_adr;
    reg [31:0] cfg_dat_o;
    wire [31:0] cfg_dat_i;
    reg cfg_we;
    reg cfg_stb;
    reg cfg_cyc;
    wire cfg_ack;

    reg [31:0] mem [0:MEM_WORDS-1];
    reg [31:0] slave_cnt;
    reg slave_busy;

    integer i;
    integer j;
    integer k;
    integer cyc_on;
    integer cyc_off;
    reg [31:0] cycle_count;
    reg counting;
    reg [31:0] hits;
    reg [31:0] misses;
    real ratio;
    real hitrate;

    wb_icache #(
        .CACHE_KB(CACHE_KB),
        .LINE_WORDS(LINE_WORDS)
    ) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(c_adr), .c_dat_i(c_dat_o), .c_dat_o(c_dat_i),
        .c_we_i(c_we), .c_sel_i(c_sel), .c_stb_i(c_stb),
        .c_cyc_i(c_cyc), .c_instr_i(c_instr), .c_ack_o(c_ack),
        .m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i),
        .m_we_o(m_we), .m_sel_o(m_sel), .m_stb_o(m_stb),
        .m_cyc_o(m_cyc), .m_ack_i(m_ack),
        .cfg_adr_i(cfg_adr), .cfg_dat_i(cfg_dat_o),
        .cfg_dat_o(cfg_dat_i), .cfg_we_i(cfg_we),
        .cfg_stb_i(cfg_stb), .cfg_cyc_i(cfg_cyc), .cfg_ack_o(cfg_ack)
    );

    initial clk = 1'b0;
    always #5 clk = ~clk;

    always @(posedge clk) begin
        if (rst) cycle_count <= 0;
        else if (counting) cycle_count <= cycle_count + 1;
    end

    // fixed-latency slave
    always @(posedge clk) begin
        if (rst) begin
            m_ack <= 1'b0;
            slave_busy <= 1'b0;
            slave_cnt <= 0;
            m_dat_i <= 32'b0;
        end else begin
            m_ack <= 1'b0;
            if (m_cyc && m_stb && !slave_busy && !m_ack) begin
                slave_busy <= 1'b1;
                slave_cnt <= 0;
            end else if (slave_busy) begin
                slave_cnt <= slave_cnt + 1;
                if (slave_cnt >= SLAVE_LAT) begin
                    m_dat_i <= mem[(m_adr - MEM_BASE) >> 2];
                    m_ack <= 1'b1;
                    slave_busy <= 1'b0;
                end
            end
        end
    end

    task cpu_fetch;
        input [31:0] a;
        begin
            @(posedge clk);
            c_adr <= a;
            c_we <= 1'b0;
            c_sel <= 4'b1111;
            c_instr <= 1'b1;
            c_stb <= 1'b1;
            c_cyc <= 1'b1;
            @(posedge clk);
            while (!c_ack) @(posedge clk);
            c_stb <= 1'b0;
            c_cyc <= 1'b0;
            c_instr <= 1'b0;
            @(posedge clk);
        end
    endtask

    task cfg_write;
        input [31:0] a;
        input [31:0] d;
        begin
            @(posedge clk);
            cfg_adr <= a; cfg_dat_o <= d; cfg_we <= 1'b1;
            cfg_stb <= 1'b1; cfg_cyc <= 1'b1;
            @(posedge clk);
            while (!cfg_ack) @(posedge clk);
            cfg_stb <= 1'b0; cfg_cyc <= 1'b0; cfg_we <= 1'b0;
            @(posedge clk);
        end
    endtask

    task cfg_read;
        input [31:0] a;
        output [31:0] d;
        begin
            @(posedge clk);
            cfg_adr <= a; cfg_we <= 1'b0;
            cfg_stb <= 1'b1; cfg_cyc <= 1'b1;
            @(posedge clk);
            while (!cfg_ack) @(posedge clk);
            d = cfg_dat_i;
            cfg_stb <= 1'b0; cfg_cyc <= 1'b0;
            @(posedge clk);
        end
    endtask

    // Fetch trace: an outer loop over an inner loop, roughly the
    // shape of a renderer -- a small hot body executed many times,
    // with an occasional excursion to a different routine.
    task run_trace;
        begin
            for (j = 0; j < 40; j = j + 1) begin
                // inner loop body: 24 instructions, 60 iterations
                for (k = 0; k < 60; k = k + 1)
                    for (i = 0; i < 24; i = i + 1)
                        cpu_fetch(MEM_BASE + 32'h0400 + (i * 4));
                // called routine: 40 instructions elsewhere
                for (i = 0; i < 40; i = i + 1)
                    cpu_fetch(MEM_BASE + 32'h2000 + (i * 4));
            end
        end
    endtask

    initial begin
        c_adr = 0; c_dat_o = 0; c_we = 0; c_sel = 0;
        c_stb = 0; c_cyc = 0; c_instr = 0;
        cfg_adr = 0; cfg_dat_o = 0; cfg_we = 0;
        cfg_stb = 0; cfg_cyc = 0;
        counting = 0;

        for (i = 0; i < MEM_WORDS; i = i + 1)
            mem[i] = 32'hC0DE_0000 + i;

        rst = 1'b1;
        repeat (8) @(posedge clk);
        rst = 1'b0;
        repeat (4) @(posedge clk);

        // --- cache disabled ---
        cfg_write(32'd0, 32'h0);
        repeat (4) @(posedge clk);
        counting = 1;
        run_trace;
        counting = 0;
        cyc_off = cycle_count;

        // --- cache enabled ---
        rst = 1'b1;
        repeat (8) @(posedge clk);
        rst = 1'b0;
        repeat (4) @(posedge clk);
        cfg_write(32'd0, 32'h3);
        repeat (4) @(posedge clk);
        counting = 1;
        run_trace;
        counting = 0;
        cyc_on = cycle_count;

        cfg_read(32'd1, hits);
        cfg_read(32'd2, misses);

        ratio = $itor(cyc_off) / $itor(cyc_on);
        hitrate = 100.0 * $itor(hits) / $itor(hits + misses);

        $display("");
        $display("=================================================");
        $display(" slave latency   : %0d cycles/word", SLAVE_LAT);
        $display(" cache           : %0d KB, %0d-word lines",
            CACHE_KB, LINE_WORDS);
        $display(" fetches         : %0d", hits + misses);
        $display("-------------------------------------------------");
        $display(" cycles, cache off : %0d", cyc_off);
        $display(" cycles, cache on  : %0d", cyc_on);
        $display(" hits / misses     : %0d / %0d", hits, misses);
        $display(" hit rate          : %0.2f %%", hitrate);
        $display(" fetch speedup     : %0.2f x", ratio);
        $display("=================================================");
        $finish;
    end

    initial begin
        #200_000_000;
        $display("timeout");
        $stop;
    end

endmodule
