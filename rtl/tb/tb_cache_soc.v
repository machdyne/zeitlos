/*
 * tb_cache_soc.v -- real CPU + cache + real SDRAM controller + model
 *
 *   picorv32_wb or zeitlos32_wb
 *     -> (nothing | wb_icache | wb_cache)
 *     -> address decode: 0x4xxx_xxxx -> sdram_wb -> sdram_model
 *                         anything else -> a small I/O slave
 *
 * Runs rtl/tb/cache_soc/prog.hex (build it with `make` in that
 * directory), which exercises code loading without a flush (on
 * wb_cache), sorting, byte memcpy, pointer chasing, strings and
 * recursion, then reports a checksum. The checksum must be identical
 * for every configuration; the per-phase cycle counts are the
 * performance comparison.
 *
 * Defines:
 *   CPU_Z32        use zeitlos32 (default picorv32)
 *   CACHE=0|1|2    none | wb_icache | wb_cache      (default 2)
 *   BURST=0|1      sdram_kianv.v BURST and wb_cache BURST (default 0)
 *   WBUF=n         wb_cache write buffer depth       (default 2)
 *   IKB, DKB       array sizes (default 8, 4)
 *   SNOOP=0|1      wb_cache SNOOP parameter          (default 1)
 *   MPU            put rtl/mpu.v between the CPU and the cache. prog.c
 *                  then runs its whole workload under an enforcing MPU
 *                  and finishes with a small app, at 0x4000_8000, that
 *                  must have its stray store blocked (add rtl/mpu.v
 *                  to the file list)
 *   ARB            put the real rtl/arbiter_main.v between the cache
 *                  and memory, with sysctl.v's exact CTI and snoop
 *                  gating, plus two more masters: a blitter-like reader
 *                  checking every word it reads from the code region,
 *                  and a writer bumping a mailbox the program polls
 *                  through the data cache (phase 7 of prog.c)
 *
 * Example:
 *   cd rtl/tb/cache_soc && make && cd ../../..
 *   iverilog -g2005 -DCACHE=2 -DBURST=1 -o tb_soc2 rtl/tb/tb_cache_soc.v \
 *     rtl/cache_id.v rtl/cache.v rtl/mem/sdram_kianv.v rtl/tb/sdram_model.v \
 *     rtl/cpu/picorv32/picorv32.v rtl/cpu/zeitlos32/zeitlos32.v \
 *     rtl/cpu/zeitlos32/zeitlos32_muldiv.v rtl/arbiter_main.v
 *   vvp tb_soc2 +prog=rtl/tb/cache_soc/prog.hex
 */

