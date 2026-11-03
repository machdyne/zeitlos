/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Redistribution and use in source, binary or physical forms, with or
 * without modification, is permitted provided that the following
 * condition is met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * THIS HARDWARE, SOFTWARE, DATA AND/OR DOCUMENTATION ("THE ASSETS") IS
 * PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE ASSETS OR THE USE OR OTHER DEALINGS IN THE ASSETS. USE AT
 * YOUR OWN RISK.
 *
 */

/*
 * wb_cache_split -- instruction + data cache with separate I and D CPU
 * ports, for the pipelined zeitlos32p (docs/cache_split.md).
 *
 * rtl/cache_id.v's wb_cache serves one CPU request at a time on one
 * Wishbone port: right for picorv32 and zeitlos32, which have one
 * access in flight and need several cycles per instruction anyway. A
 * pipelined core wants to fetch every cycle while a load or store is
 * also in flight, so this module gives each side a port of its own.
 * wb_cache is untouched and still what every other board builds;
 * rtl/sysctl.v builds this one only with `CPU_ZEITLOS32P.
 *
 * Everything that is a rule rather than a structure is the same as
 * wb_cache's, on purpose, so software and the rest of the SOC see no
 * difference:
 *
 *   - Physically tagged; only main memory is cached.
 *   - D is WRITE-THROUGH, UPDATE-ON-HIT, NO WRITE-ALLOCATE. Memory is
 *     never behind the cache: the read-only masters (blitter source,
 *     mixer) always see current data, and nothing needs flushing.
 *   - Every store to main memory SNOOPS the I side: a matching I line
 *     is invalidated. Code written as data is not fetched stale.
 *   - Posted stores are DRAINED BEFORE ANY OTHER BUS ACCESS -- every
 *     fill, every uncached read or write -- so "store a buffer, then
 *     write a register that starts another master reading it" is
 *     ordered with no fence.
 *   - snoop_stb_i invalidates both sides by index (a future writing
 *     master); a second one before the first is serviced becomes a
 *     full flush.
 *   - The same register window at CFG_BASE, answered on the D port;
 *     FEAT bit 11 says this is the split cache.
 *
 * -- The two ports --
 *
 * Same handshake on both. A request is taken at a clock edge where
 * req && rdy. The answer is `ack` for one cycle, with the data (loads,
 * fetches): at the earliest the cycle after it was taken, from a hit;
 * later from memory. A port has at most one request outstanding, but
 * rdy is high again in the cycle of its ack, so a stream of hits goes
 * at one per clock. Address, data, we and sel are sampled with the
 * request and need not be held.
 *
 * The I port only reads. The D port reads and writes and reaches
 * everything: main memory cached, everything else (peripherals, VRAM,
 * the BIOS RAM, this module's own registers) uncached.
 *
 * -- What runs concurrently --
 *
 * I and D look up in parallel, each in its own arrays, so a fetch hit
 * and a load hit complete in the same cycle. One memory port serves
 * both sides' misses and uncached accesses, D first, and the write
 * buffer. While it fills a line for one side, the other side's hits
 * carry on (hit under miss), and stores keep posting until the buffer
 * is full.
 *
 * -- Three hazards, and what is done about each --
 *
 * 1. Store snoop vs fetch. A store reads the I tags through a second
 *    read port of their own (the tag array is duplicated: block RAM or
 *    LUT RAM), so snooping steals no fetch cycles. It invalidates the
 *    cycle after the store is taken; a fetch of that very line in the
 *    same two cycles may still hit the old line. Code is loaded long
 *    before it runs, and the kernel's z_icache_flush() after loading an
 *    app (required on I-cache-only bitstreams anyway) closes even that.
 *
 * 2. Store hit vs the next D access. A store merges its bytes into the
 *    word it read and writes it back the cycle after it is taken. A D
 *    request taken in that cycle reads the array in it too, and for the
 *    SAME WORD would get the old word -- so the merged word is kept for
 *    one cycle and, when the next access's word index matches, used in
 *    place of what the array read (a load returns it; a store merges
 *    into it). Nothing waits: d_rdy never looks at the address, which
 *    keeps the core's address adder off the take/ready path.
 *
 * 3. Stores during a fill. The I side can be filling a line while D
 *    stores post. A store into the very line being filled reaches
 *    memory after the fill read it, so the fill is POISONED: its words
 *    still answer the fetch that asked for them, but the line is not
 *    marked valid. (A D fill cannot meet a CPU store -- the D port is
 *    waiting for it -- but an external snoop poisons either.)
 */

