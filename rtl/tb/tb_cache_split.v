/*
 * tb_cache_split.v -- unit testbench for rtl/cache_split.v
 * (wb_cache_split, docs/cache_split.md)
 *
 * Two independent agents drive the two ports at the same time, back to
 * back whenever the cache lets them:
 *
 *   the fetcher  I port: runs of sequential fetches with jumps, over a
 *                "code" region the D agent sometimes writes into
 *   the D agent  D port, in program order, one access at a time:
 *                loads and stores (byte, half, word) mostly over a
 *                "data" region, sometimes into the code region, plus
 *                uncached I/O, fences and register-window operations
 *
 * Checked:
 *
 *   - every load returns exactly what the D agent last stored there
 *     (only the D agent writes memory, so in-order is exact)
 *   - every fetch returns a value the word held at some moment between
 *     SNOOP_WIN cycles before the fetch was taken and its ack: a store
 *     snooped late by at most the documented window is allowed, a stale
 *     line that SURVIVES a store (a missed snoop, a poisoned fill marked
 *     valid) is not
 *   - write-buffer ordering: when an uncached write to IOBASE+0x3c is
 *     acknowledged on the bus, every store acknowledged before it is in
 *     slave memory
 *   - the snoop input: memory changed behind the cache's back (both
 *     agents paused) plus a snoop pulse is never read stale
 *   - acks only for outstanding requests; nothing hangs (timeout)
 *   - Wishbone rule: address/data/we/sel stable while STB waits
 *   - runtime: D on/off, posted writes on/off, I off (uncached fetches),
 *     flushes, all while both ports run
 *
 * Run:
 *   iverilog -g2005 -o tb_cs rtl/tb/tb_cache_split.v rtl/cache_split.v
 *   vvp tb_cs
 *   iverilog -g2005 -Ptb_cache_split.BURST=1 -Ptb_cache_split.WBUF=4 ...
 * Parameters: I_KB, I_LW, D_KB, D_LW, WBUF, BURST, MAIN_512, SEED, NOPS.
 */

