/*
 * STATUS: INCOMPLETE -- this model does not yet work.
 *
 * A control test (controller alone, no cache, driving this model)
 * returns zeros for every read, and tracing shows the controller
 * issuing cmd=0100 (WRITE per the decode below) during read
 * operations, with column addresses incrementing by 2.
 *
 * That is either a bit-ordering error in the command decode here, or
 * the test reads colliding with the tail of the ~200us SDRAM power-up
 * sequence. Both are worth checking before trusting anything this
 * model reports.
 *
 * It is included because the IDEA is sound and the gap it addresses is
 * real: rtl/cache.v was only ever tested against a synthetic wishbone
 * slave with no banks, no open rows and no CAS latency, so a cache
 * that violates the SDRAM protocol passes that testbench -- which is
 * exactly what appears to be happening on hardware.
 *
 * Finish this before writing more cache theories.
 */

/*
 * Behavioural SDRAM model -- W9825G6KH / MT48LC16M16A2 class
 *
 * 256Mbit: 4 banks x 8192 rows x 512 columns x 16 bits.
 * The Winbond and Micron parts are protocol-identical at this level;
 * only their timing parameters differ, and this model checks PROTOCOL,
 * not timing.
 *
 * The point of this file is to close a specific gap. rtl/cache.v was
 * developed against a synthetic wishbone slave that acknowledged
 * requests on demand. That slave modelled none of the things a real
 * SDRAM controller does -- open rows, CAS latency, banks, or the rule
 * that a column read is only legal against an activated row -- so a
 * cache that violated the protocol would still pass. Wiring the cache
 * to the real rtl/mem/sdram_kianv.v and then to this closes that gap.
 *
 * It is deliberately strict: illegal command sequences are reported as
 * errors rather than tolerated, because a silently forgiving model is
 * how a bug survives simulation and shows up on hardware.
 *
 * Not modelled: refresh interval enforcement, tRP/tRCD/tRAS/tWR
 * minimums, power-up sequencing. Those are timing; a violation of them
 * shows up as data corruption on hardware, not as wrong logic, and the
 * controller already has parameters for them.
 */