module wb_cache_split #(
    parameter MAIN_512     = 0,
    parameter I_KB         = 8,
    parameter I_LINE_WORDS = 4,
    parameter D_KB         = 4,
    parameter D_LINE_WORDS = 4,
    // Posted-write entries: 0, 1, 2 or 4 (only while D_CTRL bit 2 is set)
    parameter WBUF_DEPTH   = 2,
    // Burst line fills, as wb_cache (sdram_kianv.v BURST)
    parameter BURST        = 0,
    parameter SNOOP        = 1,
    parameter [31:0] CFG_BASE = 32'h7000_0100
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- I port: fetches, physical address --
    input         i_req,
    input  [31:0] i_adr,
    output        i_rdy,
    output        i_ack,
    output [31:0] i_dat,

    // -- D port: loads and stores, physical address --
    input         d_req,
    input  [31:0] d_adr,
    input         d_we,
    input   [3:0] d_sel,
    input  [31:0] d_wdat,
    output        d_rdy,
    output        d_ack,
    output [31:0] d_rdat,

    // -- the MPU's verdicts (rtl/mpu_split.v; tie to 0 without one) --
    // i_mpu_veto, sampled with a fetch when it is taken: blocked
    // (answered 0, never reaches the bus). d_mpu_reg, likewise with a
    // load or store: it is for the MPU's own registers, answered the
    // cycle after LOOK with d_mpu_dat, which the MPU supplies then.
    // d_mpu_veto, in the cycle AFTER a load or store is taken: it is
    // blocked (a load reads 0, a store changes nothing).
    input         i_mpu_veto,
    input         d_mpu_veto,
    input         d_mpu_reg,
    input  [31:0] d_mpu_dat,

    // -- memory side: the main bus via wb_arbiter_main --
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
    // high while the memory port carries an access for the I side (a
    // fetch fill or an uncached fetch); rtl/sysctl.v's MPU and any
    // fetch-only logic downstream can tell fetches from data by it
    output reg m_instr_o,

    // -- write snoop from other masters --
    input snoop_stb_i,
    input [31:0] snoop_adr_i,

    // the D port's request this cycle is for the register window
    output d_cfg_hit
);

    // -- geometry (as wb_cache) ------------------------------------

    localparam I_WORDS   = (I_KB * 1024) / 4;
    localparam I_LINES   = I_WORDS / I_LINE_WORDS;
    localparam I_IDXB    = $clog2(I_LINES);
    localparam I_WOFFB   = $clog2(I_LINE_WORDS);
    localparam I_DIDXB   = $clog2(I_WORDS);
    localparam I_IDXLSB  = 2 + I_WOFFB;
    localparam I_TAGLSB  = I_IDXLSB + I_IDXB;
    localparam I_TAGB    = (MAIN_512 ? 29 : 28) - I_TAGLSB;

    localparam D_WORDS   = (D_KB * 1024) / 4;
    localparam D_LINES   = D_WORDS / D_LINE_WORDS;
    localparam D_IDXB    = $clog2(D_LINES);
    localparam D_WOFFB   = $clog2(D_LINE_WORDS);
    localparam D_DIDXB   = $clog2(D_WORDS);
    localparam D_IDXLSB  = 2 + D_WOFFB;
    localparam D_TAGLSB  = D_IDXLSB + D_IDXB;
    localparam D_TAGB    = (MAIN_512 ? 29 : 28) - D_TAGLSB;

    localparam FCW  = (I_WOFFB > D_WOFFB) ? I_WOFFB : D_WOFFB;
    localparam FIB  = (I_IDXB > D_IDXB) ? I_IDXB : D_IDXB;
    localparam MAXL = (I_LINES > D_LINES) ? I_LINES : D_LINES;

    localparam I_BURST = (BURST != 0) && ((I_LINE_WORDS % 4) == 0);
    localparam D_BURST = (BURST != 0) && ((D_LINE_WORDS % 4) == 0);

    localparam WBD  = (WBUF_DEPTH < 1) ? 1 : WBUF_DEPTH;
    localparam WBS  = (WBD < 2) ? 2 : WBD;
    localparam WBPB = $clog2(WBS);

    // -- storage ---------------------------------------------------
    //
    // One write port per array (the always @* below). The I tag array
    // has two synchronous read ports -- fetch and store snoop -- which
    // synthesis builds as two copies written together.

    reg [31:0] i_data [0:I_WORDS-1];
`ifdef CACHE_TAG_LUTRAM
    (* ram_style = "distributed" *)
`endif
    reg [I_TAGB:0] i_tag [0:I_LINES-1];
    reg [31:0] d_data [0:D_WORDS-1];