`timescale 1ns / 1ps

module tb_cache_split;

    parameter I_KB   = 1;
    parameter I_LW   = 4;
    parameter D_KB   = 1;
    parameter D_LW   = 4;
    parameter WBUF   = 2;
    parameter BURST  = 0;
    parameter SEED   = 1;
    parameter NOPS   = 60000;       // D agent operations
    parameter MAIN_512 = 0;

    localparam SNOOP_WIN = 3;
    localparam MEM_WORDS = 16384;   // 64KB at 0x4000_0000 (+ 0x5000_0000)
    localparam TOT_WORDS = MEM_WORDS * (1 + MAIN_512);
    localparam MAIN   = 32'h4000_0000;
    localparam IOBASE = 32'h1000_0000;
    localparam CFG    = 32'h7000_0100;
    // the code region: the first 8KB of the window (and its 0x5 twin)
    localparam CODE_WORDS = 2048;
    localparam HIST = 4;

    reg clk, rst;

    reg i_req; reg [31:0] i_adr; wire i_rdy, i_ack; wire [31:0] i_dat;
    reg d_req; reg [31:0] d_adr; reg d_we; reg [3:0] d_sel; reg [31:0] d_wdat;
    wire d_rdy, d_ack; wire [31:0] d_rdat; wire d_cfg_hit;

    wire [31:0] m_adr, m_dat_o; reg [31:0] m_dat_i;
    wire m_we, m_stb, m_cyc, m_instr; wire [3:0] m_sel; wire [2:0] m_cti;
    wire [1:0] m_bte; reg m_ack;
    reg snoop_stb; reg [31:0] snoop_adr;

    wb_cache_split #(
        .MAIN_512(MAIN_512),
        .I_KB(I_KB), .I_LINE_WORDS(I_LW), .D_KB(D_KB), .D_LINE_WORDS(D_LW),
        .WBUF_DEPTH(WBUF), .BURST(BURST), .SNOOP(1)
    ) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .i_req(i_req), .i_adr(i_adr), .i_rdy(i_rdy), .i_ack(i_ack), .i_dat(i_dat),
        .d_req(d_req), .d_adr(d_adr), .d_we(d_we), .d_sel(d_sel), .d_wdat(d_wdat),
        .d_rdy(d_rdy), .d_ack(d_ack), .d_rdat(d_rdat),
        .i_mpu_veto(1'b0), .d_mpu_veto(1'b0), .d_mpu_reg(1'b0), .d_mpu_dat(32'h0),
        .m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i), .m_we_o(m_we),
        .m_sel_o(m_sel), .m_stb_o(m_stb), .m_cyc_o(m_cyc), .m_cti_o(m_cti),
        .m_bte_o(m_bte), .m_ack_i(m_ack), .m_instr_o(m_instr),
        .snoop_stb_i(snoop_stb), .snoop_adr_i(snoop_adr),
        .d_cfg_hit(d_cfg_hit)
    );

    initial clk = 1'b0;
    always #5 clk = ~clk;

    integer now;
    always @(posedge clk) now <= now + 1;

    // -- memories ---------------------------------------------------

    reg [31:0] smem [0:TOT_WORDS-1];    // what the slave holds
    reg [31:0] rmem [0:TOT_WORDS-1];    // what the D agent has stored
    // fetch history: the last HIST values each word held, and when
    // each stopped being current (hval[k] was current until htime[k])
    reg [31:0] hval [0:TOT_WORDS*HIST-1];
    integer    htime [0:TOT_WORDS*HIST-1];
    reg [31:0] iomem [0:15];

    function [31:0] midx;
        input [31:0] a;
    begin
        if (MAIN_512) midx = {a[28], a[15:2]};
        else midx = (a - MAIN) >> 2;
    end
    endfunction

    integer seed, errors, k, n;

    // a word changes: remember what it was and until when
    task word_set;
        input integer w;
        input [31:0] v;
        input integer t;
    begin
        for (k = HIST - 1; k > 0; k = k - 1) begin
            hval[w*HIST + k] = hval[w*HIST + k - 1];
            htime[w*HIST + k] = htime[w*HIST + k - 1];
        end
        hval[w*HIST] = rmem[w];
        htime[w*HIST] = t;
        rmem[w] = v;
    end
    endtask

    // -- slave: random latency, CTI bursts --------------------------

    reg [31:0] lat; reg in_burst; reg [31:0] sw;
    integer bursts_seen;

    always @(posedge clk) begin
        if (rst) begin
            m_ack <= 1'b0; lat <= 0; in_burst <= 1'b0;
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
                    if (MAIN_512 ? ((m_adr & 32'he000_0000) == MAIN)
                                 : ((m_adr & 32'hf000_0000) == MAIN)) begin
                        if (m_we) begin
                            sw = smem[midx(m_adr)];
                            if (m_sel[0]) sw[7:0]   = m_dat_o[7:0];
                            if (m_sel[1]) sw[15:8]  = m_dat_o[15:8];
                            if (m_sel[2]) sw[23:16] = m_dat_o[23:16];
                            if (m_sel[3]) sw[31:24] = m_dat_o[31:24];
                            smem[midx(m_adr)] <= sw;
                        end
                        m_dat_i <= smem[midx(m_adr)];
                    end else begin
                        if (m_we) iomem[m_adr[5:2]] <= m_dat_o;
                        m_dat_i <= iomem[m_adr[5:2]];
                    end
                    if (m_cti == 3'b010) begin
                        if (!in_burst) bursts_seen = bursts_seen + 1;
                        in_burst <= 1'b1;
                    end
                end else begin
                    lat <= lat - 1;
                end
            end
        end
    end

    // ordering: at an uncached write to IOBASE+0x3c, memory == reference
    always @(posedge clk) begin
        if (m_cyc && m_stb && m_ack && m_we && (m_adr == IOBASE + 32'h3c)) begin
            for (n = 0; n < TOT_WORDS; n = n + 1) begin
                if (smem[n] !== rmem[n]) begin
                    if (errors < 10)
                        $display("ORDER ERROR: word %0d slave=%08x ref=%08x at fence",
                            n, smem[n], rmem[n]);
                    errors = errors + 1;
                end
            end
        end
    end

    // Wishbone stability while STB waits
    reg [31:0] p_adr, p_dat; reg p_we; reg [3:0] p_sel; reg p_wait;
    always @(posedge clk) begin
        if (rst) p_wait <= 1'b0;
        else begin
            if (p_wait && m_cyc && m_stb &&
                (m_adr !== p_adr || m_we !== p_we || m_sel !== p_sel ||
                 (m_we && m_dat_o !== p_dat))) begin
                $display("WB ERROR: request changed while waiting (%08x->%08x)",
                    p_adr, m_adr);
                errors = errors + 1;
            end
            p_wait <= m_cyc && m_stb && !m_ack;
            p_adr <= m_adr; p_dat <= m_dat_o; p_we <= m_we; p_sel <= m_sel;
        end
    end

    // -- address generators ------------------------------------------

    // a main-memory address in the code region (or anywhere), with the
    // 0x5 twin half the time on a 512MB build
    function [31:0] maddr;
        input integer word;
        input integer r;
    begin
        maddr = MAIN + word * 4;
        if (MAIN_512 && r[0]) maddr = maddr | 32'h1000_0000;
    end
    endfunction

    // -- the fetcher --------------------------------------------------

    reg pause;                // both agents stop issuing
    reg i_out; reg [31:0] i_out_adr; integer i_take_t;
    integer i_fetches, i_run, i_pc, i_wait_cyc;
    reg [31:0] r;

    task check_fetch;
        input [31:0] adr;
        input [31:0] got;
        input integer t_take;
        reg ok;
        integer w, h;
    begin
      if (MAIN_512 ? ((adr & 32'he000_0000) == MAIN) : ((adr & 32'hf000_0000) == MAIN)) begin
        w = midx(adr);
        ok = (got === rmem[w]);
        for (h = 0; h < HIST; h = h + 1)
            if (!ok && got === hval[w*HIST + h] &&
                htime[w*HIST + h] >= t_take - SNOOP_WIN)
                ok = 1'b1;
        if (!ok) begin
            if (errors < 20)
                $display("FETCH ERROR adr=%08x got=%08x now=%08x (taken t=%0d, now t=%0d, last change t=%0d)",
                    adr, got, rmem[w], t_take, now, htime[w*HIST]);
            errors = errors + 1;
        end
      end
    end
    endtask

    always @(posedge clk) begin
        if (rst) begin
            i_req <= 1'b0; i_out <= 1'b0; i_pc = 0; i_run = 0; i_wait_cyc = 0;
        end else begin
            if (i_ack) begin
                if (!i_out) begin
                    $display("PROTOCOL ERROR: I ack with nothing outstanding");
                    errors = errors + 1;
                end else begin
                    check_fetch(i_out_adr, i_dat, i_take_t);
                    i_fetches = i_fetches + 1;
                end
            end
            if (i_ack && !(i_req && i_rdy)) i_out <= 1'b0;

            if (i_out && !i_ack) i_wait_cyc = i_wait_cyc + 1;
            else i_wait_cyc = 0;
            if (i_wait_cyc > 3000) begin
                $display("TIMEOUT on the I port, adr=%08x", i_out_adr);
                $finish;
            end

            if (i_req && i_rdy) begin
                i_out <= 1'b1;
                i_out_adr <= i_adr;
                i_take_t = now;
            end

            if (!i_req || i_rdy) begin
                // next fetch: sequential runs, jumps, gaps, rarely an
                // uncached one (the BIOS at 0)
                r = $random(seed);
                if (pause || r[2:0] == 3'd0) begin
                    i_req <= 1'b0;
                end else begin
                    if (r[27:25] == 3'd0 && code_store_t > 0 &&
                        now - code_store_t > 8 && now - code_store_t < 400) begin
                        // back to a word just stored into a line that was
                        // being fetched: a poisoned fill must not have
                        // left the old line valid
                        i_pc = code_store_word % CODE_WORDS;
                        i_run = 2;
                    end else if (i_run == 0 || r[7:4] == 4'd0) begin
                        i_pc = $unsigned($random(seed)) % CODE_WORDS;
                        i_run = 1 + ($unsigned($random(seed)) % 24);
                    end else begin
                        i_pc = (i_pc + 1) % CODE_WORDS;
                        i_run = i_run - 1;
                    end
                    i_req <= 1'b1;
                    if (r[15:10] == 6'd0)
                        i_adr <= IOBASE + ((r[19:16] & 4'hf) << 2);   // uncached
                    else
                        i_adr <= maddr(i_pc, r[23:20]);
                end
            end
        end
    end

    // uncached fetches read iomem, which the D agent also writes: allow
    // any value there by keeping the reference in step (see store_io)

    // -- the D agent --------------------------------------------------

    // The request being presented (p_*) and the one outstanding (o_*).
    // The next request is presented as soon as the current one is taken,
    // so it goes in on the edge of that one's ack: back to back, as a
    // pipeline's memory stage does.
    reg p_cfg, p_io;
    reg o_valid, o_we, o_cfg, o_io; reg [31:0] o_adr, o_dat; reg [3:0] o_sel;
    integer d_ops, d_issued, d_wait_cyc, loads, stores, ios, fences, cfgs, snoops_done;
    reg [31:0] w, expv, last_adr;
    integer code_store_word, code_store_t;
    reg [1:0] size;
    integer word;

    task d_issue;
    begin
        r = $random(seed);
        p_cfg <= 1'b0;
        p_io <= 1'b0;
        d_sel <= 4'b1111;
        d_wdat <= $random(seed);
        if (r[6:0] == 7'd0) begin
            // the register window: switch something, or read FEAT
            p_cfg <= 1'b1;
            cfgs = cfgs + 1;
            case (($unsigned($random(seed))) % 6)
                0: begin d_adr <= CFG + 32'h10; d_we <= 1'b1; d_wdat <= 32'h5; end // D on, posted
                1: begin d_adr <= CFG + 32'h10; d_we <= 1'b1; d_wdat <= 32'h1; end // D on, not posted
                2: begin d_adr <= CFG + 32'h10; d_we <= 1'b1; d_wdat <= 32'h0; end // D off
                3: begin d_adr <= CFG + 32'h00; d_we <= 1'b1; d_wdat <= 32'h3; end // I on + flush
                4: begin d_adr <= CFG + 32'h00; d_we <= 1'b1;
                         d_wdat <= ($random(seed) & 1) ? 32'h1 : 32'h0; end         // I on/off
                default: begin d_adr <= CFG + 32'h2c; d_we <= 1'b0; end             // FEAT
            endcase
        end else if (r[6:0] == 7'd1) begin
            // fence: the ordering check watches this uncached write
            p_io <= 1'b1;
            fences = fences + 1;
            d_adr <= IOBASE + 32'h3c; d_we <= 1'b1;
        end else if (r[6:2] == 5'd1) begin
            p_io <= 1'b1;
            ios = ios + 1;
            d_adr <= IOBASE + ((r[11:8] % 15) << 2);
            d_we <= r[12];
        end else begin
            // main memory: 1 in 4 the last word again (hazard 2), 1 in
            // 8 of the rest in the code region (the snoop and poison)
            if (r[24:23] == 2'd0 && last_adr != 0) begin
                word = midx(last_adr);
            end else if (r[30:28] == 3'd0 && !r[22] && i_req && i_adr[31:28] != 4'h1) begin
                // a store into the line the fetcher is fetching right now:
                // the line may be being filled (hazard 3)
                word = (midx(i_adr) & ~(I_LW - 1)) + ($unsigned($random(seed)) % I_LW);
                code_store_word = word;
                code_store_t = now;
            end else if (r[15:13] == 3'd0)
                word = $unsigned($random(seed)) % CODE_WORDS;
            else
                word = CODE_WORDS + ($unsigned($random(seed)) % (MEM_WORDS - CODE_WORDS));
            size = r[17:16];
            if (size == 2'd3) size = 2'd2;
            w = maddr(word % MEM_WORDS, r[27:26]);
            if (MAIN_512 && word >= MEM_WORDS) w = w | 32'h1000_0000;
            w = w | ((size == 2'd0) ? {30'd0, r[19:18]} :
                     (size == 2'd1) ? {30'd0, r[19], 1'b0} : 32'd0);
            last_adr = w;
            d_adr <= w;
            d_we <= !r[22];
            d_sel <= (size == 2'd0) ? (4'b0001 << r[19:18]) :
                     (size == 2'd1) ? (r[19] ? 4'b1100 : 4'b0011) : 4'b1111;
        end
        d_req <= 1'b1;
        d_issued = d_issued + 1;
    end
    endtask

    reg d_take;
    always @(posedge clk) begin
        if (rst) begin
            d_req <= 1'b0; o_valid <= 1'b0; d_wait_cyc = 0; last_adr = 0;
            p_cfg <= 1'b0; p_io <= 1'b0;
        end else begin
            d_take = d_req && d_rdy;

            if (d_ack) begin
                if (!o_valid) begin
                    $display("PROTOCOL ERROR: D ack with nothing outstanding");
                    errors = errors + 1;
                end else if (o_cfg) begin
                    if (!o_we && o_adr == CFG + 32'h2c && !d_rdat[11]) begin
                        $display("FEAT ERROR: SPLIT bit (11) not set: %08x", d_rdat);
                        errors = errors + 1;
                    end
                end else if (o_io) begin
                    if (o_we) iomem[o_adr[5:2]] = o_dat;
                    else if (d_rdat !== iomem[o_adr[5:2]]) begin
                        $display("IO ERROR adr=%08x got=%08x exp=%08x",
                            o_adr, d_rdat, iomem[o_adr[5:2]]);
                        errors = errors + 1;
                    end
                end else if (o_we) begin
                    w = rmem[midx(o_adr)];
                    if (o_sel[0]) w[7:0]   = o_dat[7:0];
                    if (o_sel[1]) w[15:8]  = o_dat[15:8];
                    if (o_sel[2]) w[23:16] = o_dat[23:16];
                    if (o_sel[3]) w[31:24] = o_dat[31:24];
                    word_set(midx(o_adr), w, now);
                    stores = stores + 1;
                end else begin
                    expv = rmem[midx(o_adr)];
                    if (d_rdat !== expv) begin
                        if (errors < 20)
                            $display("LOAD ERROR adr=%08x got=%08x exp=%08x t=%0d",
                                o_adr, d_rdat, expv, now);
                        errors = errors + 1;
                    end
                    loads = loads + 1;
                end
                d_ops = d_ops + 1;
            end

            if (d_take) begin
                o_valid <= 1'b1;
                o_adr <= d_adr; o_we <= d_we; o_sel <= d_sel; o_dat <= d_wdat;
                o_cfg <= p_cfg; o_io <= p_io;
            end else if (d_ack) begin
                o_valid <= 1'b0;
            end

            if (o_valid && !d_ack) d_wait_cyc = d_wait_cyc + 1;
            else d_wait_cyc = 0;
            if (d_wait_cyc > 3000) begin
                $display("TIMEOUT on the D port, adr=%08x", o_adr);
                $finish;
            end

            if (!d_req || d_take) begin
                if (pause || d_issued >= NOPS || ($random(seed) & 3) == 0)
                    d_req <= 1'b0;
                else
                    d_issue;
            end
        end
    end

    // -- coverage: what the run actually exercised ---------------------

    integer cv_ihit, cv_dhit, cv_i_b2b, cv_d_b2b, cv_both, cv_hum_i, cv_hum_d;
    integer cv_iinv, cv_poison, cv_haz2, cv_stdwr, cv_post;
    reg prev_fill_end;
    always @(posedge clk) begin
        if (rst) begin
            cv_ihit = 0; cv_dhit = 0; cv_i_b2b = 0; cv_d_b2b = 0; cv_both = 0;
            cv_hum_i = 0; cv_hum_d = 0; cv_iinv = 0; cv_poison = 0; cv_haz2 = 0;
            cv_stdwr = 0; cv_post = 0;
        end else begin
            if (dut.i_hit_ack) cv_ihit = cv_ihit + 1;
            if (dut.d_hit_ack) cv_dhit = cv_dhit + 1;
            if (i_ack && i_req && i_rdy) cv_i_b2b = cv_i_b2b + 1;
            if (d_ack && d_req && d_rdy) cv_d_b2b = cv_d_b2b + 1;
            if (i_ack && d_ack) cv_both = cv_both + 1;
            // a hit on one side while the memory port fills for the other
            if (dut.i_hit_ack && dut.mstate != 0 && dut.mstate != 5 && !dut.m_for_i)
                cv_hum_i = cv_hum_i + 1;
            if (dut.d_hit_ack && dut.mstate != 0 && dut.mstate != 5 && dut.m_for_i)
                cv_hum_d = cv_hum_d + 1;
            if (dut.st_iinv) cv_iinv = cv_iinv + 1;
            if (dut.st_dwr) cv_stdwr = cv_stdwr + 1;
            if (dut.st_post) cv_post = cv_post + 1;
            if (dut.fill_end_go && dut.fill_poison) cv_poison = cv_poison + 1;
            if (dut.d_pend && dut.d_look && dut.d_fwd) cv_haz2 = cv_haz2 + 1;
        end
    end

    // -- the external snoop: both agents paused, memory changed --------

    integer t, idle;
    initial begin
        seed = SEED; errors = 0; now = 0;
        i_fetches = 0; d_ops = 0; d_issued = 0; code_store_t = 0; code_store_word = 0; loads = 0; stores = 0; ios = 0;
        fences = 0; cfgs = 0; snoops_done = 0; bursts_seen = 0;
        pause = 1'b0; snoop_stb = 1'b0; snoop_adr = 0;
        d_adr = 0; d_we = 0; d_sel = 0; d_wdat = 0; i_adr = 0;
        for (n = 0; n < TOT_WORDS; n = n + 1) begin
            smem[n] = $random(seed); rmem[n] = smem[n];
            for (k = 0; k < HIST; k = k + 1) begin
                hval[n*HIST + k] = smem[n]; htime[n*HIST + k] = -1000;
            end
        end
        for (n = 0; n < 16; n = n + 1) iomem[n] = 0;
        rst = 1'b1;
        repeat (4) @(posedge clk);
        rst = 1'b0;

        while (d_ops < NOPS) begin
            repeat (500 + ($unsigned($random(seed)) % 2000)) @(posedge clk);
            // pause, wait for both ports to go quiet
            pause = 1'b1;
            idle = 0;
            while (idle < 4) begin
                @(posedge clk);
                if (!i_out && !o_valid && !i_req && !d_req) idle = idle + 1;
                else idle = 0;
            end
            // memory changes behind the cache: a word in each region
            for (t = 0; t < 2; t = t + 1) begin
                word = (t == 0) ? ($unsigned($random(seed)) % CODE_WORDS) :
                    (CODE_WORDS + ($unsigned($random(seed)) % (MEM_WORDS - CODE_WORDS)));
                w = $random(seed);
                smem[word] = w;
                word_set(word, w, now);
                @(posedge clk);
                snoop_adr <= maddr(word, 0); snoop_stb <= 1'b1;
                @(posedge clk);
                snoop_stb <= 1'b0;
                snoops_done = snoops_done + 1;
            end
            repeat (4) @(posedge clk);
            pause = 1'b0;
        end
        repeat (200) @(posedge clk);

        $display("tb_cache_split: I%0dK/%0dw D%0dK/%0dw wbuf=%0d burst=%0d 512=%0d seed=%0d",
            I_KB, I_LW, D_KB, D_LW, WBUF, BURST, MAIN_512, SEED);
        $display("  %0d fetches, %0d D ops (%0d loads, %0d stores, %0d io, %0d fences, %0d cfg), %0d snoops, %0d bursts, %0d cycles",
            i_fetches, d_ops, loads, stores, ios, fences, cfgs, snoops_done, bursts_seen, now);
        $display("  coverage: I hit acks %0d, D hit acks %0d, back-to-back I %0d / D %0d, I+D same cycle %0d",
            cv_ihit, cv_dhit, cv_i_b2b, cv_d_b2b, cv_both);
        $display("            hit under miss: I during D fill %0d, D during I fill %0d",
            cv_hum_i, cv_hum_d);
        $display("            posted %0d, store hits %0d, I snoop invalidations %0d, poisoned fills %0d, same-word forwards %0d",
            cv_post, cv_stdwr, cv_iinv, cv_poison, cv_haz2);
        if (errors == 0) $display("PASS");
        else $display("FAIL: %0d errors", errors);
        $finish;
    end

endmodule
