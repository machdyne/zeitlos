/*
 * Behavioural SDR SDRAM model -- W9825G6KH / MT48LC16M16A2 class (x16)
 *
 * 4 banks x 8192 rows x 512 columns x 16 bits. Commands are sampled on
 * the rising edge of `clk`, which is the SDRAM's own CK pin (in this
 * SOC that is ~fabric clock, see rtl/mem/sdram_kianv.v).
 *
 * What it models:
 *   - MRS: burst length 1/2/4/8, sequential wrap within the burst,
 *     CAS latency 2/3, and burst-write vs single-write mode (A9)
 *   - READ bursts delivered CAS_LATENCY edges after the command,
 *     driven TAC ns after the previous edge and held until TOH ns
 *     after the sampling edge -- the controller's negedge capture
 *     (READ_NEGEDGE=1) sees exactly what a real part would give it
 *   - a READ or WRITE issued during a read burst truncates it (this
 *     is what makes back-to-back BL2 READs seamless), BST terminates
 *   - WRITE bursts with DQM byte masking per beat
 *   - auto-precharge (A10) on READ/WRITE, PRE one bank / all
 *
 * What it checks (each is an error, counted in `errors`):
 *   - any command before the mode register is set, except the
 *     init sequence (NOP, PRE, REF, MRS)
 *   - ACT on a bank whose row is already open
 *   - READ/WRITE on a bank with no open row
 *   - REF while any bank is open
 *   - MRS while any bank is open
 *
 * Not modelled: timing minimums (tRCD, tRP, tRAS, tWR, tRFC, tREFI).
 * The controller derives those from parameters; a violation shows up
 * as data corruption on hardware, which is a different kind of test.
 *
 * Backing store is sparse: only the low MEM_WORDS 16-bit words of the
 * flattened {bank,row,col} space exist; anything above reads 16'hDEAD.
 *
 * This replaces an earlier, unfinished model (it returned zeros for
 * every read). tb_sdram_burst.v and tb_cache_sdram.v drive it.
 */