`ifdef CACHE_TAG_LUTRAM
    (* ram_style = "distributed" *)
`endif
    reg [D_TAGB:0] d_tag [0:D_LINES-1];

    reg [31:0] i_data_q;
    reg [I_TAGB:0] i_tag_q;
    reg [I_TAGB:0] i_tag2_q;		// the store-snoop read
    reg [31:0] d_data_q;
    reg        fw_v;                 // hazard 2: last cycle's store-hit word
    reg [D_DIDXB-1:0] fw_idx;
    reg [31:0] fw_dat;
    reg [D_TAGB:0] d_tag_q;

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
    reg [31:0] cfg_dat;

    reg [31:0] stat_i_hits;
    reg [31:0] stat_i_misses;
    reg [31:0] stat_d_hits;
    reg [31:0] stat_d_misses;
    reg [31:0] stat_loads;
    reg [31:0] stat_stores;
    reg [31:0] stat_stall;
    reg [31:0] stat_wbfull;
    reg [31:0] stat_isnoop;
    reg stall_pulse;
    // Every statistic counts a cycle late, from a registered event:
    // the events come out of the hit compares and the ports' take
    // logic, and a 32-bit increment there was the module's critical
    // path (block RAM out -> hit -> take -> counter enable).
    reg ev_ihit, ev_imiss, ev_dhit, ev_dmiss, ev_load, ev_store;
    reg ev_wbfull, ev_isnoop;

    // -- the ports -------------------------------------------------
    //
    // A port's request is in one of: none (pend 0); LOOK (the cycle
    // after it was taken: the arrays' answer is out, hit or not is
    // known); WAIT (needs the memory port: a miss, or uncached).

    localparam K_IF  = 3'd0;   // cacheable fetch
    localparam K_LD  = 3'd1;   // cacheable load
    localparam K_ST  = 3'd2;   // store to main memory
    localparam K_BYP = 3'd3;   // uncached: straight to the bus
    localparam K_CFG = 3'd4;   // this module's registers
    localparam K_VETO = 3'd5;  // blocked by the MPU: answered 0, no effect
    localparam K_LOC = 3'd6;   // the MPU's registers (answered with d_mpu_dat)

    reg i_pend, i_look, i_wait;
    reg [2:0] i_kind;
    reg [31:0] i_radr;
    reg i_ack_r;
    reg [31:0] i_dat_r;

    reg d_pend, d_look, d_wait;
    reg [2:0] d_kind;
    reg [31:0] d_radr;
    reg [31:0] d_rwdat;
    reg [3:0] d_rsel;
    reg d_rwe;
    reg d_ack_r;
    reg [31:0] d_dat_r;
    reg cfg_done;               // a register-window access is answered
    reg loc_done;               // ...an MPU register access

    // -- memory port -----------------------------------------------

    localparam M_IDLE     = 3'd0;
    localparam M_FILL     = 3'd1;
    localparam M_FILL_SEQ = 3'd2;
    localparam M_FILL_END = 3'd3;
    localparam M_BYPASS   = 3'd4;
    localparam M_FLUSH    = 3'd5;

    (* fsm_encoding = "none" *) reg [2:0] mstate;
    reg m_for_i;                 // the memory port is serving the I side
    reg [FCW-1:0] fill_cnt;
    reg [31:0] fill_want_dat;
    reg fill_poison;

    reg flush_pending_i;
    reg flush_pending_d;
    reg flush_i;
    reg flush_d;
    reg [FIB-1:0] flush_idx;

    reg snoop_pending;
    reg [31:0] snoop_adr;

    // -- write buffer ----------------------------------------------

    reg [31:0] wb_adr [0:WBS-1];
    reg [31:0] wb_dat [0:WBS-1];
    reg [3:0]  wb_sel [0:WBS-1];
    reg [WBPB-1:0] wb_rp;
    reg [WBPB-1:0] wb_wp;
    reg [WBPB:0] wb_cnt;
    reg dr_busy;

    // -- request decode --------------------------------------------

    wire i_main = MAIN_512 ? ((i_adr & 32'he000_0000) == 32'h4000_0000)
                           : ((i_adr & 32'hf000_0000) == 32'h4000_0000);
    wire d_main = MAIN_512 ? ((d_adr & 32'he000_0000) == 32'h4000_0000)
                           : ((d_adr & 32'hf000_0000) == 32'h4000_0000);
    wire d_cfg  = ((d_adr & 32'hf000_0700) == CFG_BASE);
    assign d_cfg_hit = d_req && d_cfg;

    // An external snoop is applied from M_IDLE with both ports quiet.
    wire snoop_svc = snoop_pending && (mstate == M_IDLE) &&
                     !i_pend && !d_pend;

    // Nothing is taken while the arrays are being walked or an external
    // snoop is being applied: both write tags the lookups would read.
    // And nothing NEW is taken while either is waiting to start, or a
    // steady stream of hits would keep it waiting for ever.
    wire arrays_busy = (mstate == M_FLUSH) || snoop_svc ||
                       flush_pending_i || flush_pending_d || snoop_pending;

    wire i_take = i_req && i_rdy;
    wire d_take = d_req && d_rdy;

    // read addresses: the incoming request when one is taken, the
    // pending one otherwise (re-read every cycle while it waits)
    wire [31:0] lu_iadr = i_take ? i_adr : i_radr;
    wire [31:0] lu_dadr = d_take ? d_adr : d_radr;

    wire [I_TAGB-1:0] i_rtag  = i_radr[I_TAGLSB +: I_TAGB];
    wire [I_IDXB-1:0] i_ridx  = i_radr[I_IDXLSB +: I_IDXB];
    wire [I_WOFFB-1:0] i_rwoff = i_radr[2 +: I_WOFFB];
    wire [D_TAGB-1:0] d_rtag  = d_radr[D_TAGLSB +: D_TAGB];
    wire [D_IDXB-1:0] d_ridx  = d_radr[D_IDXLSB +: D_IDXB];
    wire [D_WOFFB-1:0] d_rwoff = d_radr[2 +: D_WOFFB];
    // the store's own line seen from the I side, for the snoop
    wire [I_TAGB-1:0] d_itag  = d_radr[I_TAGLSB +: I_TAGB];
    wire [I_IDXB-1:0] d_iidx  = d_radr[I_IDXLSB +: I_IDXB];

    wire i_hit  = i_tag_q[I_TAGB] && (i_tag_q[I_TAGB-1:0] == i_rtag);
    wire d_hit  = d_tag_q[D_TAGB] && (d_tag_q[D_TAGB-1:0] == d_rtag);
    // the data word in LOOK, with hazard 2's forwarding (below)
    wire d_fwd = fw_v && (fw_idx == d_radr[2 +: D_DIDXB]);
    wire [31:0] d_word = d_fwd ? fw_dat : d_data_q;
    wire i_hit2 = i_tag2_q[I_TAGB] && (i_tag2_q[I_TAGB-1:0] == d_itag);

    wire post_en  = (WBUF_DEPTH > 0) && wb_enable;
    wire wb_full  = (wb_cnt == WBD);
    wire bus_free = (wb_cnt == 0) && !dr_busy;
    wire bus_owned = (mstate == M_FILL) || (mstate == M_FILL_SEQ) ||
                     (mstate == M_FILL_END) || (mstate == M_BYPASS);

    // a store proceeds in its LOOK cycle: posted (needs a free slot), or
    // handed to the memory port to be written through (blocking)
    // The MPU's data verdict arrives the cycle AFTER the take, judged
    // from its own registered copy of the request (rtl/mpu_split.v):
    // the comparators then sit after a register, not after the core's
    // address adder. That cycle is LOOK for a cached access, the first
    // waiting cycle for an uncached one; nothing has reached the
    // arrays, the write buffer or the bus yet, so a veto stops it
    // there: answered with 0, and nothing else.
    reg d_first;                // the cycle after a take
    wire d_vetoed = d_pend && d_first && d_mpu_veto;

    wire st_look = d_pend && d_look && (d_kind == K_ST) && !d_vetoed;
    wire st_go   = st_look && (!post_en || !wb_full);
    wire st_post = st_go && post_en;
    // ...and writes the D array (a hit) and/or invalidates the I line
    wire st_dwr  = st_go && d_hit;
    wire st_iinv = st_go && i_hit2;

    // acknowledgements in LOOK, combinational: a hit, or a posted store
    wire i_hit_ack = i_pend && i_look && (i_kind == K_IF) && i_hit;
    wire d_hit_ack = d_pend && d_look && (d_kind == K_LD) && d_hit && !d_vetoed;
    wire d_st_ack  = st_post;
    wire d_cfg_ack = d_pend && (d_kind == K_CFG) && cfg_done;
    // the MPU's verdicts (rtl/mpu_split.v), taken with the request:
    // blocked -> answered in LOOK with 0; its registers -> answered the
    // cycle after LOOK, as this module's own, with what it supplies
    wire i_veto_ack = i_pend && i_look && (i_kind == K_VETO);
    wire d_veto_ack = d_vetoed;
    wire d_loc_ack  = d_pend && (d_kind == K_LOC) && loc_done;

    assign i_ack = i_hit_ack || i_veto_ack || i_ack_r;
    assign d_ack = d_hit_ack || d_st_ack || d_cfg_ack || d_veto_ack || d_loc_ack || d_ack_r;
    // The data muxes pick by registered state, not by the hit: the tag
    // compare decides only the ack, and stays off the data path (a
    // miss presents the array's word with no ack, and nobody looks).
    wire i_look_if = i_look && (i_kind == K_IF);
    wire d_look_ld = d_look && (d_kind == K_LD);
    assign i_dat = i_look_if ? i_data_q : ((i_kind == K_VETO) ? 32'b0 : i_dat_r);
    assign d_rdat = d_vetoed ? 32'b0 :
                    d_look_ld ? d_word :
                    (d_kind == K_CFG) ? cfg_dat :
                    (d_kind == K_LOC) ? d_mpu_dat :
                    (d_kind == K_VETO) ? 32'b0 : d_dat_r;

    assign i_rdy = !arrays_busy && (!i_pend || i_ack);
    assign d_rdy = !arrays_busy && (!d_pend || d_ack);

    // memory port requests
    wire i_need = i_pend && i_wait;
    wire d_need = d_pend && d_wait && !d_vetoed;

    // Hazard 3: a store going ahead this cycle into the I line that is
    // being filled, or that is about to be (the fill is started from
    // M_IDLE in the same cycle the store posts). Only I fills: the D
    // port cannot store while it waits for its own fill.
    wire st_hits_iline = st_go && (d_radr[31:I_IDXLSB] == i_radr[31:I_IDXLSB]);
    wire i_filling = m_for_i && ((mstate == M_FILL) ||
                     (mstate == M_FILL_SEQ) || (mstate == M_FILL_END));
    wire st_hits_fill = st_hits_iline && i_filling;

    // A fill ends when no store is invalidating an I line this cycle
    // (one I tag write port) and none is poisoning this fill: it waits
    // a cycle, then writes its tag with the poison seen.
    wire fill_end_go = (mstate == M_FILL_END) &&
                       !(m_for_i && st_iinv) && !st_hits_fill;

    wire fill_last = m_for_i ? (fill_cnt == (I_LINE_WORDS-1)) :
                               (fill_cnt == (D_LINE_WORDS-1));
    wire [2:0] fill_cti_next = (m_for_i ? (fill_cnt == (I_LINE_WORDS-2)) :
                                          (fill_cnt == (D_LINE_WORDS-2))) ?
                               3'b111 : 3'b010;
    assign m_bte_o = 2'b00;

    // -- synchronous reads -----------------------------------------

    always @(posedge wb_clk_i) begin
        i_data_q <= i_data[lu_iadr[2 +: I_DIDXB]];
        i_tag_q  <= i_tag[lu_iadr[I_IDXLSB +: I_IDXB]];
        i_tag2_q <= i_tag[lu_dadr[I_IDXLSB +: I_IDXB]];
        d_data_q <= d_data[lu_dadr[2 +: D_DIDXB]];
        d_tag_q  <= d_tag[lu_dadr[D_IDXLSB +: D_IDXB]];
    end

    // -- one write port per array ----------------------------------

    always @(posedge wb_clk_i) if (i_data_we) i_data[i_data_wa] <= i_data_wd;
    always @(posedge wb_clk_i) if (i_tag_we)  i_tag[i_tag_wa]   <= i_tag_wd;
    always @(posedge wb_clk_i) if (d_data_we) d_data[d_data_wa] <= d_data_wd;
    always @(posedge wb_clk_i) if (d_tag_we)  d_tag[d_tag_wa]   <= d_tag_wd;

    wire [31:0] st_merged = {
        d_rsel[3] ? d_rwdat[31:24] : d_word[31:24],
        d_rsel[2] ? d_rwdat[23:16] : d_word[23:16],
        d_rsel[1] ? d_rwdat[15:8]  : d_word[15:8],
        d_rsel[0] ? d_rwdat[7:0]   : d_word[7:0] };

    // Hazard 2: the word a store hit wrote last cycle, for the access
    // in LOOK now (taken in that same cycle, so its array read did not
    // see the write). LOOK is always the cycle after the take.
    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            fw_v <= 1'b0;
        end else begin
            fw_v <= st_dwr;
        end
        if (st_dwr) begin
            fw_idx <= d_radr[2 +: D_DIDXB];
            fw_dat <= st_merged;
        end
    end

    // Every array write. They are exclusive: the flush walk and an
    // external snoop happen only with the ports quiet; a store's I
    // invalidate and a fill's I tag write never share a cycle (the fill
    // waits in M_FILL_END, below); a store's D write and a D fill never
    // overlap (the D port is waiting for its fill).
    always @* begin
        i_data_we = 1'b0;
        i_data_wa = { i_ridx, fill_cnt[I_WOFFB-1:0] };
        i_data_wd = m_dat_i;
        i_tag_we  = 1'b0;
        i_tag_wa  = i_ridx;
        i_tag_wd  = { !fill_poison, i_rtag };
        d_data_we = 1'b0;
        d_data_wa = { d_ridx, fill_cnt[D_WOFFB-1:0] };
        d_data_wd = m_dat_i;
        d_tag_we  = 1'b0;
        d_tag_wa  = d_ridx;
        d_tag_wd  = { !fill_poison, d_rtag };

        if (mstate == M_FLUSH) begin
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
        end else if (snoop_svc) begin
            i_tag_we = 1'b1;
            i_tag_wa = snoop_adr[I_IDXLSB +: I_IDXB];
            i_tag_wd = 0;
            d_tag_we = 1'b1;
            d_tag_wa = snoop_adr[D_IDXLSB +: D_IDXB];
            d_tag_wd = 0;
        end else begin
            if (st_iinv) begin
                i_tag_we = 1'b1;
                i_tag_wa = d_iidx;
                i_tag_wd = 0;
            end else if (fill_end_go && m_for_i) begin
                i_tag_we = 1'b1;
            end
            if (st_dwr) begin
                d_data_we = 1'b1;
                d_data_wa = { d_ridx, d_rwoff };
                d_data_wd = st_merged;
            end else if (mstate == M_FILL && m_ack_i && !m_for_i) begin
                d_data_we = 1'b1;
            end
            if (fill_end_go && !m_for_i)
                d_tag_we = 1'b1;
            if (mstate == M_FILL && m_ack_i && m_for_i)
                i_data_we = 1'b1;
        end
    end

    // -- config registers --------------------------------------------
    //
    // Acted on in LOOK, from the registered request, and answered the
    // cycle after (cfg_done): nothing here depends on this cycle's take,
    // which comes out of the hit compares -- the register decode and the
    // counter clears were the critical path when they did.

    wire cfg_now = d_pend && d_look && (d_kind == K_CFG) && !d_vetoed;
    wire [3:0] cfg_reg = d_radr[5:2];
    wire cfg_wr = cfg_now && d_rwe;
    wire cfg_rd = cfg_now && !d_rwe;

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            cfg_dat <= 32'b0;
            cfg_i_flush_req <= 1'b0;
            cfg_d_flush_req <= 1'b0;
            i_enable <= 1'b1;
            d_enable <= 1'b0;
            wb_enable <= 1'b0;
        end else begin
            cfg_i_flush_req <= 1'b0;
            cfg_d_flush_req <= 1'b0;
            if (cfg_wr) begin
                case (cfg_reg)
                    4'd0: begin
                        i_enable <= d_rwdat[0];
                        cfg_i_flush_req <= d_rwdat[1];
                    end
                    4'd4: begin
                        d_enable <= d_rwdat[0];
                        wb_enable <= d_rwdat[2];
                        cfg_d_flush_req <= d_rwdat[1] ||
                            (d_rwdat[0] && !d_enable);
                    end
                    default: ;
                endcase
            end
            if (cfg_rd) begin
                case (cfg_reg)
                    4'd0:  cfg_dat <= { 31'b0, i_enable };
                    4'd1:  cfg_dat <= stat_i_hits;
                    4'd2:  cfg_dat <= stat_i_misses;
                    4'd3:  cfg_dat <= { 16'h1CAC, I_LINE_WORDS[7:0], I_KB[7:0] };
                    4'd4:  cfg_dat <= { 29'b0, wb_enable, 1'b0, d_enable };
                    4'd5:  cfg_dat <= stat_d_hits;
                    4'd6:  cfg_dat <= stat_d_misses;
                    4'd7:  cfg_dat <= { 16'h1DCA, D_LINE_WORDS[7:0], D_KB[7:0] };
                    4'd8:  cfg_dat <= stat_loads;
                    4'd9:  cfg_dat <= stat_stores;
                    4'd10: cfg_dat <= stat_stall;
                    // bit 11 SPLIT; FAST_HIT (bit 8) is always set here
                    4'd11: cfg_dat <= { 16'h0, 4'b0, 1'b1, (SNOOP != 0),
                               (BURST != 0), 1'b1, WBUF_DEPTH[7:0] };
                    4'd12: cfg_dat <= stat_wbfull;
                    4'd13: cfg_dat <= stat_isnoop;
                    default: cfg_dat <= 32'b0;
                endcase
            end
        end
    end

    // -- everything sequential else --------------------------------

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            mstate <= M_FLUSH;          // block RAM is undefined at reset
            flush_i <= 1'b1;
            flush_d <= 1'b1;
            flush_idx <= 0;
            flush_pending_i <= 1'b0;
            flush_pending_d <= 1'b0;
            snoop_pending <= 1'b0;
            snoop_adr <= 32'b0;
            m_for_i <= 1'b0;
            m_instr_o <= 1'b0;
            fill_cnt <= 0;
            fill_want_dat <= 32'b0;
            fill_poison <= 1'b0;
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
            i_pend <= 1'b0; i_look <= 1'b0; i_wait <= 1'b0;
            i_kind <= K_BYP; i_radr <= 32'b0;
            i_ack_r <= 1'b0; i_dat_r <= 32'b0;
            d_pend <= 1'b0; d_look <= 1'b0; d_wait <= 1'b0;
            d_kind <= K_BYP; d_radr <= 32'b0;
            d_rwdat <= 32'b0; d_rsel <= 4'b0; d_rwe <= 1'b0;
            d_ack_r <= 1'b0; d_dat_r <= 32'b0;
            cfg_done <= 1'b0;
            loc_done <= 1'b0;
            d_first <= 1'b0;
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
            ev_ihit <= 1'b0; ev_imiss <= 1'b0; ev_dhit <= 1'b0; ev_dmiss <= 1'b0;
            ev_load <= 1'b0; ev_store <= 1'b0; ev_wbfull <= 1'b0; ev_isnoop <= 1'b0;
        end else begin

            i_ack_r <= 1'b0;
            d_ack_r <= 1'b0;
            cfg_done <= cfg_now;
            loc_done <= d_pend && d_look && (d_kind == K_LOC);
            d_first <= d_take;

            // -- flush and snoop requests: latched, acted on from idle
            if (cfg_i_flush_req) flush_pending_i <= 1'b1;
            if (cfg_d_flush_req) flush_pending_d <= 1'b1;
            if (SNOOP != 0 && snoop_stb_i) begin
                if (snoop_pending) begin
                    flush_pending_i <= 1'b1;
                    flush_pending_d <= 1'b1;
                end else begin
                    snoop_pending <= 1'b1;
                    snoop_adr <= snoop_adr_i;
                end
                // a line being filled may already hold the old word
                if (mstate != M_IDLE && mstate != M_FLUSH)
                    fill_poison <= 1'b1;
            end
            if (snoop_svc) snoop_pending <= 1'b0;

            // -- statistics
            if (cfg_wr && cfg_reg == 4'd8)  stat_loads <= 32'b0;
            if (cfg_wr && cfg_reg == 4'd9)  stat_stores <= 32'b0;
            if (cfg_wr && cfg_reg == 4'd12) stat_wbfull <= 32'b0;
            if (cfg_wr && cfg_reg == 4'd13) stat_isnoop <= 32'b0;
            stall_pulse <= (i_pend && !i_ack) || (d_pend && !d_ack);
            if (cfg_wr && cfg_reg == 4'd10) stat_stall <= 32'b0;
            else if (stall_pulse) stat_stall <= stat_stall + 1;

            ev_ihit   <= i_hit_ack;
            ev_imiss  <= i_pend && i_look && !i_ack;
            ev_dhit   <= d_hit_ack;
            ev_dmiss  <= d_pend && d_look && (d_kind == K_LD) && !d_ack;
            ev_load   <= d_take && !d_cfg && !d_we && d_main && !d_mpu_veto && !d_mpu_reg;
            ev_store  <= d_take && !d_cfg && d_we && d_main && !d_mpu_veto && !d_mpu_reg;
            ev_wbfull <= st_look && !st_go;
            ev_isnoop <= st_iinv;
            if (ev_ihit)  stat_i_hits   <= stat_i_hits + 1;
            if (ev_imiss) stat_i_misses <= stat_i_misses + 1;
            if (ev_dhit)  stat_d_hits   <= stat_d_hits + 1;
            if (ev_dmiss) stat_d_misses <= stat_d_misses + 1;
            if (ev_load && !(cfg_wr && cfg_reg == 4'd8))  stat_loads  <= stat_loads + 1;
            if (ev_store && !(cfg_wr && cfg_reg == 4'd9)) stat_stores <= stat_stores + 1;
            if (ev_wbfull && !(cfg_wr && cfg_reg == 4'd12)) stat_wbfull <= stat_wbfull + 1;
            if (ev_isnoop && !(cfg_wr && cfg_reg == 4'd13)) stat_isnoop <= stat_isnoop + 1;

            // -- the I port
            if (i_take) begin
                i_pend <= 1'b1;
                i_radr <= i_adr;
                if (i_mpu_veto) begin
                    i_kind <= K_VETO;
                    i_look <= 1'b1;
                    i_wait <= 1'b0;
                end else if (i_main && i_enable) begin
                    i_kind <= K_IF;
                    i_look <= 1'b1;
                    i_wait <= 1'b0;
                end else begin
                    i_kind <= K_BYP;
                    i_look <= 1'b0;
                    i_wait <= 1'b1;
                end
            end else if (i_ack) begin
                i_pend <= 1'b0;
                i_look <= 1'b0;
                i_wait <= 1'b0;
            end else if (i_pend && i_look) begin
                // a miss: to the memory port
                i_look <= 1'b0;
                i_wait <= 1'b1;
            end

            // -- the D port
            if (d_take) begin
                d_pend <= 1'b1;
                d_radr <= d_adr;
                d_rwdat <= d_wdat;
                d_rsel <= d_sel;
                d_rwe <= d_we;
                if (d_mpu_reg) begin
                    d_kind <= K_LOC;
                    d_look <= 1'b1;
                    d_wait <= 1'b0;
                end else if (d_cfg) begin
                    d_kind <= K_CFG;
                    d_look <= 1'b1;
                    d_wait <= 1'b0;
                end else if (d_main && d_we) begin
                    d_kind <= K_ST;
                    d_look <= 1'b1;
                    d_wait <= 1'b0;
                end else if (d_main && d_enable) begin
                    d_kind <= K_LD;
                    d_look <= 1'b1;
                    d_wait <= 1'b0;
                end else begin
                    d_kind <= K_BYP;
                    d_look <= 1'b0;
                    d_wait <= 1'b1;
                end
            end else if (d_ack) begin
                d_pend <= 1'b0;
                d_look <= 1'b0;
                d_wait <= 1'b0;
            end else if (d_pend && d_look) begin
                if (d_kind == K_CFG || d_kind == K_LOC) begin
                    d_look <= 1'b0;
                end else if (d_kind == K_LD) begin
                    d_look <= 1'b0;
                    d_wait <= 1'b1;
                end else if (d_kind == K_ST) begin
                    if (st_go) begin
                        // not posted: written through, blocking
                        d_kind <= K_BYP;
                        d_look <= 1'b0;
                        d_wait <= 1'b1;
                    end
                end
            end

            // -- write buffer: push from a posted store, pop on drain
            if (st_post) begin
                wb_adr[wb_wp] <= d_radr;
                wb_dat[wb_wp] <= d_rwdat;
                wb_sel[wb_wp] <= d_rsel;
                wb_wp <= wb_wp + 1'b1;
            end
            if (dr_busy && m_ack_i) wb_rp <= wb_rp + 1'b1;
            case ({ st_post, dr_busy && m_ack_i })
                2'b10: wb_cnt <= wb_cnt + 1'b1;
                2'b01: wb_cnt <= wb_cnt - 1'b1;
                default: ;
            endcase

            if (st_hits_fill) fill_poison <= 1'b1;

            // -- drain: whenever the port is not owned by a fill/bypass
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
                m_instr_o <= 1'b0;
                dr_busy <= 1'b1;
            end

            // -- the memory port
            case (mstate)

                M_FLUSH: begin
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
                        mstate <= M_IDLE;
                    end else begin
                        flush_idx <= flush_idx + 1'b1;
                    end
                end

                M_IDLE: begin
                    if ((flush_pending_i || flush_pending_d) &&
                        !i_pend && !d_pend && !snoop_svc) begin
                        flush_i <= flush_pending_i;
                        flush_d <= flush_pending_d;
                        flush_pending_i <= cfg_i_flush_req;
                        flush_pending_d <= cfg_d_flush_req;
                        flush_idx <= 0;
                        mstate <= M_FLUSH;
                    end else if (bus_free && (d_need || i_need)) begin
                        // D first: it is the older instruction. The
                        // request leaves WAIT as the port takes it:
                        // otherwise it would still be waiting in the
                        // cycle of its own ack, and be started again.
                        if (d_need) d_wait <= 1'b0;
                        else i_wait <= 1'b0;
                        m_for_i <= !d_need;
                        m_instr_o <= !d_need;
                        fill_poison <= !d_need && st_hits_iline;
                        fill_cnt <= 0;
                        m_stb_o <= 1'b1;
                        m_cyc_o <= 1'b1;
                        if (d_need) begin
                            if (d_kind == K_BYP) begin
                                m_adr_o <= d_radr;
                                m_dat_o <= d_rwdat;
                                m_sel_o <= d_rsel;
                                m_we_o <= d_rwe;
                                m_cti_o <= 3'b000;
                                mstate <= M_BYPASS;
                            end else begin
                                m_adr_o <= { d_radr[31:D_IDXLSB], {D_WOFFB{1'b0}}, 2'b00 };
                                m_dat_o <= 32'b0;
                                m_sel_o <= 4'b0000;
                                m_we_o <= 1'b0;
                                m_cti_o <= D_BURST ? 3'b010 : 3'b000;
                                mstate <= M_FILL;
                            end
                        end else begin
                            m_dat_o <= 32'b0;
                            m_we_o <= 1'b0;
                            if (i_kind == K_BYP) begin
                                m_adr_o <= i_radr;
                                m_sel_o <= 4'b1111;
                                m_cti_o <= 3'b000;
                                mstate <= M_BYPASS;
                            end else begin
                                m_adr_o <= { i_radr[31:I_IDXLSB], {I_WOFFB{1'b0}}, 2'b00 };
                                m_sel_o <= 4'b0000;
                                m_cti_o <= I_BURST ? 3'b010 : 3'b000;
                                mstate <= M_FILL;
                            end
                        end
                    end
                end

                M_FILL: begin
                    if (m_ack_i) begin
                        if (m_for_i ? (fill_cnt == i_rwoff) :
                                      (fill_cnt == d_rwoff))
                            fill_want_dat <= m_dat_i;
                        if (fill_last) begin
                            m_stb_o <= 1'b0;
                            m_cti_o <= 3'b000;
                            mstate <= M_FILL_END;
                        end else if (m_for_i ? I_BURST : D_BURST) begin
                            fill_cnt <= fill_cnt + 1'b1;
                            m_adr_o <= m_adr_o + 32'd4;
                            m_cti_o <= fill_cti_next;
                        end else begin
                            m_stb_o <= 1'b0;
                            fill_cnt <= fill_cnt + 1'b1;
                            mstate <= M_FILL_SEQ;
                        end
                    end
                end

                M_FILL_SEQ: begin
                    m_adr_o <= m_for_i ?
                        { i_radr[31:I_IDXLSB], fill_cnt[I_WOFFB-1:0], 2'b00 } :
                        { d_radr[31:D_IDXLSB], fill_cnt[D_WOFFB-1:0], 2'b00 };
                    m_stb_o <= 1'b1;
                    mstate <= M_FILL;
                end

                // The tag is written here (always @* above), valid unless
                // poisoned. An I fill waits a cycle if a store is
                // invalidating an I line this cycle: one I tag write
                // port, and the store may be poisoning this very fill.
                M_FILL_END: begin
                    if (fill_end_go) begin
                        m_cyc_o <= 1'b0;
                        m_instr_o <= 1'b0;
                        if (m_for_i) begin
                            i_dat_r <= fill_want_dat;
                            i_ack_r <= 1'b1;
                        end else begin
                            d_dat_r <= fill_want_dat;
                            d_ack_r <= 1'b1;
                        end
                        mstate <= M_IDLE;
                    end
                end

                M_BYPASS: begin
                    if (m_ack_i) begin
                        m_stb_o <= 1'b0;
                        m_cyc_o <= 1'b0;
                        m_we_o <= 1'b0;
                        m_instr_o <= 1'b0;
                        if (m_for_i) begin
                            i_dat_r <= m_dat_i;
                            i_ack_r <= 1'b1;
                        end else begin
                            d_dat_r <= m_dat_i;
                            d_ack_r <= 1'b1;
                        end
                        mstate <= M_IDLE;
                    end
                end

                default: mstate <= M_IDLE;

            endcase
        end
    end

endmodule
