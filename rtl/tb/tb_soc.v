/*
 * Cycle-accurate IPC comparison: real picorv32_wb, real wb_icache,
 * against a memory model that reproduces rtl/mem/sram.v's actual ack
 * timing (a FREE-RUNNING ack generator, not a request-triggered one --
 * see the model below, that detail turns out to matter a lot).
 *
 * This exists because rtl/tb/tb_cache_perf.v measures the wrong
 * baseline for this question: its "cache off" path still routes
 * through wb_icache's S_BYPASS, whereas a board built without `ICACHE
 * has no module in the path at all. Comparing against bypass flatters
 * the cache by roughly a cycle per fetch.
 *
 * The program is a hand-assembled copy of k_cpu_report()'s benchmark
 * loop (sw/os/kernel.c): pure register work, no loads or stores, so
 * essentially every bus cycle is an instruction fetch. That makes it a
 * near-pure measurement of fetch latency, which is exactly what the
 * cache changes -- and exactly what the reported MIPS figure reflects.
 *
 * Run: iverilog -g2005 -DUSE_CACHE -o tb tb_soc.v cache.v picorv32.v
 */

`timescale 1ns / 1ps

module tb_soc;

    parameter MEM_LAT = 0;      // 0 = model sram.v, else fixed N cycles
    parameter RUN_CYCLES = 200000;

    localparam MEM_WORDS = 4096;
    localparam MEM_BASE  = 32'h4000_0000;

    reg clk;
    reg rst;

    // cpu <-> (cache) <-> memory
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

    wire [31:0] m_adr;
    wire [31:0] m_dat_o;
    reg [31:0] m_dat_i;
    wire m_we;
    wire [3:0] m_sel;
    wire m_stb;
    wire m_cyc;
    reg m_ack;

    wire [31:0] cfg_dat_o;
    wire cfg_ack;

    reg [31:0] mem [0:MEM_WORDS-1];

    integer cycles;
    integer fetches;
    // Counts fetches of one chosen address. IPC alone cannot compare
    // two programs that do the same WORK with different instruction
    // counts -- a software multiply retires ~200 instructions where a
    // MUL retires one, so both can show identical IPC while one is
    // 30x slower. Counting arrivals at the top of the loop body counts
    // useful work instead.
    integer marker_hits;
    reg [31:0] marker_adr;
    integer i;
    real ipc;
    real mips;
    reg [1023:0] progfile;

    picorv32_wb #(
        .STACKADDR(32'h4000_8000),
        .PROGADDR_RESET(MEM_BASE),
        .BARREL_SHIFTER(1),
        .COMPRESSED_ISA(0),
`ifdef CPU_MUL_FAST
        .ENABLE_MUL(0),
        .ENABLE_FAST_MUL(1),
`else
        .ENABLE_MUL(0),
        .ENABLE_FAST_MUL(0),
`endif
`ifdef CPU_DIV
        .ENABLE_DIV(1),
`else
        .ENABLE_DIV(0),
`endif
        .ENABLE_IRQ(1),
        .ENABLE_IRQ_TIMER(0),
        .ENABLE_IRQ_QREGS(1)
    ) cpu (
        .wb_clk_i(clk),
        .wb_rst_i(rst),
        .wbm_adr_o(cpu_adr),
        .wbm_dat_o(cpu_dat_o),
        .wbm_dat_i(cpu_dat_i),
        .wbm_we_o(cpu_we),
        .wbm_sel_o(cpu_sel),
        .wbm_stb_o(cpu_stb),
        .wbm_ack_i(cpu_ack),
        .wbm_cyc_o(cpu_cyc),
        .trap(cpu_trap),
        .irq(32'b0),
        .mem_instr(cpu_instr)
    );

`ifdef USE_CACHE
    wb_icache #(
        .CACHE_KB(`CACHE_KB),
        .LINE_WORDS(`LINE_WORDS)
    ) icache (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(cpu_adr), .c_dat_i(cpu_dat_o), .c_dat_o(cpu_dat_i),
        .c_we_i(cpu_we), .c_sel_i(cpu_sel), .c_stb_i(cpu_stb),
        .c_cyc_i(cpu_cyc), .c_instr_i(cpu_instr), .c_ack_o(cpu_ack),
        .m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i),
        .m_we_o(m_we), .m_sel_o(m_sel), .m_stb_o(m_stb),
        .m_cyc_o(m_cyc), .m_ack_i(m_ack),
        .cfg_adr_i(32'b0), .cfg_dat_i(32'b0), .cfg_dat_o(cfg_dat_o),
        .cfg_we_i(1'b0), .cfg_stb_i(1'b0), .cfg_cyc_i(1'b0),
        .cfg_ack_o(cfg_ack)
    );
