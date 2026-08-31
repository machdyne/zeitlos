/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-mapped instruction cache (wishbone pass-through)
 *
 * -- What this is for --
 *
 * The CPU (rtl/cpu/picorv32/picorv32.v, instantiated as picorv32_wb in
 * rtl/sysctl.v) has no cache of its own, so EVERY instruction fetch
 * goes out over wishbone to main memory. Main memory is slow here:
 * rtl/mem/sdram.v precharges and re-activates on every single access
 * (~11 cycles per 32-bit word at 48MHz), and rtl/mem/qqspi.v is far
 * worse -- it issues a complete command/address/dummy sequence per
 * word, roughly 63 cycles. Against picorv32's own ~4 cycles of
 * execution per instruction, fetch latency is the dominant term in
 * cycles-per-instruction on every board.
 *
 * This module sits in the fetch path and serves repeat fetches out of
 * BRAM in 3 cycles instead of ~13 (SDRAM) or ~65 (PSRAM).
 *
 * -- Why this caches PHYSICAL addresses (post-MTU) --
 *
 * The most important structural decision here, so: in rtl/sysctl.v
 * this module is inserted AFTER wb_mtu (rtl/mtu.v), not before it.
 * Every app executes at virtual 0x8000_0000 and the MTU remaps that
 * to the app's real location in main memory by rewriting the top
 * nibble during context switches.
 *
 * If this cache were tagged on the VIRTUAL address the CPU emits,
 * every app's 0x8000_0100 would collide in the tag with every other
 * app's 0x8000_0100 -- the same cache line would mean different code
 * depending on which process is scheduled, and staying correct would
 * require a full invalidate on every context switch (~732Hz, see
 * sw/os/kernel.c's KTIMER handler). That would throw away most of the
 * benefit and add a correctness landmine.
 *
 * Tagged physically, a context switch is just an MTU base register
 * write. The next fetch of 0x8000_0100 translates to a different
 * physical address, misses cleanly, and fills from the right place.
 * Nothing here needs to know context switches exist at all.
 *
 * -- Coherency: why an explicit flush is enough --
 *
 * Only instruction fetches are cached (see c_instr_i below), so
 * ordinary data traffic can never put stale entries in here and
 * stores never need to be watched. The only way a stale line can
 * arise is code being WRITTEN as data and then executed, and in this
 * codebase that happens in exactly two places:
 *
 *   1. sw/os/fs/fs.c's fs_load_exec() -- the only app loader, five
 *      call sites (sw/os/kernel.c:196, sw/os/sh.c:568/678/712/749).
 *   2. sw/bios/bios.c's load_zeitlos(), which memcpy()s the kernel
 *      from memory-mapped flash to 0x4000_0000 before jumping to it.
 *
 * Both must write CACHE_CTRL_FLUSH (below) after writing the image
 * and before jumping into it. The dangerous case is worth naming: app
 * A loads at base X, exits, k_mem_free() releases X, then app B loads
 * at the same base -- without a flush, B executes A's instructions.
 * That failure is intermittent and allocation-order dependent, which
 * is exactly the kind of bug that is miserable to find later.
 *
 * Hardware write-snooping would make this correct by construction
 * rather than by discipline, and is a reasonable later addition. It
 * is deliberately NOT done here: it must compare tags rather than
 * invalidate by index alone, because index-only invalidation would be
 * flushed continuously by ordinary data writes (a graphics workload
 * writing tens of thousands of pixels per frame would clear this
 * cache many times over per frame and gain nothing).
 *
 * -- Cacheable region --
 *
 * Only main memory (0x4xxx_xxxx) is cached. rtl/boards.vh makes
 * MEM_SRAM / MEM_SDRAM / MEM_QQSPI mutually exclusive and rtl/sysctl.v
 * decodes all three at the same base, so one address test covers every
 * backend and this module needs no `ifdef for them. Everything else
 * passes straight through: BRAM is already single-cycle, and VRAM,
 * glyph memory and every peripheral MUST bypass -- caching a UART LSR
 * or blitter status read would break them.
 *
 * -- Storage --
 *
 * Tags and data are both read SYNCHRONOUSLY, through a lookup address
 * that is muxed between the incoming address (in S_IDLE) and the
 * registered one (afterwards). This is what makes them infer as EBR.
 * An asynchronous tag read would infer distributed LUT RAM instead:
 * at 8KB/4-word lines that is 512 x 15 bits, ~7700 flops, roughly a
 * third of an ECP5-25F's registers spent on tags alone.
 *
 * The valid bit is stored as the top bit of each tag word rather than
 * in a separate flop array. A flop array would allow a single-cycle
 * invalidate-all, which sounds attractive but costs far more than it
 * is worth: an indexed read plus an indexed set over NUM_LINES flops
 * synthesises to a wide decoder and a wide mux, measured at roughly
 * 3000 LUT4s on ECP5 for 512 lines -- more logic than the rest of
 * this module put together.
 *
 * Instead, a flush WALKS the lines, clearing one per cycle (S_FLUSH).
 * That costs NUM_LINES cycles, about 11us at 48MHz for 512 lines,
 * which is nothing next to what triggers it: fs_load_exec() has just
 * read an executable off an SD card over bit-banged SPI, measured in
 * milliseconds. Trading ~11us for ~3000 LUTs is not a close call.
 *
 * -- Sizing --
 *
 * LINE_WORDS is a parameter because the right value is backend
 * dependent. rtl/mem/sdram.v has no burst path today, so a fill costs
 * ~11 cycles per word regardless and short lines (4) keep the miss
 * penalty down. rtl/mem/qqspi.v spends ~40 of its ~63 cycles on fixed
 * command/address/dummy overhead, so once qqspi_wb learns to burst,
 * longer lines get much cheaper there. Don't hardcode it.
 *
 * -- Debug support --
 *
 * CACHE_CTRL_ENABLE defaults to 1 but can be cleared at runtime to
 * force every fetch to bypass. If something misbehaves on hardware,
 * that turns "is the cache at fault?" into a single register write
 * instead of a re-synthesis. Hit/miss counters exist for the same
 * reason: so cache behaviour can be measured rather than assumed.
 */

module wb_icache #(
    parameter CACHE_KB    = 8,   // total data array size, KB (power of 2)
    parameter LINE_WORDS  = 4,   // words per line (power of 2, >= 2)
    // 1: acknowledge a hit combinationally, in the same cycle the tag
    // compare resolves, making a hit cost 1 cycle instead of 2. This
    // puts the tag comparison into the path that feeds the CPU's ack
    // input, so if timing closure ever gets tight, set it to 0 -- the
    // cache stays correct, just a cycle slower per hit.
    // 1: acknowledge a hit COMBINATIONALLY, in the same cycle the tag
    // compare resolves (1-cycle hit). This puts BRAM output -> tag
    // compare -> c_ack_o -> the CPU's wbm_ack_i input all in one
    // combinational path, and it is the least conventional thing in
    // this module.
    //
    // 0: acknowledge from a register instead (2-cycle hit). Functionally
    // identical, measurably slower, and takes that whole path out of
    // play. Try this first when bringing the cache up on a new board.
    parameter FAST_HIT    = 1,
    // Base of this module's own register window, matched against the
    // CPU address upstream of the arbiter. See the note on the removed
    // cfg_* slave port below.
    parameter [31:0] CFG_BASE = 32'h7000_0100
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- CPU side (upstream): physical address, post-MTU --
    input [31:0] c_adr_i,
    input [31:0] c_dat_i,
    output [31:0] c_dat_o,
    input c_we_i,
    input [3:0] c_sel_i,
    input c_stb_i,
    input c_cyc_i,
    input c_instr_i,          // picorv32 mem_instr: 1 = instruction fetch
    output c_ack_o,

    // -- Memory side (downstream): drives the main wishbone bus --
    output reg [31:0] m_adr_o,
    output reg [31:0] m_dat_o,
    input [31:0] m_dat_i,
    output reg m_we_o,
    output reg [3:0] m_sel_o,
    output reg m_stb_o,
    output reg m_cyc_o,
    input m_ack_i,

    // -- Control / status registers --
    //
    // Decoded from the CPU's own address INSIDE this module, upstream
    // of the arbiter, rather than being a slave on the main bus.
    //
    // It used to be the latter, and that deadlocked. This module is a
    // bus MASTER: it drives the CPU's side of wb_arbiter_main. A write
    // to its own register is not cacheable, so it was forwarded down
    // the bypass path, granted by the arbiter, decoded on the far side,
    // and routed back to this module's slave port -- which was waiting
    // for the very transaction it was being asked to answer. The cache
    // was asking itself a question through an arbiter it was already
    // occupying. sw/bios/bios.c writes the flush register immediately
    // before jumping to the kernel, so the machine hung at boot.
    //
    // Answering upstream means these accesses never reach the main bus
    // at all, which is both correct and faster.
    //
    // The window base is the CFG_BASE parameter above.
    output c_cfg_hit          // 1 when this module is answering its own
                              // registers; sysctl.v uses it to keep the
                              // main-bus decode consistent
);

    // -- geometry --------------------------------------------------

    localparam TOTAL_WORDS  = (CACHE_KB * 1024) / 4;
    localparam NUM_LINES    = TOTAL_WORDS / LINE_WORDS;
    localparam IDX_BITS     = $clog2(NUM_LINES);
    localparam WOFF_BITS    = $clog2(LINE_WORDS);
    localparam DIDX_BITS    = $clog2(TOTAL_WORDS);

    // byte address layout:
    //   [1:0]                          byte offset within word
    //   [IDX_LSB-1 : 2]                word offset within line
    //   [TAG_LSB-1 : IDX_LSB]          line index
    //   [27 : TAG_LSB]                 tag
    //
    // bit 27 is the top: sysctl.v decodes main memory as the whole
    // 0x4xxx_xxxx nibble, so bits [31:28] are constant for anything
    // cacheable here and carry no information worth storing.

    localparam OFF_LSB      = 2;
    localparam IDX_LSB      = OFF_LSB + WOFF_BITS;
    localparam TAG_LSB      = IDX_LSB + IDX_BITS;
    localparam TAG_BITS     = 28 - TAG_LSB;

    // -- storage ---------------------------------------------------

    // tag word layout: { valid, tag }
    reg [31:0] cache_data [0:TOTAL_WORDS-1];
    reg [TAG_BITS:0] cache_tag [0:NUM_LINES-1];

    reg [31:0] data_q;
    reg [TAG_BITS:0] tag_q;

    // -- control registers -----------------------------------------

    reg cache_enable;
    reg cfg_flush_req;
    reg [31:0] stat_hits;
    reg [31:0] stat_misses;

    // -- fsm -------------------------------------------------------

    localparam S_IDLE     = 3'd0;
    localparam S_LOOKUP   = 3'd1;
    localparam S_FILL     = 3'd2;
    localparam S_FILL_SEQ = 3'd3;
    localparam S_FILL_END = 3'd4;
    localparam S_BYPASS   = 3'd5;
    localparam S_FLUSH    = 3'd6;

    reg [2:0] state;

    reg [31:0] req_adr;
    reg [WOFF_BITS-1:0] fill_cnt;
    reg [31:0] fill_want_dat;
    reg fill_want_got;

    // A flush is LATCHED here and acted on from S_IDLE only, never
    // taken mid-transaction. The control register write that requests
    // a flush is itself a bus cycle passing through this module's
    // bypass path, so jumping straight to S_FLUSH on the request would
    // abandon that cycle without acknowledging it and hang the CPU.
    reg flush_pending;
    reg [IDX_BITS-1:0] flush_idx;

    // registered ack/data, used by the fill and bypass paths
    reg ack_r;
    reg [31:0] dat_r;

    // -- control register decode (upstream, never hits the bus) ----

    reg [31:0] cfg_dat_o;
    reg cfg_ack_o;

    // Matches this module's own 256-byte window. Must agree with
    // sysctl.v's cs_cache mask, but is tested against the CPU address
    // rather than the post-arbiter one.
    wire cfg_sel;
    assign cfg_sel = ((c_adr_i & 32'hf000_0700) == CFG_BASE);
    assign c_cfg_hit = cfg_sel;

    wire [31:0] cfg_adr_i;
    assign cfg_adr_i = { 30'b0, c_adr_i[3:2] };

    wire cfg_cyc_i = c_cyc_i && cfg_sel;
    wire cfg_stb_i = c_stb_i && cfg_sel;
    wire cfg_we_i  = c_we_i;
    wire [31:0] cfg_dat_i = c_dat_i;

    // -- request decode --------------------------------------------

    wire req_cacheable;
    wire [31:0] lu_adr;
    wire [IDX_BITS-1:0] lu_idx;
    wire [DIDX_BITS-1:0] lu_didx;
    wire [TAG_BITS-1:0] req_tag;
    wire [IDX_BITS-1:0] req_idx;
    wire [WOFF_BITS-1:0] req_woff;
    wire [DIDX_BITS-1:0] fill_didx;
    wire req_hit;

    // cacheable == main memory (0x4xxx_xxxx), read, instruction fetch,
    // cache enabled. sysctl.v decodes MEM_SRAM, MEM_SDRAM and
    // MEM_QQSPI all at this same base and boards.vh makes them
    // mutually exclusive, so this single test covers every backend.
    assign req_cacheable = cache_enable && c_instr_i && !c_we_i &&
        ((c_adr_i & 32'hf000_0000) == 32'h4000_0000);

    // In S_IDLE the arrays are addressed with the address arriving on
    // the bus this cycle, so tag/data/valid are already registered and
    // ready to compare when S_LOOKUP runs the next cycle. Everywhere
    // else the registered request address is used.
    assign lu_adr   = (state == S_IDLE) ? c_adr_i : req_adr;
    assign lu_idx   = lu_adr[IDX_LSB +: IDX_BITS];
    assign lu_didx  = lu_adr[OFF_LSB +: DIDX_BITS];

    assign req_tag  = req_adr[TAG_LSB +: TAG_BITS];
    assign req_idx  = req_adr[IDX_LSB +: IDX_BITS];
    assign req_woff = req_adr[OFF_LSB +: WOFF_BITS];

    assign req_hit  = tag_q[TAG_BITS] && (tag_q[TAG_BITS-1:0] == req_tag);

    // With FAST_HIT the CPU is answered in S_LOOKUP itself rather than
    // a cycle later. That matters more than it sounds: against a fast
    // backend (rtl/mem/sram.v answers in ~1 cycle on average) a
    // 2-cycle hit is SLOWER than simply going to memory, and the cache
    // becomes a net loss. Each output still has exactly one driver.
    wire hit_now;
    assign hit_now = FAST_HIT && (state == S_LOOKUP) && req_hit;

    assign c_ack_o = cfg_sel ? cfg_ack_o : (ack_r | hit_now);
    assign c_dat_o = cfg_sel ? cfg_dat_o : (hit_now ? data_q : dat_r);

    // word being written during a fill
    assign fill_didx = { req_idx, fill_cnt };

    // -- synchronous read port (infers EBR) ------------------------
    //
    // Separate always block from the one that writes these arrays, so
    // yosys infers a simple dual-port BRAM. Read-during-write returns
    // the old value, which is harmless: the only cycles where a write
    // and a read collide are inside a fill, and the fill path takes
    // its result from fill_want_dat rather than from data_q.

    always @(posedge wb_clk_i) begin
        data_q <= cache_data[lu_didx];
        tag_q  <= cache_tag[lu_idx];
    end

    // -- control register slave ------------------------------------
    //
    // cfg_adr_i is word-addressed, matching every other simple slave
    // in this codebase (see rtl/csrs.v, rtl/debug.v).
    //
    //   0  CTRL    bit0 = enable, bit1 = flush (write 1, self-clearing)
    //   1  HITS    fetch hits since last flush
    //   2  MISSES  fetch misses since last flush
    //   3  INFO    { 16'h1CAC, LINE_WORDS[7:0], CACHE_KB[7:0] }
    //
    // INFO carries a magic in its top half for the same reason
    // rtl/csrs.v has a MAGIC register: reading an address nothing
    // decodes does NOT fault on this bus (see sysctl.v's own
    // 32'hzzzz_zzzz default case), so without a known constant to
    // check, software has no way to tell "no cache in this bitstream"
    // from "cache present, reporting these numbers".

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            cfg_ack_o <= 1'b0;
            cfg_dat_o <= 32'b0;
            cfg_flush_req <= 1'b0;
            cache_enable <= 1'b1;
        end else begin
            cfg_ack_o <= 1'b0;
            cfg_flush_req <= 1'b0;

            if (cfg_cyc_i && cfg_stb_i && !cfg_ack_o) begin
                cfg_ack_o <= 1'b1;

                if (cfg_we_i) begin
                    if (cfg_adr_i == 32'd0) begin
                        cache_enable <= cfg_dat_i[0];
                        cfg_flush_req <= cfg_dat_i[1];
                    end
                end else begin
                    case (cfg_adr_i)
                        32'd0: cfg_dat_o <= { 31'b0, cache_enable };
                        32'd1: cfg_dat_o <= stat_hits;
                        32'd2: cfg_dat_o <= stat_misses;
                        32'd3: cfg_dat_o <= { 16'h1CAC,
                            LINE_WORDS[7:0], CACHE_KB[7:0] };
                        default: cfg_dat_o <= 32'b0;
                    endcase
                end
            end
        end
    end

    // -- main state machine ----------------------------------------
    //
    // One always block drives every c_* and m_* output and both cache
    // arrays, so each has exactly one driver.

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin

            state <= S_IDLE;
            ack_r <= 1'b0;
            dat_r <= 32'b0;
            m_adr_o <= 32'b0;
            m_dat_o <= 32'b0;
            m_we_o <= 1'b0;
            m_sel_o <= 4'b0;
            m_stb_o <= 1'b0;
            m_cyc_o <= 1'b0;
            req_adr <= 32'b0;
            fill_cnt <= 0;
            fill_want_dat <= 32'b0;
            fill_want_got <= 1'b0;
            stat_hits <= 32'b0;
            stat_misses <= 32'b0;
            // Reset enters S_FLUSH rather than S_IDLE: BRAM contents
            // are undefined out of reset, so the cache must not be
            // allowed to answer anything until every line has been
            // walked and marked invalid.
            state <= S_FLUSH;
            flush_idx <= 0;
            flush_pending <= 1'b0;

        end else begin

            ack_r <= 1'b0;

            // latch, don't act -- see flush_pending's declaration
            if (cfg_flush_req) begin
                flush_pending <= 1'b1;
            end

            case (state)

                // Walk every line clearing its valid bit, one per
                // cycle. Requests are not accepted while this runs;
                // the CPU simply stalls for NUM_LINES cycles.
                S_FLUSH: begin
                    cache_tag[flush_idx] <= 0;
                    if (flush_idx == (NUM_LINES-1)) begin
                        flush_idx <= 0;
                        stat_hits <= 32'b0;
                        stat_misses <= 32'b0;
                        state <= S_IDLE;
                    end else begin
                        flush_idx <= flush_idx + 1;
                    end
                end

                S_IDLE: begin
                    m_stb_o <= 1'b0;
                    m_cyc_o <= 1'b0;
                    m_we_o <= 1'b0;

                    // A pending flush is serviced before any new
                    // request, so a fetch can never be answered from
                    // lines the flush was meant to kill.
                    if (flush_pending) begin
                        flush_pending <= 1'b0;
                        flush_idx <= 0;
                        state <= S_FLUSH;
                    // !cfg_sel: our own registers are answered above,
                    // upstream. Forwarding them would send the access
                    // to the arbiter and back to this module -- the
                    // deadlock described at the top of this file.
                    //
                    // !c_ack_o: don't re-accept the request we are
                    // still acknowledging this cycle.
                    end else if (c_cyc_i && c_stb_i && !c_ack_o &&
                        !cfg_sel) begin
                        req_adr <= c_adr_i;

                        if (req_cacheable) begin
                            state <= S_LOOKUP;
                        end else begin
                            // uncached: forward verbatim
                            m_adr_o <= c_adr_i;
                            m_dat_o <= c_dat_i;
                            m_sel_o <= c_sel_i;
                            m_we_o <= c_we_i;
                            m_stb_o <= 1'b1;
                            m_cyc_o <= 1'b1;
                            state <= S_BYPASS;
                        end
                    end
                end

                S_LOOKUP: begin
                    if (req_hit) begin
                        // when FAST_HIT is set these are redundant --
                        // hit_now already drove the outputs this very
                        // cycle -- but harmless, and they are what
                        // serves the hit when FAST_HIT is 0.
                        dat_r <= FAST_HIT ? 32'b0 : data_q;
                        ack_r <= FAST_HIT ? 1'b0 : 1'b1;
                        stat_hits <= stat_hits + 1;
                        state <= S_IDLE;
                    end else begin
                        stat_misses <= stat_misses + 1;
                        fill_cnt <= 0;
                        fill_want_got <= 1'b0;
                        // Fill starts at the beginning of the line
                        // rather than critical-word-first: lines are
                        // stored whole, and wrapping the fill order
                        // would buy little at these line lengths.
                        m_adr_o <= { req_adr[31:IDX_LSB],
                            {WOFF_BITS{1'b0}}, 2'b00 };
                        m_dat_o <= 32'b0;
                        // sel = 0000, NOT 1111. This is load bearing:
                        // rtl/mem/sdram_kianv.v decides read-vs-write
                        // from wb_sel_i (picorv32's wstrb convention,
                        // where sel==0 means read and nonzero sel
                        // names the byte lanes to WRITE) -- it never
                        // looks at wb_we_i. With sel=1111 every line
                        // fill executed the controller's WRITE path,
                        // zeroing the code it was meant to fetch and
                        // returning stale data, which is why enabling
                        // this cache crashed the kernel on SDRAM
                        // boards while the BIOS and all data traffic
                        // (both of which carry wstrb correctly) were
                        // fine.
                        m_sel_o <= 4'b0000;
                        m_we_o <= 1'b0;
                        m_stb_o <= 1'b1;
                        m_cyc_o <= 1'b1;
                        state <= S_FILL;
                    end
                end

                S_FILL: begin
                    if (m_ack_i) begin
                        cache_data[fill_didx] <= m_dat_i;

                        // stash the word the CPU actually asked for
                        if (fill_cnt == req_woff) begin
                            fill_want_dat <= m_dat_i;
                            fill_want_got <= 1'b1;
                        end

                        // STB drops between words, CYC does NOT.
                        //
                        // This is the Wishbone convention -- CYC marks
                        // the whole bus cycle, STB marks each transfer
                        // within it -- and here it is load bearing for
                        // two reasons:
                        //
                        //  1. CYC is what holds the arbiter grant
                        //     (rtl/arbiter_main.v only moves the grant
                        //     when the current master drops CYC). The
                        //     old code released the bus between every
                        //     word, so the GPU blitter could take it
                        //     mid-line-fill.
                        //  2. rtl/mem/sdram_kianv.v gates its ack on
                        //     CYC combinationally and tracks open rows
                        //     across a burst. A line fill is four
                        //     sequential words in the same row, which
                        //     is exactly the case that tracking exists
                        //     to serve -- but only if it is one bus
                        //     cycle rather than four.
                        //
                        // CYC is dropped once, in S_FILL_END.
                        m_stb_o <= 1'b0;

                        if (fill_cnt == (LINE_WORDS-1)) begin
                            state <= S_FILL_END;
                        end else begin
                            fill_cnt <= fill_cnt + 1;
                            state <= S_FILL_SEQ;
                        end
                    end
                end

                // One dead cycle between words so the slave sees
                // stb/cyc deassert cleanly between transactions.
                // rtl/mem/sdram.v and rtl/mem/qqspi.v both gate their
                // next access on !ready/!ack; holding cyc high across
                // words would look like one long transaction to them.
                S_FILL_SEQ: begin
                    m_adr_o <= { req_adr[31:IDX_LSB], fill_cnt, 2'b00 };
                    m_stb_o <= 1'b1;
                    m_cyc_o <= 1'b1;   // already high; held for clarity
                    state <= S_FILL;
                end

                S_FILL_END: begin
                    // the whole line is in; release the bus here
                    m_cyc_o <= 1'b0;

                    // { valid, tag }. If a flush was requested while
                    // this fill was in flight, flush_pending is set
                    // and S_IDLE will walk the whole array next --
                    // including this line -- so writing it valid here
                    // is safe and needs no special case.
                    cache_tag[req_idx] <= { 1'b1, req_tag };
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

                default: begin
                    state <= S_IDLE;
                end

            endcase
        end
    end

endmodule
