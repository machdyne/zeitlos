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
 * wb_cache_one -- a unified cache with ONE CPU port, for zeitlos32ps.
 *
 * rtl/cache_split.v's D side on its own: the same port handshake, the
 * same rules, the same register window. A fetch is a cacheable read
 * like a load, so instructions and data share one array (KB, the I
 * and D budgets of the other caches together).
 *
 * The rules (as wb_cache and wb_cache_split):
 *   - Physically tagged; only main memory is cached.
 *   - WRITE-THROUGH, UPDATE-ON-HIT, NO WRITE-ALLOCATE.
 *   - Posted stores are drained before any other bus access.
 *   - snoop_stb_i invalidates by index (a writing master); a second one
 *     before the first is serviced becomes a full flush.
 *
 * What one array changes:
 *   - Code written as data is never fetched stale: the store updates
 *     the very line a fetch reads. There is no I side to snoop, so the
 *     duplicated I tag array and the store-snoop logic go.
 *   - I_CTRL's enable gates fetches, D_CTRL's gates loads, as before.
 *     Either flush bit flushes the one array.
 *   - I_* statistics count fetches, D_* count loads.
 *   - FEAT bit 12 says this is the unified cache (bit 11, split, clear).
 *
 * The port: a request is taken at a clock edge where req && rdy; the
 * answer is ack for one cycle with the data, at the earliest the cycle
 * after (a hit); rdy is high again in the ack cycle. One outstanding.
 */

module wb_cache_one #(
    parameter MAIN_512     = 0,
    parameter KB           = 8,
    parameter LINE_WORDS   = 4,
    parameter WBUF_DEPTH   = 2,
    parameter BURST        = 0,
    parameter SNOOP        = 1,
    parameter [31:0] CFG_BASE = 32'h7000_0100,
    // 1: the eight statistics counters live in a block RAM, updated in
    // turn by one adder from small per-counter pending counts, instead
    // of eight 32-bit counters in logic (about 200 LUT4 fewer). Reading
    // one waits for its next update (2-9 cycles), and returns its value
    // as of that update; clearing works as before. 0: as before.
    parameter STAT_BRAM    = 0
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- the CPU port --
    input         c_req,
    input  [31:0] c_adr,
    input         c_we,
    input   [3:0] c_sel,
    input  [31:0] c_wdat,
    input         c_instr,          // a fetch
    output        c_rdy,
    output        c_ack,
    output [31:0] c_rdat,

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
    output reg m_instr_o,

    // -- write snoop from other masters --
    input snoop_stb_i,
    input [31:0] snoop_adr_i,

    output c_cfg_hit,

    // -- the MPU (rtl/mpu_split.v, as cache_split takes it); tie to 0
    // without one. mpu_iveto is sampled with a fetch when it is taken:
    // blocked, answered 0, never reaches the bus. mpu_reg likewise with
    // a load or store: the MPU's register window, answered the cycle
    // after LOOK with mpu_dat. mpu_veto comes in the cycle AFTER a load
    // or store is taken (the MPU judges its own registered copy): the
    // access has not touched the arrays, the write buffer or the bus
    // yet, and is answered 0 with no other effect.
    input         mpu_iveto,
    input         mpu_veto,
    input         mpu_reg,
    input  [31:0] mpu_dat
);

    // -- geometry ----------------------------------------------------

    localparam WORDS   = (KB * 1024) / 4;
    localparam LINES   = WORDS / LINE_WORDS;
    localparam IDXB    = $clog2(LINES);
    localparam WOFFB   = $clog2(LINE_WORDS);
    localparam DIDXB   = $clog2(WORDS);
    localparam IDXLSB  = 2 + WOFFB;
    localparam TAGLSB  = IDXLSB + IDXB;
    localparam TAGB    = (MAIN_512 ? 29 : 28) - TAGLSB;

    localparam C_BURST = (BURST != 0) && ((LINE_WORDS % 4) == 0);

    localparam WBD  = (WBUF_DEPTH < 1) ? 1 : WBUF_DEPTH;
    localparam WBS  = (WBD < 2) ? 2 : WBD;
    localparam WBPB = $clog2(WBS);

    // -- storage -------------------------------------------------------

    reg [31:0] data [0:WORDS-1];