`timescale 1ns / 1ps

module sdram_model #(
    parameter MEM_WORDS = 65536,     // 16-bit words backed
    parameter TAC = 5,               // clock-to-data-valid, ns
    parameter TOH = 2                // data hold after next edge, ns
) (
    input wire clk,
    input wire cke,
    input wire csn,
    input wire rasn,
    input wire casn,
    input wire wen,
    input wire [1:0] dqm,
    input wire [1:0] ba,
    input wire [12:0] addr,
    inout wire [15:0] dq
);

    localparam CMD_MRS     = 4'b0000;
    localparam CMD_REFRESH = 4'b0001;
    localparam CMD_PRE     = 4'b0010;
    localparam CMD_ACT     = 4'b0011;
    localparam CMD_WRITE   = 4'b0100;
    localparam CMD_READ    = 4'b0101;
    localparam CMD_BST     = 4'b0110;
    localparam CMD_NOP     = 4'b0111;

    wire [3:0] cmd = csn ? CMD_NOP : { 1'b0, rasn, casn, wen };

    reg [15:0] mem [0:MEM_WORDS-1];
    reg [12:0] open_row [0:3];
    reg        row_open [0:3];

    reg        mode_set;
    reg [3:0]  bl;                   // 1, 2, 4 or 8
    reg [2:0]  cl;
    reg        single_write;

    // read burst in progress (as issued; data emerges cl edges later)
    reg        rd_act;
    reg [1:0]  rd_bank;
    reg [12:0] rd_row;
    reg [8:0]  rd_col0;
    reg [3:0]  rd_beat;
    reg        rd_ap;

    // write burst in progress
    reg        wr_act;
    reg [1:0]  wr_bank;
    reg [12:0] wr_row;
    reg [8:0]  wr_col0;
    reg [3:0]  wr_beat;
    reg        wr_ap;

    // output pipeline, indexed by edges-until-sampled
    reg [15:0] pipe_d [0:7];
    reg        pipe_v [0:7];
    reg [2:0]  dqm_d1;               // read DQM has 2-edge latency
    reg [1:0]  rdqm_0;
    reg [1:0]  rdqm_1;

    reg [15:0] dq_out;
    reg        dq_oe;
    assign dq = dq_oe ? dq_out : 16'hzzzz;

    integer errors;
    integer reads;
    integer writes;
    integer i;
    reg [31:0] fa;
    reg [8:0] col;
    reg [15:0] w;

    function [31:0] flat;
        input [1:0] b;
        input [12:0] r;
        input [8:0] c;
    begin
        flat = { b, r, c };
    end
    endfunction

    // sequential wrap inside a BL-aligned block
    function [8:0] burst_col;
        input [8:0] c0;
        input [3:0] beat;
        input [3:0] len;
    begin
        case (len)
            4'd2: burst_col = { c0[8:1], c0[0]   + beat[0]   };
            4'd4: burst_col = { c0[8:2], c0[1:0] + beat[1:0] };
            4'd8: burst_col = { c0[8:3], c0[2:0] + beat[2:0] };
            default: burst_col = c0;
        endcase
    end
    endfunction

    task rd_word;
        input [31:0] a;
        output [15:0] v;
    begin
        if (a < MEM_WORDS) v = mem[a]; else v = 16'hDEAD;
    end
    endtask

    // Seed one 16-bit word of the flattened array (kept for
    // tb_cache_sdram.v; newer testbenches write mem[] directly).
    task preload;
        input [31:0] word_addr;
        input [15:0] value;
    begin
        if (word_addr < MEM_WORDS) mem[word_addr] = value;
    end
    endtask

    initial begin
        errors = 0; reads = 0; writes = 0;
        mode_set = 0; bl = 1; cl = 2; single_write = 0;
        rd_act = 0; wr_act = 0; wr_ap = 0; dq_oe = 0; dq_out = 0;
        rdqm_0 = 2'b11; rdqm_1 = 2'b11;
        for (i = 0; i < 4; i = i + 1) begin row_open[i] = 0; open_row[i] = 0; end
        for (i = 0; i < 8; i = i + 1) begin pipe_v[i] = 0; pipe_d[i] = 0; end
        for (i = 0; i < MEM_WORDS; i = i + 1) mem[i] = 16'h0000;
    end

    always @(posedge clk) begin
        if (cke) begin

            // -- shift the output pipeline one edge ---------------
            for (i = 0; i < 7; i = i + 1) begin
                pipe_d[i] = pipe_d[i+1];
                pipe_v[i] = pipe_v[i+1];
            end
            pipe_v[7] = 1'b0;

            // -- continue an in-flight write burst ----------------
            if (wr_act && cmd == CMD_NOP) begin
                col = burst_col(wr_col0, wr_beat, bl);
                fa = flat(wr_bank, wr_row, col);
                if (fa < MEM_WORDS) begin
                    w = mem[fa];
                    if (!dqm[0]) w[7:0]  = dq[7:0];
                    if (!dqm[1]) w[15:8] = dq[15:8];
                    mem[fa] = w;
                end
                wr_beat = wr_beat + 1;
                if (wr_beat >= bl) begin
                    wr_act = 1'b0;
                    if (wr_ap) row_open[wr_bank] = 1'b0;
                end
            end else if (wr_act) begin
                wr_act = 1'b0;          // any command ends a write burst
            end

            // -- continue an in-flight read burst -----------------
            if (rd_act && (cmd == CMD_NOP || cmd == CMD_ACT || cmd == CMD_PRE ||
                           cmd == CMD_REFRESH)) begin
                col = burst_col(rd_col0, rd_beat, bl);
                rd_word(flat(rd_bank, rd_row, col), w);
                pipe_d[cl-1] = w;
                pipe_v[cl-1] = 1'b1;
                rd_beat = rd_beat + 1;
                if (rd_beat >= bl) begin
                    rd_act = 1'b0;
                    if (rd_ap) row_open[rd_bank] = 1'b0;
                end
            end

            case (cmd)
                CMD_MRS: begin
                    for (i = 0; i < 4; i = i + 1)
                        if (row_open[i]) begin
                            $display("SDRAM ERROR @%0t: MRS with bank %0d open", $time, i);
                            errors = errors + 1;
                        end
                    case (addr[2:0])
                        3'd0: bl = 1; 3'd1: bl = 2; 3'd2: bl = 4; 3'd3: bl = 8;
                        default: begin
                            $display("SDRAM ERROR @%0t: unsupported BL code %0d", $time, addr[2:0]);
                            errors = errors + 1;
                        end
                    endcase
                    cl = addr[6:4];
                    if (cl != 2 && cl != 3) begin
                        $display("SDRAM ERROR @%0t: unsupported CAS latency %0d", $time, cl);
                        errors = errors + 1;
                    end
                    single_write = addr[9];
                    mode_set = 1'b1;
                end
                CMD_REFRESH: begin
                    for (i = 0; i < 4; i = i + 1)
                        if (row_open[i]) begin
                            $display("SDRAM ERROR @%0t: REFRESH with bank %0d open", $time, i);
                            errors = errors + 1;
                        end
                end
                CMD_PRE: begin
                    if (addr[10]) for (i = 0; i < 4; i = i + 1) row_open[i] = 1'b0;
                    else row_open[ba] = 1'b0;
                end
                CMD_ACT: begin
                    if (!mode_set) begin
                        $display("SDRAM ERROR @%0t: ACT before MRS", $time);
                        errors = errors + 1;
                    end
                    if (row_open[ba]) begin
                        $display("SDRAM ERROR @%0t: ACT bank %0d row %0d, row %0d already open",
                            $time, ba, addr, open_row[ba]);
                        errors = errors + 1;
                    end
                    row_open[ba] = 1'b1;
                    open_row[ba] = addr;
                end
                CMD_READ: begin
                    if (!row_open[ba]) begin
                        $display("SDRAM ERROR @%0t: READ bank %0d with no open row", $time, ba);
                        errors = errors + 1;
                    end
                    reads = reads + 1;
                    rd_act = 1'b1;
                    rd_bank = ba;
                    rd_row = open_row[ba];
                    rd_col0 = addr[8:0];
                    rd_ap = addr[10];
                    rd_beat = 0;
                    col = burst_col(rd_col0, 0, bl);
                    rd_word(flat(rd_bank, rd_row, col), w);
                    pipe_d[cl-1] = w;
                    pipe_v[cl-1] = 1'b1;
                    rd_beat = 1;
                    if (rd_beat >= bl) begin
                        rd_act = 1'b0;
                        if (rd_ap) row_open[ba] = 1'b0;
                    end
                end
                CMD_WRITE: begin
                    if (!row_open[ba]) begin
                        $display("SDRAM ERROR @%0t: WRITE bank %0d with no open row", $time, ba);
                        errors = errors + 1;
                    end
                    writes = writes + 1;
                    // a WRITE truncates a read burst
                    rd_act = 1'b0;
                    for (i = 0; i < 8; i = i + 1) pipe_v[i] = 1'b0;
                    fa = flat(ba, open_row[ba], addr[8:0]);
                    if (fa < MEM_WORDS) begin
                        w = mem[fa];
                        if (!dqm[0]) w[7:0]  = dq[7:0];
                        if (!dqm[1]) w[15:8] = dq[15:8];
                        mem[fa] = w;
                    end
                    wr_bank = ba;
                    wr_row = open_row[ba];
                    wr_col0 = addr[8:0];
                    wr_beat = 1;
                    wr_act = !single_write && (bl > 1);
                    wr_ap = addr[10];
                    if (addr[10] && !wr_act) row_open[ba] = 1'b0;
                end
                CMD_BST: begin
                    rd_act = 1'b0;
                    wr_act = 1'b0;
                end
                default: ;
            endcase

            // -- read DQM, 2-edge latency: masks the beat sampled
            //    two edges after it was presented
            rdqm_1 = rdqm_0;
            rdqm_0 = dqm;
        end

        // -- drive DQ for the beat sampled at the NEXT edge -------
        if (pipe_v[0]) begin
            #(TAC);
            dq_out = (rdqm_1 == 2'b11) ? 16'hzzzz : pipe_d[0];
            dq_oe = 1'b1;
        end else begin
            #(TOH);
            dq_oe = 1'b0;
        end
    end

endmodule
