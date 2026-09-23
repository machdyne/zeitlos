/*
 * Zeitlos SOC
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Unified instruction + data cache (wishbone pass-through)
 *
 * Built when a board defines `DCACHE (which requires `ICACHE). A board
 * with only `ICACHE still builds rtl/cache.v's wb_icache, unchanged.
 * See docs/dcache.md for the full design; the short version:
 *
 *   picorv32/zeitlos32 --> wb_mtu --> wb_cache --> wb_arbiter_main
 *
 * -- Why ONE module with two arrays --
 *
 * Chaining a data cache behind the instruction cache (or the reverse)
 * makes every hit in the second module pay the first module's
 * registered bypass on the way in and out, turning a 1-cycle hit into
 * 3-4. Here both lookups run in parallel from the same incoming
 * address and both hit paths are 1 cycle. They share one memory port,
 * one bypass path, one register window and one flush walker.
 *
 * -- Coherency, by construction --
 *
 * The CPU is the only master on this SOC that writes main memory (the
 * blitter source port and the audio mixer are read-only; the rasterizer
 * and the blitter destination are on the VRAM bus). Every CPU store to
 * main memory passes through here, so:
 *
 *   - D side is WRITE-THROUGH with UPDATE-ON-HIT and NO WRITE-ALLOCATE.
 *     Memory is never behind the cache, so other masters reading main
 *     memory always see current data, and nothing is ever dirty.
 *
 *   - I side SNOOPS every CPU store: the I tag for the store address is
 *     read by the same lookup that reads the D tag, and a matching
 *     valid line is invalidated. Code written as data (app loading) can
 *     therefore never be executed stale, with or without the software
 *     flush calls that wb_icache relies on. The compare is on the full
 *     tag -- index-only invalidation would be tripped continuously by
 *     ordinary data stores.
 *
 *   - Posted stores (the write buffer) are DRAINED BEFORE ANY OTHER BUS
 *     ACCESS: every fill, every uncached read or write. So "store a
 *     buffer, then write a register that starts another master reading
 *     it" is ordered without fences, and an uncached read of a location
 *     just stored always sees the store.
 *
 *   - snoop_stb_i/snoop_adr_i invalidate by INDEX (both sides) when a
 *     master other than the CPU writes main memory. No such master
 *     exists today; this is here so one can be added (rtl/dma.v)
 *     without anyone having to remember. Overflow degrades to a full
 *     flush rather than to incoherence.
 *
 * Both sides are physically tagged (post-MTU), so context switches
 * need nothing. See rtl/cache.v's header for why that matters.
 *
 * -- Direction is we, not sel --
 *
 * picorv32 reads carry sel=0000 and zeitlos32 reads carry sel=1111.
 * This module decides read/write from c_we_i only, and forwards sel
 * verbatim on bypass. Fills drive we=0, sel=0000, which every main
 * memory controller in rtl/mem/ reads correctly.
 *
 * -- Storage --
 *
 * All four arrays are read synchronously through lu_adr (the incoming
 * address in S_IDLE, the registered one afterwards) and each has
 * exactly ONE write port, fed from combinational *_we/_wa/_wd signals,
 * so each infers as block RAM. A store hit merges the new bytes into
 * the old word (already read by the lookup) and writes a full word, so
 * the data array needs no byte enables.
 *
 * -- Register window (CFG_BASE, answered upstream, never on the bus) --
 *
 *   +0x00  I_CTRL    bit0 enable (reset 1), bit1 flush
 *   +0x04  I_HITS
 *   +0x08  I_MISSES
 *   +0x0C  I_INFO    { 16'h1CAC, I_LINE_WORDS, I_KB }
 *   +0x10  D_CTRL    bit0 enable, bit1 flush, bit2 posted writes
 *                    (reset 0; enabling bit0 also flushes D)
 *   +0x14  D_HITS    cached loads that hit
 *   +0x18  D_MISSES  cached loads that missed (line filled)
 *   +0x1C  D_INFO    { 16'h1DCA, D_LINE_WORDS, D_KB }
 *   +0x20  LOADS     data loads from main memory (any; write clears)
 *   +0x24  STORES    stores to main memory (write clears)
 *   +0x28  STALL     cycles a CPU request waited (write clears)
 *   +0x2C  FEAT      { 16'h0, 5'b0, SNOOP, BURST, FAST_HIT, WBUF_DEPTH[7:0] }
 *   +0x30  WBFULL    cycles a store waited on a full write buffer
 *                    (write clears)
 *   +0x34  I_SNOOPS  I lines invalidated by store snooping
 *                    (write clears)
 *
 * I_* registers are identical to wb_icache's, so sw/bios/bios.c,
 * sw/os/fs/fs.c and sw/os/zar.c work against either module. Note that
 * wb_icache decodes only adr[3:2], so on a bitstream built with it
 * +0x10 aliases I_CTRL: software must check D_INFO's magic before ever
 * writing D_CTRL (sw/common/zsoc.h's helpers do).
 */