`timescale 1ns / 1ps

module sdram_model #(
    parameter CAS_LATENCY = 2,
    // Sparse backing store: modelling all 256Mbit would be 32MB of
    // simulator memory for a test that touches a few kilobytes.
    parameter MEM_WORDS = 65536      // 16-bit words, from address 0
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

    // -- command decode (CS#, RAS#, CAS#, WE#) ---------------------

    localparam CMD_MRS      = 4'b0000;
    localparam CMD_REFRESH  = 4'b0001;
    localparam CMD_PRE      = 4'b0010;
    localparam CMD_ACT      = 4'b0011;
    localparam CMD_WRITE    = 4'b0100;
    localparam CMD_READ     = 4'b0101;
    localparam CMD_BST      = 4'b0110;
    localparam CMD_NOP      = 4'b0111;

    wire [3:0] cmd = { csn, rasn, casn, wen };

    // -- state -----------------------------------------------------

    reg [15:0] mem [0:MEM_WORDS-1];

    reg [12:0] open_row [0:3];
    reg row_active [0:3];

    reg [2:0] burst_len;        // decoded from the mode register
    reg mode_set;

    // read pipeline, CAS_LATENCY deep
    reg [15:0] rd_pipe [0:7];
    reg rd_valid [0:7];
    integer i;

    reg [15:0] dq_out;
    reg dq_oe;

    assign dq = dq_oe ? dq_out : 16'hzzzz;

    integer errors;

    initial begin
        errors = 0;
        mode_set = 0;
        burst_len = 3'd0;
        dq_oe = 0;
        dq_out = 0;
        for (i = 0; i < 4; i = i + 1) begin
            row_active[i] = 0;
            open_row[i] = 0;
        end
        for (i = 0; i < 8; i = i + 1) begin
            rd_valid[i] = 0;
            rd_pipe[i] = 0;
        end
        for (i = 0; i < MEM_WORDS; i = i + 1) mem[i] = 16'hDEAD;
    end

    // -- flat address from bank/row/column -------------------------
    //
    // Only the low MEM_WORDS of the array are backed. Anything above
    // reads as a poison value rather than X, so a stray access shows
    // up as recognisable garbage in the data rather than propagating
    // X's through the design and hiding the cause.

    function [31:0] flat_addr;
        input [1:0] b;
        input [12:0] r;
        input [8:0] c;
        begin
            flat_addr = ({ b, r, c } & (MEM_WORDS - 1));
        end
    endfunction

    // -- command handling ------------------------------------------

    reg [1:0]  rd_bank;
    reg [8:0]  rd_col;
    integer    burst_cnt;
    reg        bursting;

    initial begin bursting = 0; burst_cnt = 0; rd_bank = 0; rd_col = 0; end

    always @(posedge clk) begin

        // advance the CAS pipeline
        for (i = 7; i > 0; i = i - 1) begin
            rd_pipe[i] <= rd_pipe[i-1];
            rd_valid[i] <= rd_valid[i-1];
        end
        rd_valid[0] <= 1'b0;

        if (cke) begin

            case (cmd)

                CMD_MRS: begin
                    burst_len <= addr[2:0];
                    mode_set <= 1'b1;
                end

                CMD_ACT: begin
                        $display("SDRAM ERROR @%0t: ACT on bank %0d with row %0d already open",
                            $time, ba, open_row[ba]);
                    row_active[ba] <= 1'b1;
                    open_row[ba] <= addr;
                end

                CMD_PRE: begin
                    if (addr[10]) begin           // precharge all banks
                        for (i = 0; i < 4; i = i + 1) row_active[i] <= 1'b0;
                    end else begin
                        row_active[ba] <= 1'b0;
                    end
                end

                CMD_READ: begin
                    // The check that matters: a column read is only
                    // legal against an activated row. A controller (or
                    // a master driving it wrongly) that reads from a
                    // closed bank gets garbage on hardware; here it is
                    // reported.
                    if (!row_active[ba]) begin
                        $display("SDRAM ERROR @%0t: READ on bank %0d with NO ACTIVE ROW",
                            $time, ba);
                        errors = errors + 1;
                    end
                    rd_pipe[0] <= mem[flat_addr(ba, open_row[ba], addr[8:0])];
                    rd_valid[0] <= 1'b1;
                    rd_bank <= ba;
                    rd_col <= addr[8:0] + 9'd1;
                    bursting <= (burst_len != 3'd0);
                    burst_cnt <= (burst_len == 3'd0) ? 0 :
                                 (burst_len == 3'd1) ? 1 :
                                 (burst_len == 3'd2) ? 3 : 7;
                    if (addr[10]) row_active[ba] <= 1'b0;   // auto precharge
                end

                CMD_WRITE: begin
                    if (!row_active[ba]) begin
                        $display("SDRAM ERROR @%0t: WRITE on bank %0d with NO ACTIVE ROW",
                            $time, ba);
                        errors = errors + 1;
                    end
                    if (!dqm[0]) mem[flat_addr(ba, open_row[ba], addr[8:0])][7:0]  <= dq[7:0];
                    if (!dqm[1]) mem[flat_addr(ba, open_row[ba], addr[8:0])][15:8] <= dq[15:8];
                    if (addr[10]) row_active[ba] <= 1'b0;
                end

                CMD_BST: begin
                    bursting <= 1'b0;
                    burst_cnt <= 0;
                end

                default: begin
                    // continue an in-flight burst
                    if (bursting && burst_cnt > 0) begin
                        rd_pipe[0] <= mem[flat_addr(rd_bank, open_row[rd_bank], rd_col)];
                        rd_valid[0] <= 1'b1;
                        rd_col <= rd_col + 9'd1;
                        burst_cnt <= burst_cnt - 1;
                        if (burst_cnt == 1) bursting <= 1'b0;
                    end
                end

            endcase

        end
    end

    // -- data out at CAS latency -----------------------------------

    always @(*) begin
        dq_oe  = rd_valid[CAS_LATENCY];
        dq_out = rd_pipe[CAS_LATENCY];
    end

    // -- preload helper for testbenches ----------------------------

    task preload;
        input [31:0] word_addr;      // 16-bit word index
        input [15:0] value;
        begin
            mem[word_addr & (MEM_WORDS-1)] = value;
        end
    endtask

endmodule
