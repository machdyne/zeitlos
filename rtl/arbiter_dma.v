/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Two-way Wishbone round robin, for `DMA: rtl/dma.v shares the main
 * arbiter's port 2 with whatever already has it (the audio mixer, or
 * rtl/arbiter_gfx.v). Same policy as rtl/arbiter_main.v: the grant is
 * held for a whole cyc, and whoever was granted last waits if both
 * ask. Both masters here drop cyc after every word (the mixer's
 * fetches, dma.v's transfers), so neither waits more than one of the
 * other's cycles.
 *
 * grant_b says the cycle in progress is B's -- sysctl.v needs it for
 * the burst type, which only the scanout fetch (behind A) ever sets.
 */

module wb_arbiter2 (
    input  wire        clk,
    input  wire        rst,

    input  wire [31:0] a_adr_i,
    input  wire [31:0] a_dat_i,
    output wire [31:0] a_dat_o,
    input  wire        a_we_i,
    input  wire [3:0]  a_sel_i,
    input  wire        a_stb_i,
    input  wire        a_cyc_i,
    output wire        a_ack_o,

    input  wire [31:0] b_adr_i,
    input  wire [31:0] b_dat_i,
    output wire [31:0] b_dat_o,
    input  wire        b_we_i,
    input  wire [3:0]  b_sel_i,
    input  wire        b_stb_i,
    input  wire        b_cyc_i,
    output wire        b_ack_o,

    output wire [31:0] s_adr_o,
    output wire [31:0] s_dat_o,
    input  wire [31:0] s_dat_i,
    output wire        s_we_o,
    output wire [3:0]  s_sel_o,
    output wire        s_stb_o,
    output wire        s_cyc_o,
    input  wire        s_ack_i,

    output wire        grant_b
);

    reg busy;           // a grant is held
    reg owner;          // 0 A, 1 B: the holder, or the last one
    wire a_req = a_cyc_i && a_stb_i;
    wire b_req = b_cyc_i && b_stb_i;

    always @(posedge clk) begin
        if (rst) begin
            busy <= 1'b0;
            owner <= 1'b0;
        end else if (!busy) begin
            if (a_req && b_req) begin
                busy <= 1'b1;
                owner <= ~owner;     // the one not served last
            end else if (a_req) begin
                busy <= 1'b1;
                owner <= 1'b0;
            end else if (b_req) begin
                busy <= 1'b1;
                owner <= 1'b1;
            end
        end else if (owner ? !b_cyc_i : !a_cyc_i) begin
            busy <= 1'b0;
        end
    end

    assign grant_b = busy && owner;
    wire grant_a = busy && !owner;

    assign s_adr_o = owner ? b_adr_i : a_adr_i;
    assign s_dat_o = owner ? b_dat_i : a_dat_i;
    assign s_we_o  = owner ? b_we_i : a_we_i;
    assign s_sel_o = owner ? b_sel_i : a_sel_i;
    assign s_stb_o = busy && (owner ? b_stb_i : a_stb_i);
    assign s_cyc_o = busy && (owner ? b_cyc_i : a_cyc_i);

    assign a_dat_o = s_dat_i;
    assign b_dat_o = s_dat_i;
    assign a_ack_o = grant_a && s_ack_i;
    assign b_ack_o = grant_b && s_ack_i;

endmodule
