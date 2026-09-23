/*
 * tb_cache_id.v -- unit testbench for rtl/cache_id.v (wb_cache)
 *
 * Every read the "CPU" performs is checked against a reference model
 * of what memory holds, so a stale or misaddressed hit fails loudly.
 * The slave has randomized latency and supports CTI bursts.
 *
 * Checks, beyond plain read-after-write:
 *   - I-side store snooping: code overwritten by stores is never
 *     fetched stale, and this testbench NEVER flushes to make it so
 *   - write-buffer ordering: when an uncached write is acknowledged,
 *     every earlier store must already be in slave memory (that is
 *     what another master, e.g. the blitter, would see)
 *   - uncached reads of main memory see buffered stores
 *   - snoop input: memory changed behind the cache's back + snoop
 *     pulse is never read stale
 *   - Wishbone rule: address/data/we/sel stable while STB is waiting
 *   - runtime enable/disable of both sides and posted writes
 *
 * Run (defaults; override with -P):
 *   iverilog -g2005 -o tb_cid rtl/tb/tb_cache_id.v rtl/cache_id.v
 *   ./tb_cid
 *   iverilog -g2005 -Ptb_cache_id.BURST=1 -Ptb_cache_id.WBUF=4 ...
 */

`timescale 1ns / 1ps

module tb_cache_id;

    parameter I_KB   = 2;
    parameter I_LW   = 4;
    parameter D_KB   = 2;
    parameter D_LW   = 4;
    parameter FAST   = 1;
    parameter WBUF   = 2;
    parameter BURST  = 0;
    parameter SEED   = 1;
    parameter NOPS   = 40000;

    localparam MEM_WORDS = 16384;              // 64KB of main memory
    localparam MAIN      = 32'h4000_0000;
    localparam IOBASE    = 32'h1000_0000;      // uncached "peripheral"
    localparam CFG       = 32'h7000_0100;

    reg clk;
    reg rst;

    reg  [31:0] c_adr;
    reg  [31:0] c_dat_w;
    wire [31:0] c_dat_r;
    reg c_we;
    reg [3:0] c_sel;
    reg c_stb;
    reg c_cyc;
    reg c_instr;
    wire c_ack;

    wire [31:0] m_adr;
    wire [31:0] m_dat_o;
    reg  [31:0] m_dat_i;
    wire m_we;
    wire [3:0] m_sel;
    wire m_stb;
    wire m_cyc;
    wire [2:0] m_cti;
    wire [1:0] m_bte;
    reg  m_ack;

    reg snoop_stb;
    reg [31:0] snoop_adr;
    wire cfg_hit;

    wb_cache #(
        .I_KB(I_KB), .I_LINE_WORDS(I_LW),
        .D_KB(D_KB), .D_LINE_WORDS(D_LW),
        .FAST_HIT(FAST), .WBUF_DEPTH(WBUF), .BURST(BURST), .SNOOP(1)
    ) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(c_adr), .c_dat_i(c_dat_w), .c_dat_o(c_dat_r),
        .c_we_i(c_we), .c_sel_i(c_sel), .c_stb_i(c_stb), .c_cyc_i(c_cyc),
        .c_instr_i(c_instr), .c_ack_o(c_ack),
        .m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i),
        .m_we_o(m_we), .m_sel_o(m_sel), .m_stb_o(m_stb), .m_cyc_o(m_cyc),
        .m_cti_o(m_cti), .m_bte_o(m_bte), .m_ack_i(m_ack),
        .snoop_stb_i(snoop_stb), .snoop_adr_i(snoop_adr),
        .c_cfg_hit(cfg_hit)
    );

    initial clk = 1'b0;
    always #5 clk = ~clk;

    // -- memories ---------------------------------------------------

    reg [31:0] smem [0:MEM_WORDS-1];     // what the slave holds
    reg [31:0] rmem [0:MEM_WORDS-1];     // what the CPU has written
    reg [31:0] iomem [0:15];

    integer seed;
    integer errors;
    integer i;

    // -- slave --------------------------------------------------------
    //
    // Random latency for the first beat of any cycle, 0-2 extra cycles
    // between burst beats. Writes land in smem when acknowledged.

    reg [31:0] lat;
    reg in_burst;
    reg [31:0] sw;
    integer bursts_seen;
    integer beats_seen;

    always @(posedge clk) begin
        if (rst) begin
            m_ack <= 1'b0;
            lat <= 0;
            in_burst <= 1'b0;
        end else begin
            m_ack <= 1'b0;
            if (!m_cyc) in_burst <= 1'b0;
            if (m_cyc && m_stb && !m_ack) begin
                if (lat == 0) begin
                    lat <= in_burst ? ($unsigned($random(seed)) % 3) :
                                      (1 + ($unsigned($random(seed)) % 12));
                end else if (lat == 1) begin
                    lat <= 0;
                    m_ack <= 1'b1;
                    if ((m_adr & 32'hf000_0000) == MAIN) begin
                        if (m_we) begin
                            sw = smem[(m_adr - MAIN) >> 2];
                            if (m_sel[0]) sw[7:0]   = m_dat_o[7:0];
                            if (m_sel[1]) sw[15:8]  = m_dat_o[15:8];
                            if (m_sel[2]) sw[23:16] = m_dat_o[23:16];
                            if (m_sel[3]) sw[31:24] = m_dat_o[31:24];
                            smem[(m_adr - MAIN) >> 2] <= sw;
                        end
                        m_dat_i <= smem[(m_adr - MAIN) >> 2];
                    end else begin
                        if (m_we) iomem[m_adr[5:2]] <= m_dat_o;
                        m_dat_i <= iomem[m_adr[5:2]];
                    end
                    if (m_cti == 3'b010) begin
                        if (!in_burst) bursts_seen = bursts_seen + 1;
                        in_burst <= 1'b1;
                    end
                    if (m_cti != 3'b000) beats_seen = beats_seen + 1;
                end else begin
                    lat <= lat - 1;
                end
            end
        end
    end

    // Ordering check: at the moment an UNCACHED write to IOBASE+0x3c
    // is acknowledged, every store the CPU issued before it must be in
    // slave memory. The program issues one after random stores.
    always @(posedge clk) begin
        if (m_cyc && m_stb && m_ack && m_we && (m_adr == IOBASE + 32'h3c)) begin
            for (i = 0; i < MEM_WORDS; i = i + 1) begin
                if (smem[i] !== rmem[i]) begin
                    if (errors < 10)
                        $display("ORDER ERROR: word %0d slave=%08x ref=%08x at fence",
                            i, smem[i], rmem[i]);
                    errors = errors + 1;
                end
            end
        end
    end

    // Wishbone master stability: while STB waits for ACK nothing moves
    reg [31:0] p_adr;
    reg [31:0] p_dat;
    reg p_we;
    reg [3:0] p_sel;
    reg p_wait;
    always @(posedge clk) begin
        if (rst) p_wait <= 1'b0;
        else begin
            if (p_wait && m_cyc && m_stb) begin
                if (m_adr !== p_adr || m_we !== p_we || m_sel !== p_sel ||
                    (m_we && m_dat_o !== p_dat)) begin
                    $display("WB ERROR: master changed request while waiting (adr %08x->%08x)",
                        p_adr, m_adr);
                    errors = errors + 1;
                end
            end
            p_wait <= m_cyc && m_stb && !m_ack;
            p_adr <= m_adr; p_dat <= m_dat_o; p_we <= m_we; p_sel <= m_sel;
        end
    end

    // -- CPU-side driver ------------------------------------------------
    //
    // Behaves like picorv32_wb / zeitlos32_wb: holds the request until
    // ack, drops stb/cyc on the ack edge, never overlaps requests.

    reg [31:0] rd;
    integer cycles_in;
    integer total_cycles;
    integer timeout;

    task bus;
        input [31:0] adr;
        input we;
        input [3:0] sel;
        input [31:0] dat;
        input instr;
    begin
        @(posedge clk);
        c_adr <= adr; c_we <= we; c_sel <= sel; c_dat_w <= dat;
        c_instr <= instr; c_stb <= 1'b1; c_cyc <= 1'b1;
        timeout = 0;
        @(posedge clk);
        while (!c_ack) begin
            timeout = timeout + 1;
            total_cycles = total_cycles + 1;
            if (timeout > 5000) begin
                $display("TIMEOUT adr=%08x we=%0d instr=%0d", adr, we, instr);
                $finish;
            end
            @(posedge clk);
        end
        total_cycles = total_cycles + 1;
        rd = c_dat_r;
        c_stb <= 1'b0; c_cyc <= 1'b0; c_we <= 1'b0; c_instr <= 1'b0;
    end
    endtask

    reg [31:0] exp;
    reg [31:0] a;
    reg [31:0] w;
    reg [3:0] s;
    reg [31:0] m;

    task check;
        input [31:0] adr;
        input [31:0] got;
        input [31:0] expv;
        input [8*8-1:0] what;
    begin
        if (got !== expv) begin
            if (errors < 20)
                $display("DATA ERROR (%0s) adr=%08x got=%08x exp=%08x t=%0t",
                    what, adr, got, expv, $time);
            errors = errors + 1;
        end
    end
    endtask

    task store;
        input [31:0] adr;
        input [3:0] sel;
        input [31:0] dat;
    begin
        bus(adr, 1'b1, sel, dat, 1'b0);
        w = rmem[(adr - MAIN) >> 2];
        if (sel[0]) w[7:0]   = dat[7:0];
        if (sel[1]) w[15:8]  = dat[15:8];
        if (sel[2]) w[23:16] = dat[23:16];
        if (sel[3]) w[31:24] = dat[31:24];
        rmem[(adr - MAIN) >> 2] = w;
    end
    endtask

    task load;
        input [31:0] adr;
        input instr;
    begin
        // picorv32 reads carry sel=0000, zeitlos32 sel=1111: use both
        bus(adr, 1'b0, ($random(seed) & 1) ? 4'b1111 : 4'b0000, 32'h0, instr);
        check(adr, rd, rmem[(adr - MAIN) >> 2], instr ? "fetch" : "load");
    end
    endtask

    task cfgw;
        input [31:0] off;
        input [31:0] dat;
    begin
        bus(CFG + off, 1'b1, 4'b1111, dat, 1'b0);
    end
    endtask

    task cfgr;
        input [31:0] off;
    begin
        bus(CFG + off, 1'b0, 4'b0000, 32'h0, 1'b0);
    end
    endtask

    // Random main-memory address biased to produce tag conflicts: a
    // small set of "hot" lines plus anywhere in 64KB.
    function [31:0] raddr;
        input dummy;
    begin
        if ($random(seed) & 1)
            raddr = MAIN + ((($unsigned($random(seed)) % 8) * 32'h1000) +
                            (($unsigned($random(seed)) % 64) * 4));
        else
            raddr = MAIN + (($unsigned($random(seed)) % MEM_WORDS) * 4);
    end
    endfunction

    integer op;
    integer n;
    integer hits0;
    integer t0;

    initial begin
        seed = SEED;
        errors = 0;
        bursts_seen = 0;
        beats_seen = 0;
        total_cycles = 0;
        c_adr = 0; c_dat_w = 0; c_we = 0; c_sel = 0; c_stb = 0; c_cyc = 0;
        c_instr = 0; snoop_stb = 0; snoop_adr = 0;
        for (i = 0; i < MEM_WORDS; i = i + 1) begin
            smem[i] = $random(seed);
            rmem[i] = smem[i];
        end
        for (i = 0; i < 16; i = i + 1) iomem[i] = 0;

        rst = 1'b1;
        repeat (8) @(posedge clk);
        rst = 1'b0;

        // -- 1. identification and reset state --
        cfgr(32'h0c);
        if (rd[31:16] !== 16'h1CAC) begin $display("I_INFO bad %08x", rd); errors = errors + 1; end
        cfgr(32'h1c);
        if (rd[31:16] !== 16'h1DCA) begin $display("D_INFO bad %08x", rd); errors = errors + 1; end
        cfgr(32'h10);
        if (rd !== 32'h0) begin $display("D_CTRL not 0 at reset: %08x", rd); errors = errors + 1; end
        cfgr(32'h00);
        if (rd !== 32'h1) begin $display("I_CTRL not 1 at reset: %08x", rd); errors = errors + 1; end

        // -- 2. D disabled: loads/stores behave, nothing cached --
        for (n = 0; n < 200; n = n + 1) begin
            a = raddr(0);
            if ($random(seed) & 1) load(a, 1'b0);
            else store(a, 4'b1111, $random(seed));
        end
        cfgr(32'h14);
        if (rd !== 0) begin $display("D_HITS %0d with D disabled", rd); errors = errors + 1; end

        // -- 3. enable D + posted writes --
        cfgw(32'h10, 32'h5);
        cfgr(32'h10);
        if (rd !== 32'h5) begin $display("D_CTRL readback %08x", rd); errors = errors + 1; end

        // repeated loads of one line must hit
        a = MAIN + 32'h100;
        load(a, 1'b0); load(a, 1'b0); load(a + 4, 1'b0);
        cfgr(32'h14);
        if (rd !== 2) begin $display("expected 2 D hits, got %0d", rd); errors = errors + 1; end

        // store hit then load: updated data from the cache
        store(a, 4'b0010, 32'h0000_ab00);
        load(a, 1'b0);
        store(a + 4, 4'b1100, 32'h1234_0000);
        load(a + 4, 1'b0);

        // -- 4. snooped code overwrite, no flush --
        a = MAIN + 32'h2000;
        for (n = 0; n < 16; n = n + 1) load(a + n*4, 1'b1);    // cache "code"
        cfgw(32'h34, 0);
        for (n = 0; n < 16; n = n + 1) store(a + n*4, 4'b1111, 32'hC0DE_0000 + n);
        for (n = 0; n < 16; n = n + 1) load(a + n*4, 1'b1);    // must be new
        cfgr(32'h34);
        if (rd == 0) begin $display("I_SNOOPS did not count"); errors = errors + 1; end

        // same, with a byte store into the middle of a cached word
        load(a + 8, 1'b1);
        store(a + 8, 4'b0100, 32'h00EE_0000);
        load(a + 8, 1'b1);

        // -- 5. uncached read of main memory sees buffered store --
        // Force a bypass read by fetching while I is disabled.
        cfgw(32'h00, 32'h0);
        store(MAIN + 32'h300, 4'b1111, 32'hFEED_F00D);
        load(MAIN + 32'h300, 1'b1);
        cfgw(32'h00, 32'h1);

        // -- 6. snoop input --
        a = MAIN + 32'h4000;
        load(a, 1'b0); load(a, 1'b1);                    // cache both sides
        smem[(a - MAIN) >> 2] = 32'h5A5A_5A5A;           // "DMA" write
        rmem[(a - MAIN) >> 2] = 32'h5A5A_5A5A;
        @(posedge clk); snoop_adr <= a; snoop_stb <= 1'b1;
        @(posedge clk); snoop_stb <= 1'b0;
        load(a, 1'b0); load(a, 1'b1);
        // two back-to-back snoops: overflow must degrade to a flush
        a = MAIN + 32'h4100;
        load(a, 1'b0); load(a + 32'h40, 1'b0);
        smem[(a - MAIN) >> 2] = 32'h1111_1111; rmem[(a - MAIN) >> 2] = 32'h1111_1111;
        smem[(a - MAIN + 32'h40) >> 2] = 32'h2222_2222; rmem[(a - MAIN + 32'h40) >> 2] = 32'h2222_2222;
        @(posedge clk); snoop_adr <= a; snoop_stb <= 1'b1;
        @(posedge clk); snoop_adr <= a + 32'h40;
        @(posedge clk); snoop_stb <= 1'b0;
        load(a, 1'b0); load(a + 32'h40, 1'b0);

        // -- 7. random soak --
        for (n = 0; n < NOPS; n = n + 1) begin
            op = $unsigned($random(seed)) % 100;
            a = raddr(0);
            if (op < 30) load(a, 1'b1);
            else if (op < 60) load(a, 1'b0);
            else if (op < 85) begin
                s = $random(seed);
                case ($unsigned($random(seed)) % 3)
                    0: s = 4'b0001 << a[1:0];
                    1: s = a[1] ? 4'b1100 : 4'b0011;
                    default: s = 4'b1111;
                endcase
                store(a, s, $random(seed));
            end else if (op < 90) begin
                // uncached write that another master would observe
                bus(IOBASE + 32'h3c, 1'b1, 4'b1111, n, 1'b0);
            end else if (op < 93) begin
                bus(IOBASE + (($random(seed) & 7) * 4), 1'b0, 4'b0000, 0, 1'b0);
            end else if (op < 94) begin
                // toggle posted writes
                cfgr(32'h10);
                cfgw(32'h10, rd ^ 32'h4);
            end else if (op < 95) begin
                // toggle D enable (re-enable flushes)
                cfgr(32'h10);
                cfgw(32'h10, rd ^ 32'h1);
            end else if (op < 96) begin
                cfgr(32'h00);
                cfgw(32'h00, rd ^ 32'h1);
            end else if (op < 97) begin
                // explicit flushes, both sides
                if ($random(seed) & 1) cfgw(32'h00, 32'h3);
                else begin cfgr(32'h10); cfgw(32'h10, rd | 32'h2); end
            end else if (op < 98) begin
                // snoop with a real change behind the cache
                m = raddr(0);
                smem[(m - MAIN) >> 2] = $random(seed);
                rmem[(m - MAIN) >> 2] = smem[(m - MAIN) >> 2];
                @(posedge clk); snoop_adr <= m; snoop_stb <= 1'b1;
                @(posedge clk); snoop_stb <= 1'b0;
            end else begin
                // sequential run, the common case for code and memcpy
                m = a & ~32'h3f;
                for (i = 0; i < 16; i = i + 1) load(m + i*4, op[0]);
            end
        end

        // leave everything on and do a final full comparison
        cfgw(32'h00, 32'h1);
        cfgw(32'h10, 32'h5);
        for (n = 0; n < MEM_WORDS; n = n + 257) load(MAIN + n*4, 1'b0);
        bus(IOBASE + 32'h3c, 1'b1, 4'b1111, 0, 1'b0);   // final fence

        cfgr(32'h04); $display("  I hits %0d", rd);
        cfgr(32'h08); $display("  I misses %0d", rd);
        cfgr(32'h14); $display("  D hits %0d", rd);
        cfgr(32'h18); $display("  D misses %0d", rd);
        cfgr(32'h30); $display("  WB full waits %0d", rd);
        if (BURST) begin
            $display("  bursts %0d beats %0d", bursts_seen, beats_seen);
            if (bursts_seen == 0 && (I_LW >= 4 || D_LW >= 4)) begin
                $display("no bursts seen with BURST=1"); errors = errors + 1;
            end
        end else if (beats_seen != 0) begin
            $display("CTI used with BURST=0"); errors = errors + 1;
        end

        if (errors == 0)
            $display("PASS tb_cache_id I%0dK/%0dw D%0dK/%0dw FAST=%0d WBUF=%0d BURST=%0d seed=%0d (%0d cpu cycles)",
                I_KB, I_LW, D_KB, D_LW, FAST, WBUF, BURST, SEED, total_cycles);
        else
            $display("FAIL tb_cache_id: %0d errors", errors);
        $finish;
    end

endmodule