`else
    // no cache module at all -- exactly what a board built without
    // `ICACHE gets
    assign m_adr = cpu_adr;
    assign m_dat_o = cpu_dat_o;
    assign cpu_dat_i = m_dat_i;
    assign m_we = cpu_we;
    assign m_sel = cpu_sel;
    assign m_stb = cpu_stb;
    assign m_cyc = cpu_cyc;
    assign cpu_ack = m_ack;
`endif

    initial clk = 1'b0;
    always #5 clk = ~clk;

    // -- memory model ---------------------------------------------
    //
    // MEM_LAT == 0 reproduces rtl/mem/sram.v exactly. Note what that
    // module actually does: its ack generator is FREE-RUNNING and
    // never looks at wb_cyc_i or wb_stb_i at all --
    //
    //   if (wb_ack_o)          wb_ack_o <= 0;
    //   else if (ack_pending)  wb_ack_o <= 1; ack_pending <= 0;
    //   else                   ack_pending <= 1;
    //
    // so an ack pulse is always somewhere in a 3-cycle rotation,
    // independent of when a request arrives. A request that lands on
    // the right phase is acked almost immediately. This is why SRAM
    // reads are so cheap here, and why anything that adds fixed
    // latency in front of it is a net loss.

    reg sram_ack_pending;
    reg sram_ack;
    reg [31:0] lat_cnt;
    reg lat_busy;

    always @(posedge clk) begin
        if (rst) begin
            sram_ack <= 1'b0;
            sram_ack_pending <= 1'b0;
        end else begin
            if (sram_ack) begin
                sram_ack <= 1'b0;
            end else if (sram_ack_pending) begin
                sram_ack <= 1'b1;
                sram_ack_pending <= 1'b0;
            end else begin
                sram_ack_pending <= 1'b1;
            end
        end
    end

    always @(posedge clk) begin
        if (rst) begin
            m_ack <= 1'b0;
            m_dat_i <= 32'b0;
            lat_busy <= 1'b0;
            lat_cnt <= 0;
        end else if (MEM_LAT == 0) begin
            m_ack <= m_cyc && m_stb && sram_ack;
            m_dat_i <= mem[(m_adr - MEM_BASE) >> 2];
        end else begin
            m_ack <= 1'b0;
            if (m_cyc && m_stb && !lat_busy && !m_ack) begin
                lat_busy <= 1'b1;
                lat_cnt <= 0;
            end else if (lat_busy) begin
                lat_cnt <= lat_cnt + 1;
                if (lat_cnt >= (MEM_LAT-1)) begin
                    m_dat_i <= mem[(m_adr - MEM_BASE) >> 2];
                    m_ack <= 1'b1;
                    lat_busy <= 1'b0;
                end
            end
        end
    end

    // -- measurement ----------------------------------------------
    //
    // picorv32 fetches exactly once per instruction, so counting
    // acknowledged instruction-fetch cycles at the CPU port counts
    // retired instructions -- with or without a cache in the way.

    always @(posedge clk) begin
        if (!rst) begin
            cycles <= cycles + 1;
            if (cpu_cyc && cpu_stb && cpu_ack && cpu_instr) begin
                fetches <= fetches + 1;
                if (cpu_adr == marker_adr) marker_hits <= marker_hits + 1;
            end
        end
    end

    initial begin
        cycles = 0;
        fetches = 0;
        marker_hits = 0;
        if (!$value$plusargs("marker=%h", marker_adr))
            marker_adr = 32'hffff_ffff;
        for (i = 0; i < MEM_WORDS; i = i + 1) mem[i] = 32'h00000013;
        if (!$value$plusargs("prog=%s", progfile)) progfile = "prog.hex";
        $readmemh(progfile, mem);

        rst = 1'b1;
        repeat (16) @(posedge clk);
        rst = 1'b0;

        while (cycles < RUN_CYCLES) @(posedge clk);

        ipc = $itor(fetches) / $itor(cycles);
        mips = ipc * 48.0;

        $display("%0s lat=%0s : cycles=%0d insns=%0d IPC=%0.3f MIPS=%0.2f CPI=%0.2f",
`ifdef USE_CACHE
            "cache  ",
`else
            "nocache",
`endif
            (MEM_LAT == 0) ? "sram" : "fixed",
            cycles, fetches, ipc, mips, 1.0/ipc);

        if (marker_adr != 32'hffff_ffff)
            $display("    work: %0d marker hits, %0.1f cycles each",
                marker_hits,
                (marker_hits > 0) ? $itor(cycles)/$itor(marker_hits) : 0.0);
        if (cpu_trap) $display("  (CPU TRAPPED)");
        $finish;
    end

endmodule
