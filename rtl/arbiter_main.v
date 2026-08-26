/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * 2-Port Wishbone arbiter for the MAIN bus (CPU, GPU Blit source
 * reads).
 *
 * Separate from rtl/arbiter_vram.v rather than a parameterised version
 * of it. That one arbitrates the VRAM bus between three masters and is
 * relied on by every existing bitstream; generalising it would put a
 * working, shipped path at risk to save a file.
 *
 * -- why the main bus needs an arbiter at all --
 *
 * Until the blitter gained memory-copy mode (rtl/gpu/gpu_blit.v,
 * CTRL_SRCMEM) the CPU was the only master here -- sysctl.v's own
 * comment said as much, "CPU controls the main bus (will share with DMA
 * controller)". Reading a bitmap out of main memory makes the blitter a
 * second master, and this is the sharing that comment anticipated.
 *
 * -- round robin, not fixed priority --
 *
 * Whichever master was granted last drops to lower priority for the
 * next arbitration. Fixed priority either way has a bad failure mode
 * here: with the blitter on top, a long blit interleaves badly with a
 * CPU that is trying to make progress on the same bus; with the CPU on
 * top, a CPU in a tight load/store loop can keep a blit from ever
 * finishing, and the blitter holds the framebuffer's busy flag the
 * whole time, so software polling that flag would spin forever against
 * a blit that never advances. Alternating bounds both.
 *
 * -- grant is held for a whole transaction --
 *
 * The grant only moves when the current master drops cyc, so a
 * transaction is never torn in half. A master that loses the bus while
 * still asserting cyc simply stops seeing acks and waits, which is
 * exactly what Wishbone masters already do -- picorv32_wb holds its
 * address and data stable until it is acked.
 */

module wb_arbiter_main (
    input  wire        clk,
    input  wire        rst,

    // Master 0 (CPU / instruction cache)
    input  wire [31:0] m0_adr_i,
    input  wire [31:0] m0_dat_i,
    output reg  [31:0] m0_dat_o,
    input  wire        m0_we_i,
    input  wire [3:0]  m0_sel_i,
    input  wire        m0_stb_i,
    input  wire        m0_cyc_i,
    output reg         m0_ack_o,

    // Master 1 (GPU Blit source reads)
    input  wire [31:0] m1_adr_i,
    input  wire [31:0] m1_dat_i,
    output reg  [31:0] m1_dat_o,
    input  wire        m1_we_i,
    input  wire [3:0]  m1_sel_i,
    input  wire        m1_stb_i,
    input  wire        m1_cyc_i,
    output reg         m1_ack_o,

    // Shared slave interface
    output reg  [31:0] s_adr_o,
    output reg  [31:0] s_dat_o,
    input  wire [31:0] s_dat_i,
    output reg         s_we_o,
    output reg  [3:0]  s_sel_o,
    output reg         s_stb_o,
    output reg         s_cyc_o,
    input  wire        s_ack_i,

    // Current bus master: 0 = CPU, 1 = GPU Blit
    output reg         master
);

    reg [1:0] state;
    localparam IDLE = 2'd0, M0_ACTIVE = 2'd1, M1_ACTIVE = 2'd2;

    // which master to prefer on the next arbitration -- flipped away
    // from whoever just had the bus
    reg prefer_m1;

    wire m0_req = m0_cyc_i && m0_stb_i;
    wire m1_req = m1_cyc_i && m1_stb_i;

    always @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            master <= 1'b0;
            prefer_m1 <= 1'b0;
        end else begin
            case (state)

                IDLE: begin
                    if (prefer_m1) begin
                        if (m1_req) begin
                            state <= M1_ACTIVE; master <= 1'b1; prefer_m1 <= 1'b0;
                        end else if (m0_req) begin
                            state <= M0_ACTIVE; master <= 1'b0; prefer_m1 <= 1'b1;
                        end
                    end else begin
                        if (m0_req) begin
                            state <= M0_ACTIVE; master <= 1'b0; prefer_m1 <= 1'b1;
                        end else if (m1_req) begin
                            state <= M1_ACTIVE; master <= 1'b1; prefer_m1 <= 1'b0;
                        end
                    end
                end

                M0_ACTIVE: if (!m0_cyc_i) state <= IDLE;
                M1_ACTIVE: if (!m1_cyc_i) state <= IDLE;

                default: state <= IDLE;

            endcase
        end
    end

    always @(*) begin

        s_adr_o = 32'h00000000;
        s_dat_o = 32'h00000000;
        s_we_o  = 1'b0;
        s_sel_o = 4'b0000;
        s_stb_o = 1'b0;
        s_cyc_o = 1'b0;

        // Acks are steered to the granted master ONLY. An ungranted
        // master seeing a stray ack would consider a transaction
        // complete that never reached the slave -- for the CPU that is
        // a silently wrong load, which is about the worst failure this
        // module could produce.
        m0_ack_o = 1'b0;
        m1_ack_o = 1'b0;

        // Both masters see the slave's read data. Only the granted one
        // is told it is valid (via ack above), so leaving this
        // unqualified costs nothing and keeps the mux small.
        m0_dat_o = s_dat_i;
        m1_dat_o = s_dat_i;

        case (state)

            M0_ACTIVE: begin
                s_adr_o = m0_adr_i;
                s_dat_o = m0_dat_i;
                s_we_o  = m0_we_i;
                s_sel_o = m0_sel_i;
                s_stb_o = m0_stb_i;
                s_cyc_o = m0_cyc_i;
                m0_ack_o = s_ack_i;
            end

            M1_ACTIVE: begin
                s_adr_o = m1_adr_i;
                s_dat_o = m1_dat_i;
                s_we_o  = m1_we_i;
                s_sel_o = m1_sel_i;
                s_stb_o = m1_stb_i;
                s_cyc_o = m1_cyc_i;
                m1_ack_o = s_ack_i;
            end

            default: ;

        endcase

    end

endmodule
