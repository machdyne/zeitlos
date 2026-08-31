/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/cache.v (wb_icache)
 *
 * Every read issued through the cache is checked against a reference
 * model of what memory actually holds, so a stale or misaddressed hit
 * fails loudly rather than silently returning plausible garbage.
 *
 * The downstream slave model deliberately uses a RANDOMIZED ack
 * latency (1..12 cycles). Both real backends have variable latency --
 * rtl/mem/sdram.v's ack timing shifts depending on where its refresh
 * loop happens to be, and rtl/mem/qqspi.v differs between read and
 * write -- so a cache that only works against a fixed-latency slave
 * would be a cache that only works in simulation.
 *
 * Run: iverilog -g2005 -o tb_cache rtl/tb/tb_cache.v rtl/cache.v
 *      ./tb_cache
 */

`timescale 1ns / 1ps

module tb_cache;

    localparam CACHE_KB   = 8;
    localparam LINE_WORDS = 4;
    // overridable: iverilog -Ptb_cache.FAST_HIT=0
    parameter  FAST_HIT   = 1;

    localparam MEM_WORDS  = 8192;      // 32KB of model memory
    localparam MEM_BASE   = 32'h4000_0000;

    reg clk;
    reg rst;

    // cpu side
    reg [31:0] c_adr;
    reg [31:0] c_dat_o;
    wire [31:0] c_dat_i;
    reg c_we;
    reg [3:0] c_sel;
    reg c_stb;
    reg c_cyc;
    reg c_instr;
    wire c_ack;

    // memory side
    wire [31:0] m_adr;
    wire [31:0] m_dat_o;
    reg [31:0] m_dat_i;
    wire m_we;
    wire [3:0] m_sel;
    wire m_stb;
    wire m_cyc;
    reg m_ack;

    // config side
    reg [31:0] cfg_adr;
    reg [31:0] cfg_dat_o;
    wire [31:0] cfg_dat_i;
    reg cfg_we;
    reg cfg_stb;
    reg cfg_cyc;
    wire cfg_ack;

    // reference model + slave model state
    reg [31:0] mem [0:MEM_WORDS-1];
    reg [31:0] slave_lat;
    reg [31:0] slave_cnt;
    reg slave_busy;

    // bookkeeping
    integer errors;
    integer checks;
    integer mem_reads;
    integer i;
    integer j;
    integer seed;
    reg [31:0] got;
    reg [31:0] expect_val;
    reg [31:0] addr;
    reg [31:0] hits_before;
    reg [31:0] hits_after;
    reg [31:0] misses_before;
    reg [31:0] misses_after;

    wb_icache #(
        .CACHE_KB(CACHE_KB),
        .LINE_WORDS(LINE_WORDS),
        .FAST_HIT(FAST_HIT),
        .FAST_HIT(FAST_HIT)
    ) dut (
        .wb_clk_i(clk),
        .wb_rst_i(rst),

        .c_adr_i(c_adr),
        .c_dat_i(c_dat_o),
        .c_dat_o(c_dat_i),
        .c_we_i(c_we),
        .c_sel_i(c_sel),
        .c_stb_i(c_stb),
        .c_cyc_i(c_cyc),
        .c_instr_i(c_instr),
        .c_ack_o(c_ack),

        .m_adr_o(m_adr),
        .m_dat_o(m_dat_o),
        .m_dat_i(m_dat_i),
        .m_we_o(m_we),
        .m_sel_o(m_sel),
        .m_stb_o(m_stb),
        .m_cyc_o(m_cyc),
        .m_ack_i(m_ack),

        .c_cfg_hit(cfg_hit)
    );

    // -- clock -----------------------------------------------------

    initial clk = 1'b0;
    always #5 clk = ~clk;

    // -- downstream slave model ------------------------------------
    //
    // Randomized latency, and it counts every access that reaches it.
    // That count is the real measure of whether the cache is working:
    // a cache that returns correct data while still going to memory
    // every time is correct and useless.

    always @(posedge clk) begin
        if (rst) begin
            m_ack <= 1'b0;
            m_dat_i <= 32'b0;
            slave_busy <= 1'b0;
            slave_cnt <= 0;
            slave_lat <= 1;
        end else begin
            m_ack <= 1'b0;

            if (m_cyc && m_stb && !slave_busy && !m_ack) begin
                slave_busy <= 1'b1;
                slave_lat <= 1 + ({$random(seed)} % 12);
                slave_cnt <= 0;
            end else if (slave_busy) begin
                slave_cnt <= slave_cnt + 1;
                if (slave_cnt >= slave_lat) begin
                    if (m_we) begin
                        if (m_sel[0]) mem[(m_adr - MEM_BASE) >> 2][7:0]
                            <= m_dat_o[7:0];
                        if (m_sel[1]) mem[(m_adr - MEM_BASE) >> 2][15:8]
                            <= m_dat_o[15:8];
                        if (m_sel[2]) mem[(m_adr - MEM_BASE) >> 2][23:16]
                            <= m_dat_o[23:16];
                        if (m_sel[3]) mem[(m_adr - MEM_BASE) >> 2][31:24]
                            <= m_dat_o[31:24];
                    end else begin
                        m_dat_i <= mem[(m_adr - MEM_BASE) >> 2];
                    end
                    m_ack <= 1'b1;
                    slave_busy <= 1'b0;
                    mem_reads = mem_reads + 1;
                end
            end
        end
    end

    // -- CYC continuity monitor ------------------------------------
    //
    // The original testbench passed both before and after the bug that
    // stopped the machine booting, because a synthetic slave does not
    // care whether CYC is held. Real hardware does, twice over:
    // arbiter_main only moves the grant when the current master drops
    // CYC, and sdram_kianv gates its ack on CYC while tracking open
    // rows across a burst.
    //
    // So assert the protocol directly: once a multi-word fill starts,
    // CYC must stay asserted until the line is complete.
    integer fill_cyc_drops;
    reg in_fill;

    initial begin fill_cyc_drops = 0; in_fill = 0; end

    always @(posedge clk) begin
        if (!rst) begin
            // a fill is running whenever the DUT is in S_FILL/S_FILL_SEQ
            if (dut.state == 3'd2 || dut.state == 3'd3) begin
                if (in_fill && !m_cyc) fill_cyc_drops = fill_cyc_drops + 1;
                in_fill <= 1'b1;
            end else begin
                in_fill <= 1'b0;
            end
        end
    end

    // -- bus tasks -------------------------------------------------

    task cpu_read;
        input [31:0] a;
        input instr;
        begin
            @(posedge clk);
            c_adr <= a;
            c_we <= 1'b0;
            c_sel <= 4'b1111;
            c_instr <= instr;
            c_stb <= 1'b1;
            c_cyc <= 1'b1;
            @(posedge clk);
            while (!c_ack) @(posedge clk);
            got = c_dat_i;
            c_stb <= 1'b0;
            c_cyc <= 1'b0;
            c_instr <= 1'b0;
            @(posedge clk);
        end
    endtask

    task cpu_write;
        input [31:0] a;
        input [31:0] d;
        begin
            @(posedge clk);
            c_adr <= a;
            c_dat_o <= d;
            c_we <= 1'b1;
            c_sel <= 4'b1111;
            c_instr <= 1'b0;
            c_stb <= 1'b1;
            c_cyc <= 1'b1;
            @(posedge clk);
            while (!c_ack) @(posedge clk);
            c_stb <= 1'b0;
            c_cyc <= 1'b0;
            c_we <= 1'b0;
            @(posedge clk);
        end
    endtask

    // Registers are now reached through the CPU port at CFG_BASE,
    // upstream of the bus -- see rtl/cache.v. These accesses must NOT
    // produce any activity on the memory side; test 11 checks that.
    localparam [31:0] CFG_BASE = 32'h7000_0100;

    task cfg_write;
        input [31:0] a;
        input [31:0] d;
        begin
            @(posedge clk);
            c_adr <= CFG_BASE | (a << 2);
            c_dat_o <= d;
            c_we <= 1'b1;
            c_sel <= 4'b1111;
            c_instr <= 1'b0;
            c_stb <= 1'b1;
            c_cyc <= 1'b1;
            @(posedge clk);
            while (!c_ack) @(posedge clk);
            c_stb <= 1'b0;
            c_cyc <= 1'b0;
            c_we <= 1'b0;
            @(posedge clk);
        end
    endtask

    task cfg_read;
        input [31:0] a;
        output [31:0] d;
        begin
            @(posedge clk);
            c_adr <= CFG_BASE | (a << 2);
            c_we <= 1'b0;
            c_sel <= 4'b1111;
            c_instr <= 1'b0;
            c_stb <= 1'b1;
            c_cyc <= 1'b1;
            @(posedge clk);
            while (!c_ack) @(posedge clk);
            d = c_dat_i;
            c_stb <= 1'b0;
            c_cyc <= 1'b0;
            @(posedge clk);
        end
    endtask

    task check_fetch;
        input [31:0] a;
        begin
            cpu_read(a, 1'b1);
            expect_val = mem[(a - MEM_BASE) >> 2];
            checks = checks + 1;
            if (got !== expect_val) begin
                errors = errors + 1;
                $display("FAIL fetch @%08x: got %08x expected %08x",
                    a, got, expect_val);
            end
        end
    endtask

    // -- stimulus --------------------------------------------------

    initial begin
        errors = 0;
        checks = 0;
        mem_reads = 0;
        seed = 12345;

        c_adr = 0; c_dat_o = 0; c_we = 0; c_sel = 0;
        c_stb = 0; c_cyc = 0; c_instr = 0;

        for (i = 0; i < MEM_WORDS; i = i + 1)
            mem[i] = 32'hC0DE_0000 + i;

        rst = 1'b1;
        repeat (8) @(posedge clk);
        rst = 1'b0;
        repeat (4) @(posedge clk);

        // --- 1: sequential fetch, cold then warm ------------------
        $display("-- test 1: sequential fetch");
        for (i = 0; i < 64; i = i + 1)
            check_fetch(MEM_BASE + (i * 4));

        mem_reads = 0;
        for (i = 0; i < 64; i = i + 1)
            check_fetch(MEM_BASE + (i * 4));
        $display("   64 warm fetches -> %0d memory accesses (expect 0)",
            mem_reads);
        if (mem_reads != 0) begin
            errors = errors + 1;
            $display("FAIL: warm sequential fetch still hitting memory");
        end

        // --- 2: line granularity ----------------------------------
        // One cold fetch should pull in a whole line, so the other
        // LINE_WORDS-1 words of that line must cost nothing.
        $display("-- test 2: line fill granularity");
        cfg_write(32'd0, 32'h3);      // enable + flush
        repeat (4) @(posedge clk);
        mem_reads = 0;
        check_fetch(MEM_BASE + 32'h1000);
        if (mem_reads != LINE_WORDS) begin
            errors = errors + 1;
            $display("FAIL: cold fetch took %0d accesses, expected %0d",
                mem_reads, LINE_WORDS);
        end
        mem_reads = 0;
        for (i = 1; i < LINE_WORDS; i = i + 1)
            check_fetch(MEM_BASE + 32'h1000 + (i * 4));
        if (mem_reads != 0) begin
            errors = errors + 1;
            $display("FAIL: rest of line cost %0d accesses", mem_reads);
        end

        // --- 3: every word offset within a line -------------------
        // Catches critical-word selection bugs: the word handed back
        // must be the one asked for, not the first or last of the line.
        $display("-- test 3: word offset within line");
        for (j = 0; j < LINE_WORDS; j = j + 1) begin
            cfg_write(32'd0, 32'h3);
            repeat (4) @(posedge clk);
            check_fetch(MEM_BASE + 32'h2000 + (j * 4));
        end

        // --- 4: conflict thrashing --------------------------------
        // Two addresses with the same index and different tags, so
        // every fetch must miss and refill. A tag comparison that is
        // too narrow shows up here as wrong data.
        $display("-- test 4: index conflict");
        for (i = 0; i < 20; i = i + 1) begin
            check_fetch(MEM_BASE + 32'h0040);
            check_fetch(MEM_BASE + 32'h0040 + (CACHE_KB * 1024));
        end

        // --- 5: data reads must NOT be cached ---------------------
        // c_instr low means bypass. If a data read were cached it
        // could later be served to a fetch, or go stale after a write.
        $display("-- test 5: data reads bypass");
        mem_reads = 0;
        for (i = 0; i < 8; i = i + 1)
            cpu_read(MEM_BASE + 32'h0000, 1'b0);
        if (mem_reads != 8) begin
            errors = errors + 1;
            $display("FAIL: data reads cached (%0d accesses for 8 reads)",
                mem_reads);
        end

        // --- 6: uncacheable region bypasses -----------------------
        // Peripherals live outside 0x4xxx_xxxx and must never be
        // cached; caching a status register would break them.
        $display("-- test 6: non-main-memory bypasses");
        mem_reads = 0;
        for (i = 0; i < 4; i = i + 1)
            cpu_read(32'hf000_0014, 1'b1);
        if (mem_reads != 4) begin
            errors = errors + 1;
            $display("FAIL: uncacheable region was cached");
        end

        // --- 7: stale line after code overwrite, and the flush ----
        // This is the exact fs_load_exec()/BIOS scenario: code is
        // rewritten as data at an address already cached, then
        // executed. Without a flush the old code comes back.
        $display("-- test 7: flush after code overwrite");
        cfg_write(32'd0, 32'h3);
        repeat (4) @(posedge clk);
        check_fetch(MEM_BASE + 32'h3000);

        mem[32'h3000 >> 2] = 32'hDEAD_BEEF;   // reference model
        cpu_write(MEM_BASE + 32'h3000, 32'hDEAD_BEEF);

        cpu_read(MEM_BASE + 32'h3000, 1'b1);
        if (got === 32'hDEAD_BEEF) begin
            $display("   note: line happened not to be resident");
        end else begin
            $display("   stale line observed pre-flush (expected): %08x",
                got);
        end

        cfg_write(32'd0, 32'h3);              // flush
        repeat (4) @(posedge clk);
        check_fetch(MEM_BASE + 32'h3000);     // must now see new code

        // --- 8: runtime disable -----------------------------------
        // The debug escape hatch. With the cache off every fetch must
        // reach memory, so it can be ruled in or out on hardware
        // without a re-synthesis.
        $display("-- test 8: runtime disable");
        cfg_write(32'd0, 32'h0);              // disable, no flush
        repeat (4) @(posedge clk);
        mem_reads = 0;
        for (i = 0; i < 8; i = i + 1)
            check_fetch(MEM_BASE + 32'h0000 + (i * 4));
        if (mem_reads != 8) begin
            errors = errors + 1;
            $display("FAIL: disabled cache served %0d of 8 from cache",
                8 - mem_reads);
        end
        cfg_write(32'd0, 32'h1);              // re-enable
        repeat (4) @(posedge clk);

        // --- 9: hit/miss counters ---------------------------------
        $display("-- test 9: hit/miss counters");
        cfg_write(32'd0, 32'h3);
        repeat (4) @(posedge clk);
        for (i = 0; i < 16; i = i + 1)
            check_fetch(MEM_BASE + 32'h4000 + (i * 4));
        cfg_read(32'd1, hits_after);
        cfg_read(32'd2, misses_after);
        $display("   16 sequential fetches: %0d hits, %0d misses",
            hits_after, misses_after);
        if ((hits_after + misses_after) != 16) begin
            errors = errors + 1;
            $display("FAIL: counters total %0d, expected 16",
                hits_after + misses_after);
        end
        if (misses_after != (16 / LINE_WORDS)) begin
            errors = errors + 1;
            $display("FAIL: %0d misses, expected %0d",
                misses_after, 16 / LINE_WORDS);
        end

        // --- 10: random address soak ------------------------------
        // The real check: random fetches interleaved with writes that
        // change memory under the cache, each followed by a flush,
        // all verified against the reference model.
        $display("-- test 10: random soak");
        for (i = 0; i < 1500; i = i + 1) begin
            addr = MEM_BASE + (({$random(seed)} % MEM_WORDS) * 4);
            check_fetch(addr);

            if (({$random(seed)} % 20) == 0) begin
                addr = MEM_BASE + (({$random(seed)} % MEM_WORDS) * 4);
                expect_val = {$random(seed)};
                mem[(addr - MEM_BASE) >> 2] = expect_val;
                cpu_write(addr, expect_val);
                cfg_write(32'd0, 32'h3);
                repeat (2) @(posedge clk);
            end
        end

        // --- 11: register access must NOT touch the memory bus ----
        //
        // This is the regression test for the deadlock that stopped the
        // machine booting. The registers used to be a slave on the main
        // bus, so a write to them was forwarded down the bypass path,
        // through wb_arbiter_main, and back to this module's own slave
        // port -- which was busy waiting for that very transaction.
        //
        // The invariant is simple and worth asserting directly: an
        // access to CFG_BASE produces ZERO activity on the memory side.
        $display("-- test 11: register access stays off the memory bus");
        mem_reads = 0;
        cfg_read(32'd3, got);
        cfg_write(32'd0, 32'h3);
        cfg_read(32'd1, hits_after);
        repeat (4) @(posedge clk);
        $display("   4 register accesses -> %0d memory accesses (expect 0)",
            mem_reads);
        if (mem_reads != 0) begin
            errors = errors + 1;
            $display("FAIL: register access reached the memory bus");
        end

        // --- 12: CYC held for the whole line fill -----------------
        $display("-- test 12: CYC held across multi-word line fills");
        $display("   CYC drops mid-fill: %0d (expect 0)", fill_cyc_drops);
        if (fill_cyc_drops != 0) begin
            errors = errors + 1;
            $display("FAIL: bus released mid-fill -- arbiter grant can be");
            $display("      lost and sdram open-row tracking broken");
        end

        // --- summary ----------------------------------------------
        $display("");
        $display("=====================================");
        $display(" checks : %0d", checks);
        $display(" errors : %0d", errors);
        if (errors == 0)
            $display(" RESULT : PASS");
        else
            $display(" RESULT : FAIL");
        $display("=====================================");

        if (errors != 0) $stop;
        $finish;
    end

    // watchdog: a hung handshake should fail, not spin forever
    initial begin
        #20_000_000;
        $display("FAIL: timeout");
        $stop;
    end

endmodule
