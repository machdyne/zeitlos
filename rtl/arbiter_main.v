/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * 3-Port Wishbone arbiter for the MAIN bus (CPU, GPU Blit source
 * reads, audio mixer sample fetches).
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
 * Whichever master was granted last drops to lowest priority for the
 * next arbitration. Fixed priority has a bad failure mode here: with
 * the blitter on top, a long blit interleaves badly with a CPU that is
 * trying to make progress on the same bus; with the CPU on top, a CPU
 * in a tight load/store loop can keep a blit from ever finishing, and
 * the blitter holds the framebuffer's busy flag the whole time, so
 * software polling that flag would spin forever against a blit that
 * never advances. Rotating bounds both.
 *
 * -- why the audio mixer is IN the rotation, not below it --
 *
 * The obvious arrangement gives audio lowest priority, on the grounds
 * that it has a FIFO to absorb latency and the CPU and blitter do not.
 * That was the original plan here and it is not what this does.
 *
 * The mixer asks for eight single-word reads per sample period -- at
 * 44.1kHz that is 353k reads/sec against a 48MHz bus, under 1% of it.
 * A master that light perturbs nobody by being in the rotation, and
 * putting it below the others buys nothing measurable while inventing
 * a starvation corner that only appears under a long blit: exactly the
 * conditions (a game rendering hard) under which the audio must not
 * break. A bounded, uninteresting share beats an unbounded wait.
 *
 * -- rotate, not a single prefer bit --
 *
 * Two masters could be arbitrated with one flip-flop saying "prefer
 * the other one next". Three cannot: `rotate` names which master is
 * checked FIRST, and the other two are checked in order after it, so
 * the master that just finished always ends up last in line.
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

    // Master 2 (audio mixer sample fetches -- read only, but the full
    // signal set is kept so this port is identical to the others and a
    // future writing master can drop straight in)
    input  wire [31:0] m2_adr_i,
    input  wire [31:0] m2_dat_i,
    output reg  [31:0] m2_dat_o,
    input  wire        m2_we_i,
    input  wire [3:0]  m2_sel_i,
    input  wire        m2_stb_i,
    input  wire        m2_cyc_i,
    output reg         m2_ack_o,

    // Shared slave interface
    output reg  [31:0] s_adr_o,
    output reg  [31:0] s_dat_o,
    input  wire [31:0] s_dat_i,
    output reg         s_we_o,
    output reg  [3:0]  s_sel_o,
    output reg         s_stb_o,
    output reg         s_cyc_o,
    input  wire        s_ack_i,

    // Current bus master: 0 = CPU, 1 = GPU Blit, 2 = audio mixer
    output reg  [1:0]  master
);

    reg [1:0] state;
    localparam IDLE = 2'd0, M0_ACTIVE = 2'd1, M1_ACTIVE = 2'd2,
               M2_ACTIVE = 2'd3;

    // which master to check FIRST on the next arbitration -- see this
    // file's header on why this is a pointer and not a flag
    reg [1:0] rotate;

    wire m0_req = m0_cyc_i && m0_stb_i;
    wire m1_req = m1_cyc_i && m1_stb_i;
    wire m2_req = m2_cyc_i && m2_stb_i;

    // The three masters in the order they should be considered, given
    // `rotate`. Written out rather than computed with a modulo so the
    // whole policy is one readable table.
    wire [1:0] try0 = rotate;
    wire [1:0] try1 = (rotate == 2'd2) ? 2'd0 : (rotate + 2'd1);
    wire [1:0] try2 = (try1   == 2'd2) ? 2'd0 : (try1   + 2'd1);

    wire try0_req = (try0 == 2'd0) ? m0_req : (try0 == 2'd1) ? m1_req : m2_req;
    wire try1_req = (try1 == 2'd0) ? m0_req : (try1 == 2'd1) ? m1_req : m2_req;
    wire try2_req = (try2 == 2'd0) ? m0_req : (try2 == 2'd1) ? m1_req : m2_req;

    wire        any_req  = try0_req || try1_req || try2_req;
    wire [1:0]  winner   = try0_req ? try0 : try1_req ? try1 : try2;

    always @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            master <= 2'd0;
            rotate <= 2'd0;
        end else begin
            case (state)

                IDLE: begin
                    if (any_req) begin
                        master <= winner;
                        // the winner drops to the back of the queue
                        rotate <= (winner == 2'd2) ? 2'd0 : (winner + 2'd1);
                        case (winner)
                            2'd0: state <= M0_ACTIVE;
                            2'd1: state <= M1_ACTIVE;
                            default: state <= M2_ACTIVE;
                        endcase
                    end
                end

                // The grant only moves when the current master drops
                // cyc, so a transaction is never torn in half.
                M0_ACTIVE: if (!m0_cyc_i) state <= IDLE;
                M1_ACTIVE: if (!m1_cyc_i) state <= IDLE;
                M2_ACTIVE: if (!m2_cyc_i) state <= IDLE;

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
        m2_ack_o = 1'b0;

        // Both masters see the slave's read data. Only the granted one
        // is told it is valid (via ack above), so leaving this
        // unqualified costs nothing and keeps the mux small.
        m0_dat_o = s_dat_i;
        m1_dat_o = s_dat_i;
        m2_dat_o = s_dat_i;

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

            M2_ACTIVE: begin
                s_adr_o = m2_adr_i;
                s_dat_o = m2_dat_i;
                s_we_o  = m2_we_i;
                s_sel_o = m2_sel_i;
                s_stb_o = m2_stb_i;
                s_cyc_o = m2_cyc_i;
                m2_ack_o = s_ack_i;
            end

            default: ;

        endcase

    end

endmodule
