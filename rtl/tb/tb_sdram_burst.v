/*
 * tb_sdram_burst.v -- rtl/mem/sdram_kianv.v against rtl/tb/sdram_model.v
 *
 * A Wishbone master issues random single reads and writes (random
 * byte lanes, both read conventions: sel=0000 and sel=1111) and, with
 * BURST=1, 4- and 8-word incrementing bursts (CTI=010, last 111),
 * across several rows and banks so row misses, precharges and
 * refreshes interleave with everything. Every read is checked against
 * a reference; every protocol error the model detects fails the test.
 *
 * With BURST=0 the bursts are still issued, and must still work: the
 * controller ignores CTI and acks each beat as a single read. That is
 * the "BURST=0 behaves as before" check.
 *
 *   iverilog -g2005 -o tb_sb rtl/tb/tb_sdram_burst.v \
 *       rtl/mem/sdram_kianv.v rtl/tb/sdram_model.v && ./tb_sb
 *   iverilog -g2005 -Ptb_sdram_burst.BURST=1 ...
 */

`timescale 1ns / 1ps

module tb_sdram_burst;

    parameter BURST = 1;
    parameter KEEP_OPEN = 1;
    parameter SEED = 1;
    parameter NOPS = 6000;
    parameter CAS = 2;
    parameter EXTRA = 0;

    localparam WORDS = 16384;        // 64KB tested window

    reg clk;
    reg rst;

    reg  [24:0] adr;
    reg  [31:0] dat_w;
    wire [31:0] dat_r;
    reg  we;
    reg  [3:0] sel;
    reg  stb;
    reg  cyc;
    reg  [2:0] cti;
    wire ack;

    wire sdram_clk, sdram_cke, sdram_csn, sdram_wen, sdram_rasn, sdram_casn;
    wire [1:0] sdram_dqm, sdram_ba;
    wire [12:0] sdram_addr;
    wire [15:0] sdram_dq;

    sdram_wb #(
        .SDRAM_CLK_FREQ(48),
        .KEEP_OPEN(KEEP_OPEN),
        .BURST(BURST),
        .CAS(CAS),
        .READ_EXTRA_CYC(EXTRA)
    ) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
        .wb_we_i(we), .wb_sel_i(sel), .wb_stb_i(stb), .wb_ack_o(ack),
        .wb_cyc_i(cyc), .wb_cti_i(cti),
        .sdram_clk(sdram_clk), .sdram_cke(sdram_cke),
        .sdram_dqm(sdram_dqm), .sdram_addr(sdram_addr),
        .sdram_ba(sdram_ba), .sdram_csn(sdram_csn),
        .sdram_wen(sdram_wen), .sdram_rasn(sdram_rasn),
        .sdram_casn(sdram_casn), .sdram_dq(sdram_dq)
    );

    sdram_model #(.MEM_WORDS(65536)) mem (
        .clk(sdram_clk), .cke(sdram_cke), .csn(sdram_csn),
        .rasn(sdram_rasn), .casn(sdram_casn), .wen(sdram_wen),
        .dqm(sdram_dqm), .ba(sdram_ba), .addr(sdram_addr), .dq(sdram_dq)
    );

    initial clk = 1'b0;
    always #10 clk = ~clk;          // ~48MHz

    reg [31:0] ref [0:WORDS-1];
    integer seed;
    integer errors;
    integer i;
    integer n;
    integer b;
    integer op;
    integer cyc_start;
    integer burst_cycles;
    integer bursts;
    integer single_cycles;
    integer singles;
    integer timeout;
    integer now;
    reg [31:0] a;
    reg [31:0] w;
    reg [31:0] base;
    integer nbeats;

    always @(posedge clk) now = now + 1;

    task wait_ack;
    begin
        timeout = 0;
        @(posedge clk);
        while (!ack) begin
            timeout = timeout + 1;
            if (timeout > 2000) begin
                $display("TIMEOUT adr=%h", adr); $finish;
            end
            @(posedge clk);
        end
    end
    endtask

    task single;
        input [31:0] ad;
        input wr;
        input [3:0] s;
        input [31:0] d;
    begin
        @(posedge clk);
        adr <= ad[24:0]; we <= wr; sel <= s; dat_w <= d; cti <= 3'b000;
        stb <= 1'b1; cyc <= 1'b1;
        cyc_start = now;
        wait_ack;
        single_cycles = single_cycles + (now - cyc_start);
        singles = singles + 1;
        stb <= 1'b0; cyc <= 1'b0; we <= 1'b0;
        if (wr) begin
            w = ref[ad[15:2]];
            if (s[0]) w[7:0]   = d[7:0];
            if (s[1]) w[15:8]  = d[15:8];
            if (s[2]) w[23:16] = d[23:16];
            if (s[3]) w[31:24] = d[31:24];
            ref[ad[15:2]] = w;
        end else if (dat_r !== ref[ad[15:2]]) begin
            if (errors < 20)
                $display("READ ERROR adr=%h got=%h exp=%h", ad, dat_r, ref[ad[15:2]]);
            errors = errors + 1;
        end
    end
    endtask

    // Burst like wb_cache: STB held, address advanced on each ack.
    task burst;
        input [31:0] ad;       // 16-byte aligned
        input integer beats;   // 4 or 8
    begin
        @(posedge clk);
        adr <= ad[24:0]; we <= 1'b0; sel <= 4'b0000; cti <= 3'b010;
        stb <= 1'b1; cyc <= 1'b1;
        cyc_start = now;
        for (b = 0; b < beats; b = b + 1) begin
            wait_ack;
            if (dat_r !== ref[(ad[15:2] + b) & (WORDS-1)]) begin
                if (errors < 20)
                    $display("BURST ERROR base=%h beat=%0d got=%h exp=%h",
                        ad, b, dat_r, ref[(ad[15:2] + b) & (WORDS-1)]);
                errors = errors + 1;
            end
            adr <= adr + 25'd4;
            cti <= (b == beats - 2) ? 3'b111 : 3'b010;
            if (b == beats - 1) begin
                stb <= 1'b0; cyc <= 1'b0; cti <= 3'b000;
            end
        end
        burst_cycles = burst_cycles + (now - cyc_start);
        bursts = bursts + 1;
    end
    endtask

    // addresses: mostly a few rows in two banks' worth of window, so
    // both row hits and row misses are common
    function [31:0] raddr;
        input dummy;
    begin
        raddr = (($unsigned($random(seed)) % WORDS) * 4);
        if ($random(seed) & 1) raddr = raddr & 32'h0000_0fff;
    end
    endfunction

    initial begin
        seed = SEED;
        errors = 0; now = 0;
        burst_cycles = 0; bursts = 0; single_cycles = 0; singles = 0;
        adr = 0; dat_w = 0; we = 0; sel = 0; stb = 0; cyc = 0; cti = 0;
        rst = 1'b1;
        repeat (10) @(posedge clk);
        rst = 1'b0;
        // wait out the 200us power-up (plus init sequence)
        repeat (9800) @(posedge clk);

        // fill the window with known data through the controller
        for (i = 0; i < WORDS; i = i + 1) ref[i] = 32'h0;
        for (i = 0; i < 1024; i = i + 1)
            single(i * 4, 1'b1, 4'b1111, $random(seed));
        for (i = 0; i < 1024; i = i + 1)
            single(i * 4 + 32'h8000, 1'b1, 4'b1111, $random(seed));

        for (n = 0; n < NOPS; n = n + 1) begin
            op = $unsigned($random(seed)) % 10;
            a = raddr(0);
            if (op < 3) begin
                single(a, 1'b1, $random(seed), $random(seed));
            end else if (op < 6) begin
                single(a, 1'b0, ($random(seed) & 1) ? 4'b1111 : 4'b0000, 0);
            end else if (op < 9) begin
                burst(a & ~32'hf, 4);
            end else begin
                burst(a & ~32'h1f, 8);
            end
        end

        // report
        $display("  singles %0d avg %0d.%02d cyc, bursts %0d avg %0d.%02d cyc",
            singles, single_cycles / singles, (single_cycles * 100 / singles) % 100,
            bursts, burst_cycles / bursts, (burst_cycles * 100 / bursts) % 100);
        $display("  model: %0d READ, %0d WRITE commands, %0d protocol errors",
            mem.reads, mem.writes, mem.errors);
        errors = errors + mem.errors;
        if (errors == 0)
            $display("PASS tb_sdram_burst BURST=%0d KEEP_OPEN=%0d seed=%0d", BURST, KEEP_OPEN, SEED);
        else
            $display("FAIL tb_sdram_burst: %0d errors", errors);
        $finish;
    end

endmodule