module wb_cache #(
    parameter I_KB         = 8,   // instruction array size, KB (power of 2)
    parameter I_LINE_WORDS = 4,   // words per I line (power of 2, >= 2)
    parameter D_KB         = 4,   // data array size, KB (power of 2)
    parameter D_LINE_WORDS = 4,   // words per D line (power of 2, >= 2)
    // 1: answer hits combinationally in S_LOOKUP (1-cycle hit). 0: a
    // cycle later, from a register. Same meaning as wb_icache's.
    parameter FAST_HIT     = 1,
    // Posted-write buffer entries: 0 (stores always wait for memory),
    // 1, 2 or 4. Only used while D_CTRL bit2 is set.
    parameter WBUF_DEPTH   = 2,
    // 1: line fills are Wishbone B4 incrementing bursts (CTI=010, last
    // beat 111) with STB held. Only applied to sides whose line is a
    // multiple of 4 words, because rtl/mem/sdram_kianv.v's burst path
    // always delivers 4 words per burst. 0: classic single cycles.
    parameter BURST        = 0,
    // 1: honour snoop_stb_i. 0: ignore it (inputs may be tied off).
    parameter SNOOP        = 1,
    parameter [31:0] CFG_BASE = 32'h7000_0100
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- CPU side: physical address, post-MTU --
    input [31:0] c_adr_i,
    input [31:0] c_dat_i,
    output [31:0] c_dat_o,
    input c_we_i,
    input [3:0] c_sel_i,
    input c_stb_i,
    input c_cyc_i,
    input c_instr_i,
    output c_ack_o,

    // -- memory side: drives the main bus via wb_arbiter_main --
    output reg [31:0] m_adr_o,
    output reg [31:0] m_dat_o,
    input [31:0] m_dat_i,
    output reg m_we_o,
    output reg [3:0] m_sel_o,
    output reg m_stb_o,
    output reg m_cyc_o,
    output reg [2:0] m_cti_o,
    output [1:0] m_bte_o,
    input m_ack_i,

    // -- write snoop from other masters (see header) --
    input snoop_stb_i,
    input [31:0] snoop_adr_i,

    output c_cfg_hit
);

    // -- geometry --------------------------------------------------

    localparam I_WORDS   = (I_KB * 1024) / 4;
    localparam I_LINES   = I_WORDS / I_LINE_WORDS;
    localparam I_IDXB    = $clog2(I_LINES);
    localparam I_WOFFB   = $clog2(I_LINE_WORDS);
    localparam I_DIDXB   = $clog2(I_WORDS);
    localparam I_IDXLSB  = 2 + I_WOFFB;
    localparam I_TAGLSB  = I_IDXLSB + I_IDXB;
    localparam I_TAGB    = 28 - I_TAGLSB;

    localparam D_WORDS   = (D_KB * 1024) / 4;
    localparam D_LINES   = D_WORDS / D_LINE_WORDS;
    localparam D_IDXB    = $clog2(D_LINES);
    localparam D_WOFFB   = $clog2(D_LINE_WORDS);
    localparam D_DIDXB   = $clog2(D_WORDS);
    localparam D_IDXLSB  = 2 + D_WOFFB;
    localparam D_TAGLSB  = D_IDXLSB + D_IDXB;
    localparam D_TAGB    = 28 - D_TAGLSB;

    // shared counters are sized for the larger side
    localparam FCW  = (I_WOFFB > D_WOFFB) ? I_WOFFB : D_WOFFB;
    localparam FIB  = (I_IDXB > D_IDXB) ? I_IDXB : D_IDXB;
    localparam MAXL = (I_LINES > D_LINES) ? I_LINES : D_LINES;

    localparam I_BURST = (BURST != 0) && ((I_LINE_WORDS % 4) == 0);
    localparam D_BURST = (BURST != 0) && ((D_LINE_WORDS % 4) == 0);

    // WBD is the usable depth; storage is at least 2 entries so the
    // pointers can simply wrap (a 1-deep buffer uses one of the two)
    localparam WBD  = (WBUF_DEPTH < 1) ? 1 : WBUF_DEPTH;
    localparam WBS  = (WBD < 2) ? 2 : WBD;
    localparam WBPB = $clog2(WBS);

    // -- storage ---------------------------------------------------

    reg [31:0] i_data [0:I_WORDS-1];
    reg [I_TAGB:0] i_tag [0:I_LINES-1];     // { valid, tag }
    reg [31:0] d_data [0:D_WORDS-1];
    reg [D_TAGB:0] d_tag [0:D_LINES-1];

    reg [31:0] i_data_q;
    reg [I_TAGB:0] i_tag_q;
    reg [31:0] d_data_q;
    reg [D_TAGB:0] d_tag_q;

    // single write port per array (driven by the always @* below)
    reg i_data_we;
    reg [I_DIDXB-1:0] i_data_wa;
    reg [31:0] i_data_wd;
    reg i_tag_we;
    reg [I_IDXB-1:0] i_tag_wa;
    reg [I_TAGB:0] i_tag_wd;
    reg d_data_we;
    reg [D_DIDXB-1:0] d_data_wa;
    reg [31:0] d_data_wd;
    reg d_tag_we;
    reg [D_IDXB-1:0] d_tag_wa;
    reg [D_TAGB:0] d_tag_wd;

    // -- control registers -----------------------------------------

    reg i_enable;
    reg d_enable;
    reg wb_enable;
    reg cfg_i_flush_req;
    reg cfg_d_flush_req;
    reg cfg_clr_loads;
    reg cfg_clr_stores;
    reg cfg_clr_stall;
    reg cfg_clr_wbfull;
    reg cfg_clr_isnoop;

    reg [31:0] stat_i_hits;
    reg [31:0] stat_i_misses;
    reg [31:0] stat_d_hits;
    reg [31:0] stat_d_misses;
    reg [31:0] stat_loads;
    reg [31:0] stat_stores;
    reg [31:0] stat_stall;
    reg [31:0] stat_wbfull;
    reg [31:0] stat_isnoop;

    // -- fsm -------------------------------------------------------

    localparam S_IDLE     = 3'd0;
    localparam S_LOOKUP   = 3'd1;
    localparam S_MEMREQ   = 3'd2;   // waiting for the write buffer to drain
    localparam S_FILL     = 3'd3;
    localparam S_FILL_SEQ = 3'd4;
    localparam S_FILL_END = 3'd5;
    localparam S_BYPASS   = 3'd6;
    localparam S_FLUSH    = 3'd7;

    // request kinds, decided when a request is accepted
    localparam K_IF  = 2'd0;   // cacheable instruction fetch
    localparam K_LD  = 2'd1;   // cacheable data load
    localparam K_ST  = 2'd2;   // store to main memory (snoop + update)
    localparam K_BYP = 2'd3;   // everything else: straight to the bus

    // fsm_encoding "none": left to itself yosys re-encodes this FSM and
    // folds the write-buffer, snoop and hit conditions into its
    // transition logic, which measured ~430 LUT4 larger on ECP5 (2675
    // vs 2247 for I 8KB / D 4KB / WBUF 2) for no gain.
    (* fsm_encoding = "none" *) reg [2:0] state;
    reg [1:0] req_kind;
    reg [31:0] req_adr;
    reg [31:0] req_dat;
    reg [3:0] req_sel;
    reg req_we;

    reg [FCW-1:0] fill_cnt;
    reg fill_i;                  // 1 = filling the I side, 0 = D side
    reg [31:0] fill_want_dat;
    reg fill_want_got;

    reg flush_pending_i;
    reg flush_pending_d;
    reg flush_i;
    reg flush_d;
    reg [FIB-1:0] flush_idx;

    reg ack_r;
    reg [31:0] dat_r;

    // -- write buffer ----------------------------------------------

    reg [31:0] wb_adr [0:WBS-1];
    reg [31:0] wb_dat [0:WBS-1];
    reg [3:0]  wb_sel [0:WBS-1];
    reg [WBPB-1:0] wb_rp;
    reg [WBPB-1:0] wb_wp;
    reg [WBPB:0] wb_cnt;
    reg dr_busy;                 // a buffered store is on the bus

    // -- snoop -----------------------------------------------------

    reg snoop_pending;
    reg [31:0] snoop_adr;

    // STALL is counted a cycle late from this, so the hit compare
    // feeds one flop rather than a 32-bit carry chain
    reg stall_pulse;

    // -- config decode (upstream, never on the bus) ----------------

    reg [31:0] cfg_dat_o;
    reg cfg_ack_o;

    wire cfg_sel;
    assign cfg_sel = ((c_adr_i & 32'hf000_0700) == CFG_BASE);
    assign c_cfg_hit = cfg_sel;

    wire [3:0] cfg_reg;
    assign cfg_reg = c_adr_i[5:2];

    // -- request decode --------------------------------------------

    wire c_req;
    wire c_main;
    wire [31:0] lu_adr;
    wire bus_free;
    wire bus_owned;
    wire wb_full;
    wire post_en;

    wire [I_TAGB-1:0] req_itag;
    wire [I_IDXB-1:0] req_iidx;
    wire [I_WOFFB-1:0] req_iwoff;
    wire [D_TAGB-1:0] req_dtag;
    wire [D_IDXB-1:0] req_didx;
    wire [D_WOFFB-1:0] req_dwoff;
    wire i_hit;
    wire d_hit;
    wire hit_now;
    wire st_go;
    wire st_ack_now;
    wire fill_last;
    wire [31:0] st_merged;
    wire [2:0] fill_cti_next;
    wire [31:0] fill_base;
    wire fill_burst;

    assign c_req  = c_cyc_i && c_stb_i && !cfg_sel;
    assign c_main = ((c_adr_i & 32'hf000_0000) == 32'h4000_0000);

    assign lu_adr = (state == S_IDLE) ? c_adr_i : req_adr;

    assign req_itag  = req_adr[I_TAGLSB +: I_TAGB];
    assign req_iidx  = req_adr[I_IDXLSB +: I_IDXB];
    assign req_iwoff = req_adr[2 +: I_WOFFB];
    assign req_dtag  = req_adr[D_TAGLSB +: D_TAGB];
    assign req_didx  = req_adr[D_IDXLSB +: D_IDXB];
    assign req_dwoff = req_adr[2 +: D_WOFFB];

    assign i_hit = i_tag_q[I_TAGB] && (i_tag_q[I_TAGB-1:0] == req_itag);
    assign d_hit = d_tag_q[D_TAGB] && (d_tag_q[D_TAGB-1:0] == req_dtag);

    // The memory port is free for a new transaction only when nothing
    // is buffered and no buffered store is in flight. This is the
    // single rule that orders posted stores before everything else.
    assign bus_free  = (wb_cnt == 0) && !dr_busy;
    assign bus_owned = (state == S_FILL) || (state == S_FILL_SEQ) ||
                       (state == S_FILL_END) || (state == S_BYPASS);
    assign wb_full   = (wb_cnt == WBD);
    assign post_en   = (WBUF_DEPTH > 0) && wb_enable;

    // store in S_LOOKUP proceeds this cycle (posted: needs a free slot)
    assign st_go = (state == S_LOOKUP) && (req_kind == K_ST) &&
                   (!post_en || !wb_full);
    assign st_ack_now = FAST_HIT && st_go && post_en;

    assign hit_now = FAST_HIT && (state == S_LOOKUP) &&
        (((req_kind == K_IF) && i_hit) || ((req_kind == K_LD) && d_hit));

    assign c_ack_o = cfg_sel ? cfg_ack_o : (ack_r | hit_now | st_ack_now);
    assign c_dat_o = cfg_sel ? cfg_dat_o :
        (hit_now ? ((req_kind == K_IF) ? i_data_q : d_data_q) : dat_r);

    assign st_merged = {
        req_sel[3] ? req_dat[31:24] : d_data_q[31:24],
        req_sel[2] ? req_dat[23:16] : d_data_q[23:16],
        req_sel[1] ? req_dat[15:8]  : d_data_q[15:8],
        req_sel[0] ? req_dat[7:0]   : d_data_q[7:0] };

    assign fill_last = fill_i ? (fill_cnt == (I_LINE_WORDS-1)) :
                                (fill_cnt == (D_LINE_WORDS-1));

    // CTI for the beat after the one being acknowledged now
    assign fill_cti_next = (fill_i ? (fill_cnt == (I_LINE_WORDS-2)) :
                                     (fill_cnt == (D_LINE_WORDS-2))) ?
                           3'b111 : 3'b010;

    assign m_bte_o = 2'b00;      // linear bursts only

    // First word of the line a miss in S_LOOKUP/S_MEMREQ will fill.
    // Decided by req_kind, which is already final in both states.
    assign fill_base = (req_kind == K_IF) ?
        { req_adr[31:I_IDXLSB], {I_WOFFB{1'b0}}, 2'b00 } :
        { req_adr[31:D_IDXLSB], {D_WOFFB{1'b0}}, 2'b00 };
    assign fill_burst = (req_kind == K_IF) ? I_BURST : D_BURST;

    // -- synchronous read ports (infer block RAM) ------------------

    always @(posedge wb_clk_i) begin
        i_data_q <= i_data[lu_adr[2 +: I_DIDXB]];
        i_tag_q  <= i_tag[lu_adr[I_IDXLSB +: I_IDXB]];
        d_data_q <= d_data[lu_adr[2 +: D_DIDXB]];
        d_tag_q  <= d_tag[lu_adr[D_IDXLSB +: D_IDXB]];
    end

    // -- single write port per array -------------------------------

    always @(posedge wb_clk_i) begin
        if (i_data_we) i_data[i_data_wa] <= i_data_wd;
    end

    always @(posedge wb_clk_i) begin
        if (i_tag_we) i_tag[i_tag_wa] <= i_tag_wd;
    end

    always @(posedge wb_clk_i) begin
        if (d_data_we) d_data[d_data_wa] <= d_data_wd;
    end

    always @(posedge wb_clk_i) begin
        if (d_tag_we) d_tag[d_tag_wa] <= d_tag_wd;
    end

    // Every array write is listed here, and the cases are mutually
    // exclusive by state, so no write can be lost to another.
    always @* begin
        i_data_we = 1'b0;
        i_data_wa = { req_iidx, fill_cnt[I_WOFFB-1:0] };
        i_data_wd = m_dat_i;
        i_tag_we  = 1'b0;
        i_tag_wa  = req_iidx;
        i_tag_wd  = { 1'b1, req_itag };
        d_data_we = 1'b0;
        d_data_wa = { req_didx, fill_cnt[D_WOFFB-1:0] };
        d_data_wd = m_dat_i;
        d_tag_we  = 1'b0;
        d_tag_wa  = req_didx;
        d_tag_wd  = { 1'b1, req_dtag };

        case (state)
            S_FLUSH: begin
                if (flush_i && (flush_idx < I_LINES)) begin
                    i_tag_we = 1'b1;
                    i_tag_wa = flush_idx[I_IDXB-1:0];
                    i_tag_wd = 0;
                end
                if (flush_d && (flush_idx < D_LINES)) begin
                    d_tag_we = 1'b1;
                    d_tag_wa = flush_idx[D_IDXB-1:0];
                    d_tag_wd = 0;
                end
            end
            S_IDLE: begin
                // index-only snoop invalidation; S_IDLE accepts no
                // request while one is pending (see below)
                if (snoop_pending) begin
                    i_tag_we = 1'b1;
                    i_tag_wa = snoop_adr[I_IDXLSB +: I_IDXB];
                    i_tag_wd = 0;
                    d_tag_we = 1'b1;
                    d_tag_wa = snoop_adr[D_IDXLSB +: D_IDXB];
                    d_tag_wd = 0;
                end
            end
            S_LOOKUP: begin
                if (st_go) begin
                    if (i_hit) begin
                        i_tag_we = 1'b1;
                        i_tag_wd = 0;
                    end
                    if (d_hit) begin
                        d_data_we = 1'b1;
                        d_data_wa = { req_didx, req_dwoff };
                        d_data_wd = st_merged;
                    end
                end
            end
            S_FILL: begin
                if (m_ack_i) begin
                    if (fill_i) i_data_we = 1'b1;
                    else d_data_we = 1'b1;
                end
            end
            S_FILL_END: begin
                if (fill_i) i_tag_we = 1'b1;
                else d_tag_we = 1'b1;
            end
            default: ;
        endcase
    end

    // -- config registers ------------------------------------------

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            cfg_ack_o <= 1'b0;
            cfg_dat_o <= 32'b0;
            cfg_i_flush_req <= 1'b0;
            cfg_d_flush_req <= 1'b0;
            cfg_clr_loads <= 1'b0;
            cfg_clr_stores <= 1'b0;
            cfg_clr_stall <= 1'b0;
            cfg_clr_wbfull <= 1'b0;
            cfg_clr_isnoop <= 1'b0;
            i_enable <= 1'b1;
            d_enable <= 1'b0;
            wb_enable <= 1'b0;
        end else begin
            cfg_ack_o <= 1'b0;
            cfg_i_flush_req <= 1'b0;
            cfg_d_flush_req <= 1'b0;
            cfg_clr_loads <= 1'b0;
            cfg_clr_stores <= 1'b0;
            cfg_clr_stall <= 1'b0;
            cfg_clr_wbfull <= 1'b0;
            cfg_clr_isnoop <= 1'b0;

            if (c_cyc_i && c_stb_i && cfg_sel && !cfg_ack_o) begin
                cfg_ack_o <= 1'b1;
                if (c_we_i) begin
                    case (cfg_reg)
                        4'd0: begin
                            i_enable <= c_dat_i[0];
                            cfg_i_flush_req <= c_dat_i[1];
                        end
                        4'd4: begin
                            d_enable <= c_dat_i[0];
                            wb_enable <= c_dat_i[2];
                            // enabling D flushes it: lines are kept
                            // coherent while disabled anyway (stores
                            // still update on hit), this is belt and
                            // braces, and it zeroes the counters
                            cfg_d_flush_req <= c_dat_i[1] ||
                                (c_dat_i[0] && !d_enable);
                        end
                        4'd8:  cfg_clr_loads <= 1'b1;
                        4'd9:  cfg_clr_stores <= 1'b1;
                        4'd10: cfg_clr_stall <= 1'b1;
                        4'd12: cfg_clr_wbfull <= 1'b1;
                        4'd13: cfg_clr_isnoop <= 1'b1;
                        default: ;
                    endcase
                end else begin
                    case (cfg_reg)
                        4'd0:  cfg_dat_o <= { 31'b0, i_enable };
                        4'd1:  cfg_dat_o <= stat_i_hits;
                        4'd2:  cfg_dat_o <= stat_i_misses;
                        4'd3:  cfg_dat_o <= { 16'h1CAC,
                                   I_LINE_WORDS[7:0], I_KB[7:0] };
                        4'd4:  cfg_dat_o <= { 29'b0, wb_enable, 1'b0,
                                   d_enable };
                        4'd5:  cfg_dat_o <= stat_d_hits;
                        4'd6:  cfg_dat_o <= stat_d_misses;
                        4'd7:  cfg_dat_o <= { 16'h1DCA,
                                   D_LINE_WORDS[7:0], D_KB[7:0] };
                        4'd8:  cfg_dat_o <= stat_loads;
                        4'd9:  cfg_dat_o <= stat_stores;
                        4'd10: cfg_dat_o <= stat_stall;
                        4'd11: cfg_dat_o <= { 16'h0, 5'b0,
                                   (SNOOP != 0), (BURST != 0),
                                   (FAST_HIT != 0), WBUF_DEPTH[7:0] };
                        4'd12: cfg_dat_o <= stat_wbfull;
                        4'd13: cfg_dat_o <= stat_isnoop;
                        default: cfg_dat_o <= 32'b0;
                    endcase
                end
            end
        end
    end

    // -- main state machine ----------------------------------------
    //
    // Drives every m_* output, the write buffer and the statistics.
    // Two things can own the memory port: the main FSM (S_FILL..,
    // S_BYPASS) and the write-buffer drain (dr_busy). The main FSM
    // only takes it when bus_free; the drain only starts while the
    // main FSM does not own it. They never overlap.

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            state <= S_FLUSH;          // BRAM is undefined out of reset
            flush_i <= 1'b1;
            flush_d <= 1'b1;
            flush_idx <= 0;
            flush_pending_i <= 1'b0;
            flush_pending_d <= 1'b0;
            req_kind <= K_BYP;
            req_adr <= 32'b0;
            req_dat <= 32'b0;
            req_sel <= 4'b0;
            req_we <= 1'b0;
            fill_cnt <= 0;
            fill_i <= 1'b0;
            fill_want_dat <= 32'b0;
            fill_want_got <= 1'b0;
            ack_r <= 1'b0;
            dat_r <= 32'b0;
            m_adr_o <= 32'b0;
            m_dat_o <= 32'b0;
            m_we_o <= 1'b0;
            m_sel_o <= 4'b0;
            m_stb_o <= 1'b0;
            m_cyc_o <= 1'b0;
            m_cti_o <= 3'b000;
            wb_rp <= 0;
            wb_wp <= 0;
            wb_cnt <= 0;
            dr_busy <= 1'b0;
            snoop_pending <= 1'b0;
            snoop_adr <= 32'b0;
            stat_i_hits <= 32'b0;
            stat_i_misses <= 32'b0;
            stat_d_hits <= 32'b0;
            stat_d_misses <= 32'b0;
            stat_loads <= 32'b0;
            stat_stores <= 32'b0;
            stat_stall <= 32'b0;
            stat_wbfull <= 32'b0;
            stat_isnoop <= 32'b0;
            stall_pulse <= 1'b0;
        end else begin

            ack_r <= 1'b0;

            // -- flush requests: latch, act from S_IDLE only --------
            if (cfg_i_flush_req) flush_pending_i <= 1'b1;
            if (cfg_d_flush_req) flush_pending_d <= 1'b1;

            // -- snoop from other masters ---------------------------
            // One pending entry. A second arrival before the first is
            // serviced cannot be tracked, so it becomes a full flush:
            // slower, never wrong.
            if (SNOOP != 0 && snoop_stb_i) begin
                if (snoop_pending) begin
                    flush_pending_i <= 1'b1;
                    flush_pending_d <= 1'b1;
                end else begin
                    snoop_pending <= 1'b1;
                    snoop_adr <= snoop_adr_i;
                end
            end

            // -- statistics -----------------------------------------
            if (cfg_clr_loads) stat_loads <= 32'b0;
            if (cfg_clr_stores) stat_stores <= 32'b0;
            if (cfg_clr_wbfull) stat_wbfull <= 32'b0;
            if (cfg_clr_isnoop) stat_isnoop <= 32'b0;
            stall_pulse <= c_req && !c_ack_o;
            if (cfg_clr_stall) stat_stall <= 32'b0;
            else if (stall_pulse) stat_stall <= stat_stall + 1;

            // -- write buffer drain ---------------------------------
            if (dr_busy) begin
                if (m_ack_i) begin
                    m_stb_o <= 1'b0;
                    m_cyc_o <= 1'b0;
                    m_we_o <= 1'b0;
                    dr_busy <= 1'b0;
                end
            end else if ((wb_cnt != 0) && !bus_owned) begin
                m_adr_o <= wb_adr[wb_rp];
                m_dat_o <= wb_dat[wb_rp];
                m_sel_o <= wb_sel[wb_rp];
                m_we_o <= 1'b1;
                m_cti_o <= 3'b000;
                m_stb_o <= 1'b1;
                m_cyc_o <= 1'b1;
                dr_busy <= 1'b1;
            end

            // pop on drain completion; push in S_LOOKUP (below). The
            // count is updated in exactly one place.
            if (dr_busy && m_ack_i) wb_rp <= wb_rp + 1'b1;
            if (st_go && post_en) begin
                wb_adr[wb_wp] <= req_adr;
                wb_dat[wb_wp] <= req_dat;
                wb_sel[wb_wp] <= req_sel;
                wb_wp <= wb_wp + 1'b1;
            end
            case ({ st_go && post_en, dr_busy && m_ack_i })
                2'b10: wb_cnt <= wb_cnt + 1'b1;
                2'b01: wb_cnt <= wb_cnt - 1'b1;
                default: ;
            endcase

            // -- main FSM -------------------------------------------
            case (state)

                S_FLUSH: begin
                    if (flush_idx == (MAXL-1)) begin
                        flush_idx <= 0;
                        if (flush_i) begin
                            stat_i_hits <= 32'b0;
                            stat_i_misses <= 32'b0;
                        end
                        if (flush_d) begin
                            stat_d_hits <= 32'b0;
                            stat_d_misses <= 32'b0;
                        end
                        state <= S_IDLE;
                    end else begin
                        flush_idx <= flush_idx + 1'b1;
                    end
                end

                S_IDLE: begin
                    if (snoop_pending) begin
                        // the invalidating write happens this cycle
                        // (see the always @* above); no request is
                        // accepted, so none can read the old tag. A
                        // snoop arriving this same cycle was already
                        // turned into a full flush above.
                        snoop_pending <= 1'b0;
                    end else if (flush_pending_i || flush_pending_d) begin
                        flush_i <= flush_pending_i;
                        flush_d <= flush_pending_d;
                        flush_pending_i <= cfg_i_flush_req;
                        flush_pending_d <= cfg_d_flush_req;
                        flush_idx <= 0;
                        state <= S_FLUSH;
                    // !ack_r, not !c_ack_o: the combinational acks
                    // (hit_now, st_ack_now) exist only in S_LOOKUP, so
                    // here they are always 0 -- but synthesis cannot
                    // see that, and testing c_ack_o put the whole
                    // BRAM -> tag compare -> ack path in front of
                    // every register this state loads. That was the
                    // SOC's critical path (53.9MHz on mozart_ml1).
                    end else if (c_req && !ack_r) begin
                        req_adr <= c_adr_i;
                        req_dat <= c_dat_i;
                        req_sel <= c_sel_i;
                        req_we <= c_we_i;

                        if (c_main && c_we_i) begin
                            req_kind <= K_ST;
                            stat_stores <= stat_stores + 1;
                            state <= S_LOOKUP;
                        end else if (c_main && c_instr_i && i_enable) begin
                            req_kind <= K_IF;
                            state <= S_LOOKUP;
                        end else if (c_main && !c_instr_i && d_enable) begin
                            req_kind <= K_LD;
                            stat_loads <= stat_loads + 1;
                            state <= S_LOOKUP;
                        end else begin
                            req_kind <= K_BYP;
                            if (c_main && !c_instr_i)
                                stat_loads <= stat_loads + 1;
                            if (bus_free) begin
                                m_adr_o <= c_adr_i;
                                m_dat_o <= c_dat_i;
                                m_sel_o <= c_sel_i;
                                m_we_o <= c_we_i;
                                m_cti_o <= 3'b000;
                                m_stb_o <= 1'b1;
                                m_cyc_o <= 1'b1;
                                state <= S_BYPASS;
                            end else begin
                                state <= S_MEMREQ;
                            end
                        end
                    end
                end

                S_LOOKUP: begin
                    case (req_kind)
                        K_IF: begin
                            if (i_hit) begin
                                dat_r <= FAST_HIT ? 32'b0 : i_data_q;
                                ack_r <= FAST_HIT ? 1'b0 : 1'b1;
                                stat_i_hits <= stat_i_hits + 1;
                                state <= S_IDLE;
                            end else begin
                                stat_i_misses <= stat_i_misses + 1;
                                fill_i <= 1'b1;
                                state <= S_MEMREQ;
                            end
                        end
                        K_LD: begin
                            if (d_hit) begin
                                dat_r <= FAST_HIT ? 32'b0 : d_data_q;
                                ack_r <= FAST_HIT ? 1'b0 : 1'b1;
                                stat_d_hits <= stat_d_hits + 1;
                                state <= S_IDLE;
                            end else begin
                                stat_d_misses <= stat_d_misses + 1;
                                fill_i <= 1'b0;
                                state <= S_MEMREQ;
                            end
                        end
                        default: begin   // K_ST
                            if (st_go) begin
                                // array updates happen in always @*
                                if (i_hit) stat_isnoop <= stat_isnoop + 1;
                                if (post_en) begin
                                    ack_r <= FAST_HIT ? 1'b0 : 1'b1;
                                    state <= S_IDLE;
                                end else begin
                                    // blocking write-through
                                    req_kind <= K_BYP;
                                    state <= S_MEMREQ;
                                end
                            end else begin
                                stat_wbfull <= stat_wbfull + 1;
                            end
                        end
                    endcase
                end

                // Launch the pending bus operation once every posted
                // store has reached memory. K_BYP is a bypass (read or
                // write); K_IF/K_LD are line fills.
                //
                // Every fill and every blocking store starts HERE, even
                // when the port is already free in S_LOOKUP. Launching
                // from S_LOOKUP as well saves one cycle per miss but
                // duplicates the whole launch mux: measured 378 LUT4 on
                // ECP5 (2247 vs 1869), for ~0.1% of cycles in
                // rtl/tb/tb_cache_soc.v. Only uncached accesses from
                // S_IDLE still launch directly, because those are the
                // latency-sensitive peripheral reads and writes.
                S_MEMREQ: begin
                    if (bus_free) begin
                        m_stb_o <= 1'b1;
                        m_cyc_o <= 1'b1;
                        if (req_kind == K_BYP) begin
                            m_adr_o <= req_adr;
                            m_dat_o <= req_dat;
                            m_sel_o <= req_sel;
                            m_we_o <= req_we;
                            m_cti_o <= 3'b000;
                            state <= S_BYPASS;
                        end else begin
                            // Fill from the start of the line. sel is
                            // 0000 and we is 0: a read, whichever
                            // convention the controller follows.
                            fill_cnt <= 0;
                            fill_want_got <= 1'b0;
                            m_adr_o <= fill_base;
                            m_dat_o <= 32'b0;
                            m_sel_o <= 4'b0000;
                            m_we_o <= 1'b0;
                            m_cti_o <= fill_burst ? 3'b010 : 3'b000;
                            state <= S_FILL;
                        end
                    end
                end

                S_FILL: begin
                    if (m_ack_i) begin
                        if (fill_i ? (fill_cnt == req_iwoff) :
                                     (fill_cnt == req_dwoff)) begin
                            fill_want_dat <= m_dat_i;
                            fill_want_got <= 1'b1;
                        end

                        if (fill_last) begin
                            // CYC is held until S_FILL_END
                            m_stb_o <= 1'b0;
                            m_cti_o <= 3'b000;
                            state <= S_FILL_END;
                        end else if (fill_i ? I_BURST : D_BURST) begin
                            // burst: STB stays up, next beat now
                            fill_cnt <= fill_cnt + 1'b1;
                            m_adr_o <= m_adr_o + 32'd4;
                            m_cti_o <= fill_cti_next;
                        end else begin
                            // classic: STB drops between words, CYC
                            // does not (keeps the arbiter grant and
                            // sdram_kianv.v's open row)
                            m_stb_o <= 1'b0;
                            fill_cnt <= fill_cnt + 1'b1;
                            state <= S_FILL_SEQ;
                        end
                    end
                end

                S_FILL_SEQ: begin
                    m_adr_o <= fill_i ?
                        { req_adr[31:I_IDXLSB], fill_cnt[I_WOFFB-1:0], 2'b00 } :
                        { req_adr[31:D_IDXLSB], fill_cnt[D_WOFFB-1:0], 2'b00 };
                    m_stb_o <= 1'b1;
                    state <= S_FILL;
                end

                S_FILL_END: begin
                    m_cyc_o <= 1'b0;
                    // tag written valid in always @*. A flush or snoop
                    // that arrived during the fill is pending and is
                    // serviced from S_IDLE before any lookup.
                    dat_r <= fill_want_dat;
                    ack_r <= fill_want_got;
                    state <= S_IDLE;
                end

                S_BYPASS: begin
                    if (m_ack_i) begin
                        dat_r <= m_dat_i;
                        ack_r <= 1'b1;
                        m_stb_o <= 1'b0;
                        m_cyc_o <= 1'b0;
                        m_we_o <= 1'b0;
                        state <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;

            endcase
        end
    end

endmodule