`timescale 1ns / 1ps

`ifndef CACHE
`define CACHE 2
`endif
`ifndef BURST
`define BURST 0
`endif
`ifndef WBUF
`define WBUF 2
`endif
`ifndef IKB
`define IKB 8
`endif
`ifndef DKB
`define DKB 4
`endif
`ifndef SNOOP
`define SNOOP 1
`endif

module tb_cache_soc;

    reg clk;
    reg rst;
    reg cpu_rst;

    // CPU
    wire [31:0] cpu_adr;
    wire [31:0] cpu_dat_o;
    wire [31:0] cpu_dat_i;
    wire cpu_we;
    wire [3:0] cpu_sel;
    wire cpu_stb;
    wire cpu_cyc;
    wire cpu_ack;
    wire cpu_instr;
    wire cpu_trap;

    // bus after the cache
    wire [31:0] b_adr;
    wire [31:0] b_dat_o;
    wire [31:0] b_dat_i;
    wire b_we;
    wire [3:0] b_sel;
    wire b_stb;
    wire b_cyc;
    wire [2:0] b_cti;
    wire b_ack;

    // main bus after the arbiter (= b_* without ARB). Declared here,
    // ahead of the cache instance that reads s_adr/snoop_stb, so they
    // are never implicit 1-bit nets.
    wire [31:0] s_adr;
    wire [31:0] s_dat_o;
    wire [31:0] s_dat_i;
    wire s_we;
    wire [3:0] s_sel;
    wire s_stb;
    wire s_cyc;
    wire [2:0] s_cti;
    wire s_ack;
    wire snoop_stb;

`ifdef CPU_Z32
    zeitlos32_wb #(
        .PROGADDR_RESET(32'h4000_0000),
        .PROGADDR_IRQ(32'h4000_0010),
        .STACKADDR(32'h4001_0000),
        .ENABLE_MUL(1), .ENABLE_DIV(1), .FAST_MUL(1)
    ) cpu (
        .wb_clk_i(clk), .wb_rst_i(cpu_rst),
        .wbm_adr_o(cpu_adr), .mtu_base(32'h0),
        .wbm_dat_o(cpu_dat_o), .wbm_dat_i(cpu_dat_i),
        .wbm_we_o(cpu_we), .wbm_sel_o(cpu_sel), .wbm_stb_o(cpu_stb),
        .wbm_ack_i(cpu_ack), .wbm_cyc_o(cpu_cyc),
        .trap(cpu_trap), .irq(32'b0), .eoi(), .mem_instr(cpu_instr)
    );
`else
    picorv32_wb #(
        .STACKADDR(32'h4001_0000),
        .PROGADDR_RESET(32'h4000_0000),
        .BARREL_SHIFTER(1),
        .COMPRESSED_ISA(0),
        .ENABLE_MUL(0), .ENABLE_FAST_MUL(1), .ENABLE_DIV(1),
        .ENABLE_IRQ(1), .ENABLE_IRQ_TIMER(0), .ENABLE_IRQ_QREGS(1)
    ) cpu (
        .wb_clk_i(clk), .wb_rst_i(cpu_rst),
        .wbm_adr_o(cpu_adr), .wbm_dat_o(cpu_dat_o), .wbm_dat_i(cpu_dat_i),
        .wbm_we_o(cpu_we), .wbm_sel_o(cpu_sel), .wbm_stb_o(cpu_stb),
        .wbm_ack_i(cpu_ack), .wbm_cyc_o(cpu_cyc),
        .trap(cpu_trap), .irq(32'b0), .mem_instr(cpu_instr)
    );
`endif

    // -- MPU (optional) -----------------------------------------------
    //
    // Between the CPU and the cache, as in rtl/sysctl.v. No MTU in this
    // testbench, so addresses are physical (XLATE_ON_BUS=0, the
    // zeitlos32 arrangement) and the "current app" is fixed at
    // 0x4000_8000.
    wire cpu_stb_c;
    wire cpu_cyc_c;
    wire cpu_ack_c;
    wire [31:0] cpu_dat_i_c;
    wire mpu_irq;
`ifdef MPU
    wb_mpu #(.XLATE_ON_BUS(0)) mpu (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(cpu_adr), .c_dat_i(cpu_dat_o), .c_we_i(cpu_we),
        .c_sel_i(cpu_sel), .c_stb_i(cpu_stb), .c_cyc_i(cpu_cyc),
        .c_instr_i(cpu_instr), .c_dat_o(cpu_dat_i), .c_ack_o(cpu_ack),
        .d_stb_o(cpu_stb_c), .d_cyc_o(cpu_cyc_c),
        .d_dat_i(cpu_dat_i_c), .d_ack_i(cpu_ack_c),
        .mtu_base_i(32'h4000_8000), .irq_o(mpu_irq)
    );
`else
    assign cpu_stb_c = cpu_stb;
    assign cpu_cyc_c = cpu_cyc;
    assign cpu_ack = cpu_ack_c;
    assign cpu_dat_i = cpu_dat_i_c;
    assign mpu_irq = 1'b0;
`endif

    // -- cache ------------------------------------------------------

    // With a cache, its own register window is answered upstream. On
    // a build without one, the I/O slave below plays csrs_wb's role
    // and absorbs 0x7000_01xx (acks, reads 0), exactly as sysctl.v
    // does.

    generate
        if (`CACHE == 2) begin : g_unified
            wb_cache #(
                .I_KB(`IKB), .I_LINE_WORDS(4),
                .D_KB(`DKB), .D_LINE_WORDS(4),
                .FAST_HIT(1), .WBUF_DEPTH(`WBUF), .BURST(`BURST), .SNOOP(`SNOOP)
            ) cache (
                .wb_clk_i(clk), .wb_rst_i(rst),
                .c_adr_i(cpu_adr), .c_dat_i(cpu_dat_o), .c_dat_o(cpu_dat_i_c),
                .c_we_i(cpu_we), .c_sel_i(cpu_sel), .c_stb_i(cpu_stb_c),
                .c_cyc_i(cpu_cyc_c), .c_instr_i(cpu_instr), .c_ack_o(cpu_ack_c),
                .m_adr_o(b_adr), .m_dat_o(b_dat_o), .m_dat_i(b_dat_i),
                .m_we_o(b_we), .m_sel_o(b_sel), .m_stb_o(b_stb),
                .m_cyc_o(b_cyc), .m_cti_o(b_cti), .m_bte_o(), .m_ack_i(b_ack),
                .snoop_stb_i(snoop_stb), .snoop_adr_i(s_adr),
                .c_cfg_hit()
            );
        end else if (`CACHE == 1) begin : g_icache
            wb_icache #(
                .CACHE_KB(`IKB), .LINE_WORDS(4), .FAST_HIT(1)
            ) cache (
                .wb_clk_i(clk), .wb_rst_i(rst),
                .c_adr_i(cpu_adr), .c_dat_i(cpu_dat_o), .c_dat_o(cpu_dat_i_c),
                .c_we_i(cpu_we), .c_sel_i(cpu_sel), .c_stb_i(cpu_stb_c),
                .c_cyc_i(cpu_cyc_c), .c_instr_i(cpu_instr), .c_ack_o(cpu_ack_c),
                .m_adr_o(b_adr), .m_dat_o(b_dat_o), .m_dat_i(b_dat_i),
                .m_we_o(b_we), .m_sel_o(b_sel), .m_stb_o(b_stb),
                .m_cyc_o(b_cyc), .m_ack_i(b_ack),
                .c_cfg_hit()
            );
            assign b_cti = 3'b000;
        end else begin : g_none
            assign b_adr = cpu_adr;
            assign b_dat_o = cpu_dat_o;
            assign cpu_dat_i_c = b_dat_i;
            assign b_we = cpu_we;
            assign b_sel = cpu_sel;
            assign b_stb = cpu_stb_c;
            assign b_cyc = cpu_cyc_c;
            assign cpu_ack_c = b_ack;
            assign b_cti = 3'b000;
        end
    endgenerate

    // -- main bus: direct, or through wb_arbiter_main ------------------

    // (s_* and snoop_stb are declared above, before the cache uses them)

    // extra masters (driven only with ARB)
    reg  [31:0] r_adr;
    reg  r_stb;
    wire [31:0] r_dat;
    wire r_ack;
    reg  [31:0] w_adr;
    reg  [31:0] w_dat;
    reg  w_stb;
    wire w_ack;
    wire [1:0] marb_master;

`ifdef ARB
    wb_arbiter_main marb (
        .clk(clk), .rst(rst),
        .m0_adr_i(b_adr), .m0_dat_i(b_dat_o), .m0_dat_o(b_dat_i),
        .m0_we_i(b_we), .m0_sel_i(b_sel), .m0_stb_i(b_stb),
        .m0_cyc_i(b_cyc), .m0_ack_o(b_ack),
        // like gpu_blit.v's source port: we=0, sel=1111
        .m1_adr_i(r_adr), .m1_dat_i(32'h0), .m1_dat_o(r_dat),
        .m1_we_i(1'b0), .m1_sel_i(4'b1111), .m1_stb_i(r_stb),
        .m1_cyc_i(r_stb), .m1_ack_o(r_ack),
        // a writing master, which nothing in the real SOC is yet
        .m2_adr_i(w_adr), .m2_dat_i(w_dat), .m2_dat_o(),
        .m2_we_i(1'b1), .m2_sel_i(4'b1111), .m2_stb_i(w_stb),
        .m2_cyc_i(w_stb), .m2_ack_o(w_ack),
        .s_adr_o(s_adr), .s_dat_o(s_dat_o), .s_dat_i(s_dat_i),
        .s_we_o(s_we), .s_sel_o(s_sel), .s_stb_o(s_stb), .s_cyc_o(s_cyc),
        .s_ack_i(s_ack),
        .master(marb_master)
    );
    // exactly rtl/sysctl.v's wbm_cti and cache_snoop_stb
    assign s_cti = (marb_master == 2'd0) ? b_cti : 3'b000;
    assign snoop_stb = (marb_master != 2'd0) && s_cyc && s_stb &&
        s_we && s_ack && ((s_adr & 32'hf000_0000) == 32'h4000_0000);
`else
    assign s_adr = b_adr;
    assign s_dat_o = b_dat_o;
    assign b_dat_i = s_dat_i;
    assign s_we = b_we;
    assign s_sel = b_sel;
    assign s_stb = b_stb;
    assign s_cyc = b_cyc;
    assign s_cti = b_cti;
    assign b_ack = s_ack;
    assign snoop_stb = 1'b0;
    assign r_ack = 1'b0;
    assign w_ack = 1'b0;
    assign r_dat = 32'h0;
    assign marb_master = 2'd0;
`endif

    // -- decode -------------------------------------------------------

    wire cs_main = ((s_adr & 32'hf000_0000) == 32'h4000_0000);
    wire [31:0] sd_dat_o;
    wire sd_ack;
    reg  [31:0] io_dat_o;
    reg  io_ack;

    assign s_dat_i = cs_main ? sd_dat_o : io_dat_o;
    assign s_ack = cs_main ? sd_ack : io_ack;

    wire sdram_clk, sdram_cke, sdram_csn, sdram_wen, sdram_rasn, sdram_casn;
    wire [1:0] sdram_dqm, sdram_ba;
    wire [12:0] sdram_addr;
    wire [15:0] sdram_dq;

    sdram_wb #(
        .SDRAM_CLK_FREQ(48),
        .BURST(`BURST)
    ) sdram (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(s_adr[24:0]), .wb_dat_i(s_dat_o), .wb_dat_o(sd_dat_o),
        .wb_we_i(s_we), .wb_sel_i(s_sel), .wb_stb_i(s_stb && cs_main),
        .wb_ack_o(sd_ack), .wb_cyc_i(s_cyc && cs_main), .wb_cti_i(s_cti),
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

    // -- I/O slave: UART, DONE, MARK, FAIL; acks everything else ------

    integer cycles;
    integer mark_cyc [0:15];
    integer last_mark;
    integer fails;
    reg done;
    reg [31:0] result;

    always @(posedge clk) begin
        if (rst) begin
            io_ack <= 1'b0;
            io_dat_o <= 32'b0;
        end else begin
            io_ack <= 1'b0;
            if (s_cyc && s_stb && !cs_main && !io_ack) begin
                io_ack <= 1'b1;
`ifdef ARB
                io_dat_o <= (s_adr == 32'hf000_0010) ? 32'd1 : 32'b0;
`else
                io_dat_o <= 32'b0;
`endif
                if (s_we) begin
                    case (s_adr)
                        32'hf000_0000: $write("%c", s_dat_o[7:0]);
                        32'hf000_0004: begin done <= 1'b1; result <= s_dat_o; end
                        32'hf000_0008: begin
                            mark_cyc[s_dat_o[3:0]] = cycles;
                            last_mark = s_dat_o;
                        end
                        32'hf000_000c: fails = fails + 1;
                        default: ;
                    endcase
                end
            end
        end
    end

    initial clk = 1'b0;
    always #10 clk = ~clk;
    always @(posedge clk) cycles = cycles + 1;

    // -- extra masters (ARB only) ---------------------------------------
    //
    // Reader: single reads of the first TEXT_BYTES of the program, which
    // nothing writes after load, checked against the image. Writer: every
    // few hundred cycles, MAILBOX <= MAILBOX + 1.
    integer r_gap, w_gap, r_reads, r_errors, w_writes, snoops;
    reg [31:0] w_count;
    reg arb_seed_init;
    integer aseed;
    localparam TEXT_BYTES = 32'h400;
    initial begin
        r_stb = 0; w_stb = 0; r_adr = 0; w_adr = 32'h4000_f000; w_dat = 0;
        r_gap = 0; w_gap = 0; r_reads = 0; r_errors = 0; w_writes = 0;
        w_count = 0; snoops = 0; aseed = 99;
    end
`ifdef ARB
    always @(posedge clk) begin
        if (snoop_stb) snoops = snoops + 1;
        if (!cpu_rst) begin
            if (r_stb) begin
                if (r_ack) begin
                    r_reads = r_reads + 1;
                    if (r_dat !== prog[r_adr[15:2]]) begin
                        if (r_errors < 5)
                            $display("READER ERROR adr=%h got=%h exp=%h", r_adr, r_dat, prog[r_adr[15:2]]);
                        r_errors = r_errors + 1;
                    end
                    r_stb <= 1'b0;
                    r_gap = $unsigned($random(aseed)) % 24;
                end
            end else if (r_gap > 0) r_gap = r_gap - 1;
            else begin
                r_adr <= 32'h4000_0000 + (($unsigned($random(aseed)) % TEXT_BYTES) & ~32'h3);
                r_stb <= 1'b1;
            end

            if (w_stb) begin
                if (w_ack) begin
                    w_stb <= 1'b0;
                    w_writes = w_writes + 1;
                    w_gap = 200 + $unsigned($random(aseed)) % 400;
                end
            end else if (w_gap > 0) w_gap = w_gap - 1;
            else begin
                w_count = w_count + 1;
                w_dat <= w_count;
                w_stb <= 1'b1;
            end
        end
    end
`endif

    // CPU-side accounting, between MARK 1 and MARK 7
    integer n_fetch, n_load, n_store, n_wait, w_fetch, w_load, w_store;
    initial begin n_fetch = 0; n_load = 0; n_store = 0; n_wait = 0;
        w_fetch = 0; w_load = 0; w_store = 0; end
    always @(posedge clk) begin
        if (last_mark >= 1 && last_mark < 7 && cpu_cyc && cpu_stb) begin
            if (cpu_ack) begin
                if (cpu_instr) n_fetch = n_fetch + 1;
                else if (cpu_we) n_store = n_store + 1;
                else n_load = n_load + 1;
            end else begin
                n_wait = n_wait + 1;
                if (cpu_instr) w_fetch = w_fetch + 1;
                else if (cpu_we) w_store = w_store + 1;
                else w_load = w_load + 1;
            end
        end
    end

    reg [1023:0] progfile;
    reg [31:0] prog [0:16383];
    integer nwords;
    reg [31:0] expect_sum;
    integer i;
    integer p;

    initial begin
        cycles = 0; fails = 0; done = 0; result = 0; last_mark = 0;
        for (i = 0; i < 16; i = i + 1) mark_cyc[i] = 0;
        if (!$value$plusargs("prog=%s", progfile))
            progfile = "rtl/tb/cache_soc/prog.hex";
        for (i = 0; i < 16384; i = i + 1) prog[i] = 32'h0000_0013;
        $readmemh(progfile, prog);
        // program -> SDRAM model. For byte addresses below 2MB the
        // controller's {bank,row,col} mapping flattens to the 16-bit
        // word index addr/2, low half first.
        for (i = 0; i < 16384; i = i + 1) begin
            mem.mem[2*i]   = prog[i][15:0];
            mem.mem[2*i+1] = prog[i][31:16];
        end

        rst = 1'b1; cpu_rst = 1'b1;
        repeat (10) @(posedge clk);
        rst = 1'b0;
        // SDRAM power-up (200us) and the cache's reset flush
        repeat (9800) @(posedge clk);
        cycles = 0;
        cpu_rst = 1'b0;

        while (!done && cycles < 6000000 && !cpu_trap) @(posedge clk);
        repeat (20) @(posedge clk);

        $display("");
        $display("config: cpu=%0s cache=%0d burst=%0d wbuf=%0d I%0dK D%0dK",
`ifdef CPU_Z32
            "zeitlos32",
`else
            "picorv32",
`endif
            `CACHE, `BURST, `WBUF, `IKB, `DKB);
        for (p = 1; p < 7; p = p + 1)
            $display("  phase %0d: %0d cycles", p, mark_cyc[p+1] - mark_cyc[p]);
        $display("  total: %0d cycles", mark_cyc[7] - mark_cyc[1]);
        if (mem.errors != 0) $display("  SDRAM protocol errors: %0d", mem.errors);
`ifdef ARB
        $display("  arbiter: reader %0d reads %0d errors, writer %0d writes, %0d snoops",
            r_reads, r_errors, w_writes, snoops);
        fails = fails + r_errors;
`endif
        $display("  bus: %0d fetches, %0d loads, %0d stores, %0d cpu wait cycles",
            n_fetch, n_load, n_store, n_wait);
        $display("       fetch wait %0d, load wait %0d, store wait %0d",
            w_fetch, w_load, w_store);
`ifdef SHOW_STATS
        $display("  I hits %0d misses %0d  D hits %0d misses %0d  wbfull %0d",
            g_unified.cache.stat_i_hits, g_unified.cache.stat_i_misses,
            g_unified.cache.stat_d_hits, g_unified.cache.stat_d_misses,
            g_unified.cache.stat_wbfull);
`endif
        if (!$value$plusargs("expect=%h", expect_sum)) expect_sum = result;
        if (done && fails == 0 && mem.errors == 0 && !cpu_trap && result == expect_sum)
            $display("PASS tb_cache_soc result=%08x", result);
        else
            $display("FAIL tb_cache_soc done=%0d fails=%0d trap=%0d result=%08x expect=%08x",
                done, fails, cpu_trap, result, expect_sum);
        $finish;
    end

endmodule