`ifdef CACHE_TAG_LUTRAM
    (* ram_style = "distributed" *)
`endif
    reg [TAGB:0] tag [0:LINES-1];

    reg [31:0] data_q;
    reg [TAGB:0] tag_q;

    reg        fw_v;                // last cycle's store-hit word
    reg [DIDXB-1:0] fw_idx;
    reg [31:0] fw_dat;

    reg data_we;
    reg [DIDXB-1:0] data_wa;
    reg [31:0] data_wd;
    reg tag_we;
    reg [IDXB-1:0] tag_wa;
    reg [TAGB:0] tag_wd;

    // -- control registers -------------------------------------------

    reg i_enable;
    reg d_enable;
    reg wb_enable;
    reg cfg_flush_req;
    reg [31:0] cfg_dat;

    reg [31:0] stat_i_hits;
    reg [31:0] stat_i_misses;
    reg [31:0] stat_d_hits;
    reg [31:0] stat_d_misses;
    reg [31:0] stat_loads;
    reg [31:0] stat_stores;
    reg [31:0] stat_stall;
    reg [31:0] stat_wbfull;
    reg stall_pulse;
    reg ev_ihit, ev_imiss, ev_dhit, ev_dmiss, ev_load, ev_store, ev_wbfull;

    // -- the port ------------------------------------------------------

    localparam K_LD  = 3'd1;   // cacheable read (fetch or load)
    localparam K_ST  = 3'd2;   // store to main memory
    localparam K_BYP = 3'd3;   // uncached: straight to the bus
    localparam K_CFG = 3'd4;   // this module's registers
    localparam K_VETO = 3'd5;  // a fetch blocked by the MPU: answered 0
    localparam K_LOC = 3'd6;   // the MPU's registers (answered with mpu_dat)

    reg c_pend, c_look, c_wait;
    reg [2:0] c_kind;
    reg c_rinstr;
    reg [31:0] c_radr;
    reg [31:0] c_rwdat;
    reg [3:0] c_rsel;
    reg c_rwe;
    reg c_ack_r;
    reg [31:0] c_dat_r;
    reg cfg_done;
    reg loc_done;
    reg c_first;               // the cycle after a take

    // -- memory port -----------------------------------------------------

    localparam M_IDLE     = 3'd0;
    localparam M_FILL     = 3'd1;
    localparam M_FILL_SEQ = 3'd2;
    localparam M_FILL_END = 3'd3;
    localparam M_BYPASS   = 3'd4;
    localparam M_FLUSH    = 3'd5;
    (* fsm_encoding = "none" *) reg [2:0] mstate;

    reg [WOFFB-1:0] fill_cnt;
    reg [31:0] fill_want_dat;
    reg fill_poison;
    reg flush_pending;
    reg [IDXB-1:0] flush_idx;
    reg snoop_pending;
    reg [31:0] snoop_adr;

    // -- write buffer ----------------------------------------------------

    reg [31:0] wb_adr [0:WBS-1];
    reg [31:0] wb_dat [0:WBS-1];
    reg [3:0]  wb_sel [0:WBS-1];
    reg [WBPB-1:0] wb_rp;
    reg [WBPB-1:0] wb_wp;
    reg [WBPB:0] wb_cnt;
    reg dr_busy;

    // -- request decode --------------------------------------------------

    wire c_main = MAIN_512 ? ((c_adr & 32'he000_0000) == 32'h4000_0000)
                           : ((c_adr & 32'hf000_0000) == 32'h4000_0000);
    wire c_cfg  = ((c_adr & 32'hf000_0700) == CFG_BASE);
    assign c_cfg_hit = c_req && c_cfg;

    wire snoop_svc = snoop_pending && (mstate == M_IDLE) && !c_pend;
    wire arrays_busy = (mstate == M_FLUSH) || snoop_svc ||
                       flush_pending || snoop_pending;

    wire c_take = c_req && c_rdy;
    // The arrays read the request's address whenever there is one, not
    // only when it is taken: c_take depends on this cycle's hit (c_rdy
    // is high in the ack cycle), and putting that in front of the RAM
    // address made hit -> take -> address the 48 MHz critical path.
    // Reading an address that is not taken is harmless: the array
    // outputs are only used in a LOOK cycle, which follows a take,
    // except for a store waiting for write-buffer room (st_hold), which
    // stays in LOOK and so keeps reading its own address.
    wire st_hold;
    wire [31:0] lu_adr = (c_req && !st_hold) ? c_adr : c_radr;

    wire [TAGB-1:0]  rtag  = c_radr[TAGLSB +: TAGB];
    wire [IDXB-1:0]  ridx  = c_radr[IDXLSB +: IDXB];
    wire [WOFFB-1:0] rwoff = c_radr[2 +: WOFFB];

    wire hit = tag_q[TAGB] && (tag_q[TAGB-1:0] == rtag);
    wire fwd = fw_v && (fw_idx == c_radr[2 +: DIDXB]);
    wire [31:0] word = fwd ? fw_dat : data_q;

    wire post_en  = (WBUF_DEPTH > 0) && wb_enable;
    wire wb_full  = (wb_cnt == WBD);
    wire bus_free = (wb_cnt == 0) && !dr_busy;
    wire bus_owned = (mstate == M_FILL) || (mstate == M_FILL_SEQ) ||
                     (mstate == M_FILL_END) || (mstate == M_BYPASS);

    // a load or store the MPU blocks, in the cycle after its take
    wire vetoed = c_pend && c_first && !c_rinstr && mpu_veto;

    wire st_look_nv = c_pend && c_look && (c_kind == K_ST);
    wire st_look = st_look_nv && !vetoed;
    wire st_go   = st_look && (!post_en || !wb_full);
    wire st_post = st_go && post_en;
    wire st_dwr  = st_go && hit;
    assign st_hold = st_look && !st_go;

    wire hit_ack = c_pend && c_look && (c_kind == K_LD) && hit && !vetoed;
    wire cfg_ack = c_pend && (c_kind == K_CFG) && cfg_done;
    wire veto_ack = vetoed || (c_pend && c_look && (c_kind == K_VETO));
    wire loc_ack = c_pend && (c_kind == K_LOC) && loc_done;

    // c_ack, as  hit_ack || st_post || cfg_ack || veto_ack || loc_ack ||
    // c_ack_r, but built so that the two late inputs -- the tag compare
    // (hit) and the MPU's verdict (vetoed) -- each meet it in the last
    // LUT: hit_ack and st_post are only cleared by vetoed when vetoed
    // acknowledges anyway, so they may be taken without it here.
    (* keep *) wire ack_reg = (st_look_nv && (!post_en || !wb_full) && post_en) ||
                              cfg_ack || (c_pend && c_look && (c_kind == K_VETO)) ||
                              loc_ack || c_ack_r;
    (* keep *) wire hit_q   = c_pend && c_look && (c_kind == K_LD);
    assign c_ack = ack_reg || vetoed || (hit_q && hit);
`ifndef SYNTHESIS
    always @(posedge wb_clk_i)
        if (!wb_rst_i && (c_ack !== (hit_ack || st_post || cfg_ack || veto_ack || loc_ack || c_ack_r))) begin
            $display("wb_cache_one: c_ack differs from its reference expression");
            $finish;
        end
`endif

    wire look_ld = c_look && (c_kind == K_LD);
    assign c_rdat = vetoed ? 32'b0 :
                    look_ld ? word :
                    (c_kind == K_CFG) ? cfg_dat :
                    (c_kind == K_LOC) ? mpu_dat :
                    (c_kind == K_VETO) ? 32'b0 : c_dat_r;

    (* keep *) wire rdy_e = !arrays_busy && !c_pend;
    (* keep *) wire rdy_b = !arrays_busy;
    assign c_rdy = rdy_e || (rdy_b && c_ack);	// !arrays_busy && (!c_pend || c_ack)

    wire c_need = c_pend && c_wait && !vetoed;

    wire fill_end_go = (mstate == M_FILL_END);
    wire fill_last = (fill_cnt == (LINE_WORDS-1));
    wire [2:0] fill_cti_next = (fill_cnt == (LINE_WORDS-2)) ? 3'b111 : 3'b010;

    assign m_bte_o = 2'b00;

    // -- synchronous reads ---------------------------------------------

    always @(posedge wb_clk_i) begin
        data_q <= data[lu_adr[2 +: DIDXB]];
        tag_q  <= tag[lu_adr[IDXLSB +: IDXB]];
    end

    always @(posedge wb_clk_i) if (data_we) data[data_wa] <= data_wd;
    always @(posedge wb_clk_i) if (tag_we)  tag[tag_wa]   <= tag_wd;

    wire [31:0] st_merged = {
        c_rsel[3] ? c_rwdat[31:24] : word[31:24],
        c_rsel[2] ? c_rwdat[23:16] : word[23:16],
        c_rsel[1] ? c_rwdat[15:8]  : word[15:8],
        c_rsel[0] ? c_rwdat[7:0]   : word[7:0] };

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            fw_v <= 1'b0;
        end else begin
            fw_v <= st_dwr;
        end
        if (st_dwr) begin
            fw_idx <= c_radr[2 +: DIDXB];
            fw_dat <= st_merged;
        end
    end

    always @* begin
        data_we = 1'b0;
        data_wa = { ridx, fill_cnt };
        data_wd = m_dat_i;
        tag_we  = 1'b0;
        tag_wa  = ridx;
        tag_wd  = { !fill_poison, rtag };
        if (mstate == M_FLUSH) begin
            tag_we = 1'b1;
            tag_wa = flush_idx;
            tag_wd = 0;
        end else if (snoop_svc) begin
            tag_we = 1'b1;
            tag_wa = snoop_adr[IDXLSB +: IDXB];
            tag_wd = 0;
        end else begin
            if (st_dwr) begin
                data_we = 1'b1;
                data_wa = { ridx, rwoff };
                data_wd = st_merged;
            end else if (mstate == M_FILL && m_ack_i) begin
                data_we = 1'b1;
            end
            if (fill_end_go)
                tag_we = 1'b1;
        end
    end

    // -- config registers ------------------------------------------------

    wire cfg_now = c_pend && c_look && (c_kind == K_CFG) && !vetoed;
    wire [3:0] cfg_reg = c_radr[5:2];
    wire cfg_wr = cfg_now && c_rwe;
    wire cfg_rd = cfg_now && !c_rwe;

    // -- statistics in a block RAM (STAT_BRAM) ----------------------------
    //
    // Slots: 0 i hits, 1 i misses, 2 d hits, 3 d misses, 4 loads,
    // 5 stores, 6 stall cycles, 7 write-buffer-full cycles -- the same
    // events (ev_*, stall_pulse) that drive the stat_* counters, which are
    // then left without a reader and removed by synthesis (simulation
    // keeps them, and checks this against them).
    //
    // Each slot has a 4-bit count of events not yet added. A pointer
    // visits the slots in turn, one per cycle: in the visit cycle the
    // RAM reads the slot and its pending count is taken (the cycle's own
    // event starts the next count); the cycle after, RAM + count is
    // written back. A slot is visited every 8 cycles, so a count never
    // exceeds 8. Clearing a slot drops its pending count and marks it,
    // and its next write is the count alone.

    wire [7:0] sb_ev = { ev_wbfull, stall_pulse, ev_store, ev_load,
                         ev_dmiss, ev_dhit, ev_imiss, ev_ihit };
    wire [7:0] sb_clr_now = {
        cfg_wr && (cfg_reg == 4'd12), cfg_wr && (cfg_reg == 4'd10),
        cfg_wr && (cfg_reg == 4'd9),  cfg_wr && (cfg_reg == 4'd8),
        {4{(mstate == M_FLUSH) && (flush_idx == (LINES-1))}} };
    wire       sb_rd_now;       // a read of a statistics register, now
    wire       sb_capture;      // its value is in sb_wdat, now
    wire [31:0] sb_wdat;
    wire [3:0] cfg_reg_w = cfg_reg;
    wire [2:0] sb_rd_slot =
        (cfg_reg_w == 4'd1)  ? 3'd0 : (cfg_reg_w == 4'd2)  ? 3'd1 :
        (cfg_reg_w == 4'd5)  ? 3'd2 : (cfg_reg_w == 4'd6)  ? 3'd3 :
        (cfg_reg_w == 4'd8)  ? 3'd4 : (cfg_reg_w == 4'd9)  ? 3'd5 :
        (cfg_reg_w == 4'd10) ? 3'd6 : 3'd7;
    wire sb_is_stat = (cfg_reg_w == 4'd1) || (cfg_reg_w == 4'd2) ||
        (cfg_reg_w == 4'd5) || (cfg_reg_w == 4'd6) || (cfg_reg_w == 4'd8) ||
        (cfg_reg_w == 4'd9) || (cfg_reg_w == 4'd10) || (cfg_reg_w == 4'd12);

    generate if (STAT_BRAM) begin : g_sb
        (* syn_ramstyle = "block_ram" *)
        reg [31:0] mem [0:7];
        reg [31:0] q;
        reg  [3:0] pend [0:7];
        reg  [7:0] clr;
        reg  [2:0] sp, sp_q;
        reg  [3:0] amt_q;
        reg        zero_q;
        reg        rd_wait, armed;
        reg  [2:0] rd_slot;
        integer j;

        assign sb_wdat    = (zero_q ? 32'd0 : q) + {28'd0, amt_q};
        assign sb_rd_now  = cfg_now && !c_rwe && sb_is_stat;
        assign sb_capture = armed && (sp_q == rd_slot);

        always @(posedge wb_clk_i) begin
            q <= mem[sp];
            mem[sp_q] <= sb_wdat;
        end

        always @(posedge wb_clk_i) begin
            if (wb_rst_i) begin
                sp <= 3'd0; sp_q <= 3'd0; amt_q <= 4'd0; zero_q <= 1'b1;
                clr <= 8'hff;
                rd_wait <= 1'b0; armed <= 1'b0; rd_slot <= 3'd0;
                for (j = 0; j < 8; j = j + 1) pend[j] <= 4'd0;
            end else begin
                sp <= sp + 3'd1;
                sp_q <= sp;
                amt_q <= sb_clr_now[sp] ? 4'd0 : pend[sp];
                zero_q <= clr[sp] || sb_clr_now[sp];
                for (j = 0; j < 8; j = j + 1) begin
                    if (sb_clr_now[j]) begin
                        pend[j] <= 4'd0;
                        clr[j] <= 1'b1;
                    end else if (sp == j) begin
                        pend[j] <= {3'd0, sb_ev[j]};
                        clr[j] <= 1'b0;
                    end else begin
                        pend[j] <= pend[j] + {3'd0, sb_ev[j]};
                    end
                end
                // a read: wait for the slot's next visit, then take the
                // value written back in the cycle after
                if (sb_rd_now) begin
                    rd_wait <= 1'b1;
                    rd_slot <= sb_rd_slot;
                end
                if (rd_wait && !armed && (sp == rd_slot))
                    armed <= 1'b1;
                if (sb_capture) begin
                    armed <= 1'b0;
                    rd_wait <= 1'b0;
                end
            end
        end

`ifndef SYNTHESIS
        // check against the stat_* counters: what a visit writes back is
        // the counter as it was in the visit cycle (unless a clear came
        // in between, which the next visit puts right)
        reg [31:0] chk_exp;
        reg        chk_v;
        reg [31:0] shadow;
        always @* case (sp)
            3'd0: shadow = stat_i_hits;   3'd1: shadow = stat_i_misses;
            3'd2: shadow = stat_d_hits;   3'd3: shadow = stat_d_misses;
            3'd4: shadow = stat_loads;    3'd5: shadow = stat_stores;
            3'd6: shadow = stat_stall;    default: shadow = stat_wbfull;
        endcase
        always @(posedge wb_clk_i) begin
            chk_v <= !wb_rst_i && !sb_clr_now[sp];
            chk_exp <= shadow;
            if (!wb_rst_i && chk_v && !sb_clr_now[sp_q] && (sb_wdat !== chk_exp)) begin
                $display("wb_cache_one: statistics slot %0d: RAM %0d, counter %0d (t=%0t)",
                    sp_q, sb_wdat, chk_exp, $time);
                $finish;
            end
        end
`endif
    end else begin : g_nosb
        assign sb_wdat = 32'd0;
        assign sb_rd_now = 1'b0;
        assign sb_capture = 1'b0;
    end endgenerate


    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            cfg_dat <= 32'b0;
            cfg_flush_req <= 1'b0;
            i_enable <= 1'b1;
            d_enable <= 1'b0;
            wb_enable <= 1'b0;
        end else begin
            cfg_flush_req <= 1'b0;
            if (sb_capture) cfg_dat <= sb_wdat;
            if (cfg_wr) begin
                case (cfg_reg)
                    4'd0: begin
                        i_enable <= c_rwdat[0];
                        cfg_flush_req <= c_rwdat[1];
                    end
                    4'd4: begin
                        d_enable <= c_rwdat[0];
                        wb_enable <= c_rwdat[2];
                        cfg_flush_req <= c_rwdat[1] ||
                            (c_rwdat[0] && !d_enable);
                    end
                    default: ;
                endcase
            end
            if (cfg_rd) begin
                case (cfg_reg)
                    4'd0:  cfg_dat <= { 31'b0, i_enable };
                    4'd1:  cfg_dat <= STAT_BRAM ? 32'b0 : stat_i_hits;
                    4'd2:  cfg_dat <= STAT_BRAM ? 32'b0 : stat_i_misses;
                    4'd3:  cfg_dat <= { 16'h1CAC, LINE_WORDS[7:0], KB[7:0] };
                    4'd4:  cfg_dat <= { 29'b0, wb_enable, 1'b0, d_enable };
                    4'd5:  cfg_dat <= STAT_BRAM ? 32'b0 : stat_d_hits;
                    4'd6:  cfg_dat <= STAT_BRAM ? 32'b0 : stat_d_misses;
                    4'd7:  cfg_dat <= { 16'h1DCA, LINE_WORDS[7:0], KB[7:0] };
                    4'd8:  cfg_dat <= STAT_BRAM ? 32'b0 : stat_loads;
                    4'd9:  cfg_dat <= STAT_BRAM ? 32'b0 : stat_stores;
                    4'd10: cfg_dat <= STAT_BRAM ? 32'b0 : stat_stall;
                    4'd11: cfg_dat <= { 16'h0, 3'b0, 1'b1, 1'b0, (SNOOP != 0),
                               (BURST != 0), 1'b1, WBUF_DEPTH[7:0] };
                    4'd12: cfg_dat <= STAT_BRAM ? 32'b0 : stat_wbfull;
                    default: cfg_dat <= 32'b0;
                endcase
            end
        end
    end

    // -- everything sequential else --------------------------------------

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            mstate <= M_FLUSH;          // block RAM is undefined at reset
            flush_idx <= 0;
            flush_pending <= 1'b0;
            snoop_pending <= 1'b0;
            snoop_adr <= 32'b0;
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
            c_pend <= 1'b0; c_look <= 1'b0; c_wait <= 1'b0;
            c_kind <= K_BYP; c_radr <= 32'b0; c_rinstr <= 1'b0;
            c_rwdat <= 32'b0; c_rsel <= 4'b0; c_rwe <= 1'b0;
            c_ack_r <= 1'b0; c_dat_r <= 32'b0;
            cfg_done <= 1'b0;
            loc_done <= 1'b0;
            c_first <= 1'b0;
            stat_i_hits <= 32'b0;
            stat_i_misses <= 32'b0;
            stat_d_hits <= 32'b0;
            stat_d_misses <= 32'b0;
            stat_loads <= 32'b0;
            stat_stores <= 32'b0;
            stat_stall <= 32'b0;
            stat_wbfull <= 32'b0;
            stall_pulse <= 1'b0;
            ev_ihit <= 1'b0; ev_imiss <= 1'b0; ev_dhit <= 1'b0; ev_dmiss <= 1'b0;
            ev_load <= 1'b0; ev_store <= 1'b0; ev_wbfull <= 1'b0;
        end else begin
            c_ack_r <= 1'b0;
            cfg_done <= (cfg_now && !sb_rd_now) || sb_capture;
            loc_done <= c_pend && c_look && (c_kind == K_LOC);
            c_first <= c_take;

            // -- flush and snoop requests: latched, acted on from idle
            if (cfg_flush_req) flush_pending <= 1'b1;
            if (SNOOP != 0 && snoop_stb_i) begin
                if (snoop_pending)
                    flush_pending <= 1'b1;
                else begin
                    snoop_pending <= 1'b1;
                    snoop_adr <= snoop_adr_i;
                end
                if (mstate != M_IDLE && mstate != M_FLUSH)
                    fill_poison <= 1'b1;
            end
            if (snoop_svc) snoop_pending <= 1'b0;

            // -- statistics
            if (cfg_wr && cfg_reg == 4'd8)  stat_loads <= 32'b0;
            if (cfg_wr && cfg_reg == 4'd9)  stat_stores <= 32'b0;
            if (cfg_wr && cfg_reg == 4'd12) stat_wbfull <= 32'b0;
            stall_pulse <= c_pend && !c_ack;
            if (cfg_wr && cfg_reg == 4'd10) stat_stall <= 32'b0;
            else if (stall_pulse) stat_stall <= stat_stall + 1;
            ev_ihit   <= hit_ack && c_rinstr;
            ev_imiss  <= c_pend && c_look && (c_kind == K_LD) && !c_ack && c_rinstr;
            ev_dhit   <= hit_ack && !c_rinstr;
            ev_dmiss  <= c_pend && c_look && (c_kind == K_LD) && !c_ack && !c_rinstr;
            ev_load   <= c_take && !c_cfg && !c_we && !c_instr && c_main && !mpu_reg;
            ev_store  <= c_take && !c_cfg && c_we && c_main && !mpu_reg;
            ev_wbfull <= st_look && !st_go;
            if (ev_ihit)  stat_i_hits   <= stat_i_hits + 1;
            if (ev_imiss) stat_i_misses <= stat_i_misses + 1;
            if (ev_dhit)  stat_d_hits   <= stat_d_hits + 1;
            if (ev_dmiss) stat_d_misses <= stat_d_misses + 1;
            if (ev_load && !(cfg_wr && cfg_reg == 4'd8))  stat_loads  <= stat_loads + 1;
            if (ev_store && !(cfg_wr && cfg_reg == 4'd9)) stat_stores <= stat_stores + 1;
            if (ev_wbfull && !(cfg_wr && cfg_reg == 4'd12)) stat_wbfull <= stat_wbfull + 1;

            // -- the port
            if (c_take) begin
                c_pend <= 1'b1;
                c_radr <= c_adr;
                c_rwdat <= c_wdat;
                c_rsel <= c_sel;
                c_rwe <= c_we;
                c_rinstr <= c_instr;
                if (c_instr && mpu_iveto) begin
                    c_kind <= K_VETO;
                    c_look <= 1'b1;
                    c_wait <= 1'b0;
                end else if (!c_instr && mpu_reg) begin
                    c_kind <= K_LOC;
                    c_look <= 1'b1;
                    c_wait <= 1'b0;
                end else if (c_cfg) begin
                    c_kind <= K_CFG;
                    c_look <= 1'b1;
                    c_wait <= 1'b0;
                end else if (c_main && c_we) begin
                    c_kind <= K_ST;
                    c_look <= 1'b1;
                    c_wait <= 1'b0;
                end else if (c_main && (c_instr ? i_enable : d_enable)) begin
                    c_kind <= K_LD;
                    c_look <= 1'b1;
                    c_wait <= 1'b0;
                end else begin
                    c_kind <= K_BYP;
                    c_look <= 1'b0;
                    c_wait <= 1'b1;
                end
            end else if (c_ack) begin
                c_pend <= 1'b0;
                c_look <= 1'b0;
                c_wait <= 1'b0;
            end else if (c_pend && c_look) begin
                if (c_kind == K_CFG || c_kind == K_LOC) begin
                    c_look <= 1'b0;
                end else if (c_kind == K_LD) begin
                    c_look <= 1'b0;
                    c_wait <= 1'b1;
                end else if (c_kind == K_ST) begin
                    if (st_go) begin
                        // not posted: write it through as a bypass access
                        c_kind <= K_BYP;
                        c_look <= 1'b0;
                        c_wait <= 1'b1;
                    end
                end
            end

            // -- write buffer: push from a posted store, pop on drain
            if (st_post) begin
                wb_adr[wb_wp] <= c_radr;
                wb_dat[wb_wp] <= c_rwdat;
                wb_sel[wb_wp] <= c_rsel;
                wb_wp <= wb_wp + 1'b1;
            end
            if (dr_busy && m_ack_i) wb_rp <= wb_rp + 1'b1;
            case ({ st_post, dr_busy && m_ack_i })
                2'b10: wb_cnt <= wb_cnt + 1'b1;
                2'b01: wb_cnt <= wb_cnt - 1'b1;
                default: ;
            endcase

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
                    if (flush_idx == (LINES-1)) begin
                        flush_idx <= 0;
                        stat_i_hits <= 32'b0;
                        stat_i_misses <= 32'b0;
                        stat_d_hits <= 32'b0;
                        stat_d_misses <= 32'b0;
                        mstate <= M_IDLE;
                    end else begin
                        flush_idx <= flush_idx + 1'b1;
                    end
                end
                M_IDLE: begin
                    if (flush_pending && !c_pend && !snoop_svc) begin
                        flush_pending <= cfg_flush_req;
                        flush_idx <= 0;
                        mstate <= M_FLUSH;
                    end else if (bus_free && c_need) begin
                        c_wait <= 1'b0;
                        m_instr_o <= c_rinstr;
                        fill_poison <= 1'b0;
                        fill_cnt <= 0;
                        m_stb_o <= 1'b1;
                        m_cyc_o <= 1'b1;
                        if (c_kind == K_BYP) begin
                            m_adr_o <= c_radr;
                            m_dat_o <= c_rwdat;
                            m_sel_o <= c_rsel;
                            m_we_o <= c_rwe;
                            m_cti_o <= 3'b000;
                            mstate <= M_BYPASS;
                        end else begin
                            m_adr_o <= { c_radr[31:IDXLSB], {WOFFB{1'b0}}, 2'b00 };
                            m_dat_o <= 32'b0;
                            m_sel_o <= 4'b0000;
                            m_we_o <= 1'b0;
                            m_cti_o <= C_BURST ? 3'b010 : 3'b000;
                            mstate <= M_FILL;
                        end
                    end
                end
                M_FILL: begin
                    if (m_ack_i) begin
                        if (fill_cnt == rwoff)
                            fill_want_dat <= m_dat_i;
                        if (fill_last) begin
                            m_stb_o <= 1'b0;
                            m_cti_o <= 3'b000;
                            mstate <= M_FILL_END;
                        end else if (C_BURST) begin
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
                    m_adr_o <= { c_radr[31:IDXLSB], fill_cnt, 2'b00 };
                    m_stb_o <= 1'b1;
                    mstate <= M_FILL;
                end
                M_FILL_END: begin
                    m_cyc_o <= 1'b0;
                    m_instr_o <= 1'b0;
                    c_dat_r <= fill_want_dat;
                    c_ack_r <= 1'b1;
                    mstate <= M_IDLE;
                end
                M_BYPASS: begin
                    if (m_ack_i) begin
                        m_stb_o <= 1'b0;
                        m_cyc_o <= 1'b0;
                        m_we_o <= 1'b0;
                        m_instr_o <= 1'b0;
                        c_dat_r <= m_dat_i;
                        c_ack_r <= 1'b1;
                        mstate <= M_IDLE;
                    end
                end
                default: mstate <= M_IDLE;
            endcase
        end
    end

endmodule
