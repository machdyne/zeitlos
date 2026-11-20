/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DMA controller (`DMA): one or two channels that move 32-bit words
 * between main memory and the SPI masters, or from one bus address
 * to another. docs/dma.md has the whole story; this is the summary.
 *
 * -- What it is for --
 *
 * The CPU moves every byte the SD card and the network deliver. Over
 * SPI that costs ~57-80 CPU cycles a byte (docs/sdcard.md), against 16
 * (DIV=0) or 32 (DIV=1) cycles of wire time; RMII frames are copied a
 * word at a time out of the MAC's buffer. Here a channel does the
 * moving, a word per transfer, and the CPU is free (or waits, at the
 * speed of the wire).
 *
 * -- How --
 *
 * Every word is a bus read and a bus write: SRC to the one data
 * register, then to DST. Either address can stay put (a device's data
 * register) or step a word at a time (memory, the RMII MAC's buffers).
 * What paces it is the only thing that differs:
 *
 *   PACE 0  none: as fast as the bus allows (MEM2MEM, the RMII MAC)
 *   PACE 1  SPI in: a 32-bit receive (all ones out) is started on the
 *           channel's SPI master; when it is done the word is read from
 *           SRC -- that master's DATA -- and the next receive started at
 *           once, so the write to memory overlaps the next word's wire
 *           time. An SD sector read, an ENC28J60 buffer read (RBM).
 *   PACE 2  SPI out: a word is read from memory only while the SPI
 *           master is idle, and written to its DATA -- which starts the
 *           transfer. An SD sector write, an ENC28J60 buffer write
 *           (WBM). The driver sets CTRL.XFER32 on that master first.
 *
 * Words are ordered as spim_wb's 32-bit mode orders them: byte 0 of
 * memory first on the wire. Byte-granular lengths are the driver's (it
 * rounds down, and moves the odd bytes itself).
 *
 * -- The SPI side --
 *
 * Two wires to each spim_wb: a start pulse (a 32-bit receive) and its
 * BUSY. The data goes over the bus, as the CPU's does, but a DATA
 * access is only made when BUSY is clear -- a DATA access while busy
 * stalls the bus for the whole transfer (spim.v, "What stalling
 * costs"), which a DMA engine must not do to everyone else. Channel 0
 * has the SD card's master, channel 1 the ENC28J60's; a one-channel
 * build has both on channel 0, chosen job by job (CTRL.PORT). The CPU keeps
 * chip select, the clock divider and the protocol (commands, tokens,
 * CRC); while a channel is busy on a master, software keeps off DATA.
 *
 * -- The bus side --
 *
 * One Wishbone master, single-word classic cycles, cyc dropped after
 * every read and every write: rtl/arbiter_main.v holds a grant for a
 * whole cyc, so dropping it is what lets the CPU, the blitter and the
 * mixer in between. A word's read and write go together (the data
 * register is shared); two channels with work take turns a word at a
 * time.
 *
 * Writes to main memory are seen by the caches' snoop (sysctl.v,
 * cache_snoop_stb), and the data cache is write-through, so nothing
 * needs flushing either way.
 *
 * -- Registers (word index; byte address = 0x7b00_0000 + 4 x index) --
 *
 *   channel c at 4c:
 *     +0 SRC   byte address of the first word read (bits 1:0 ignored)
 *     +1 DST   byte address of the first word written
 *     +2 -     reserved
 *     +3 CTRL  write: bit 0 GO (1 starts, 0 stops), bit 1 SRC fixed,
 *                     bit 2 DST fixed, bits 4:3 PACE, bit 5 PORT (a
 *                     one-channel build: which SPI master paces it, 0
 *                     the SD card's, 1 the ENC28J60's), bits 31:16 the
 *                     words to move (1..65535)
 *              read:  bit 0 BUSY, bits 31:16 words left
 *   14 INFO    channels built (1 or 2)
 *   15 MAGIC   "DMA1" (0x444d_4131)
 *
 * Start a channel only while its BUSY is clear: BUSY covers the words
 * left, a bus cycle under way and an SPI transfer the channel started,
 * so once it is clear nothing of the last job can land late. A stop
 * (GO 0) is for giving up; BUSY then clears by itself.
 *
 * Stores here are kernel-only (rtl/mpu.v): a DMA channel writes
 * physical memory, past every check the MPU makes, so drivers ask the
 * kernel (Z_SYS_DMA), which checks the buffer is theirs, translates it
 * and starts the channel. Loads are open, so a driver can wait on BUSY
 * without a syscall.
 *
 * -- Cost --
 *
 * Small on purpose (Lakritz is nearly full): no block RAM, one 32-bit
 * data register for both channels, no 32-bit data mux at all (the bus
 * write data is that register), one address incrementer shared by
 * every pointer, and the CPU's register writes and the engine's
 * pointer updates sharing one 30-bit mux. docs/dma.md has the
 * measured LUTs per board.
 */

module dma_wb #(
    parameter CHANNELS = 2          // 1 or 2
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- registers (slave) --
    input [3:0] wb_adr_i,           // word index
    input [31:0] wb_dat_i,
    output reg [31:0] wb_dat_o,
    input wb_we_i,
    input wb_stb_i,
    input wb_cyc_i,
    output reg wb_ack_o,

    // -- the bus (master) --
    output [31:0] m_adr_o,
    output [31:0] m_dat_o,
    input [31:0] m_dat_i,
    output m_we_o,
    output [3:0] m_sel_o,
    output reg m_stb_o,
    output reg m_cyc_o,
    input m_ack_i,

    // -- SPI sidebands (spim_wb): start a 32-bit receive; its BUSY --
    output spi0_start_o,
    input spi0_busy_i,
    output spi1_start_o,
    input spi1_busy_i
);

    localparam MAGIC = 32'h444d_4131;    // "DMA1"
    localparam P_NONE = 2'd0, P_IN = 2'd1, P_OUT = 2'd2;
    localparam S_IDLE = 2'd0, S_RD = 2'd1, S_GAP = 2'd2, S_WR = 2'd3;

    assign m_sel_o = 4'b1111;

    // -- channels --
    //
    // `kick`: a receive to start (P_IN) when the master is free.
    // `infl`: a receive this channel started; when the master is free
    // again (and the pulse is over) its word is waiting in DATA.

    reg [29:0] c0_src, c0_dst, c1_src, c1_dst;
    reg [15:0] c0_cnt, c1_cnt;
    reg c0_sfix, c0_dfix, c1_sfix, c1_dfix;
    reg [1:0] c0_pace, c1_pace;
    reg c0_kick, c1_kick, c0_infl, c1_infl;

    // One channel serves both SPI masters (CTRL bit 5 says which): the
    // smallest build, for a full Lakritz. With two, each has its own.
    reg c0_port;
    wire c0_port1 = (CHANNELS == 1) && c0_port;
    wire c0_busy_spi = c0_port1 ? spi1_busy_i : spi0_busy_i;
    reg c0_start;

    // -- the engine --
    reg c1_start;
    assign spi0_start_o = c0_start && !c0_port1;
    assign spi1_start_o = c1_start || (c0_start && c0_port1);

    reg [1:0] state;
    reg bch;                        // the channel of the word under way
    reg last;                       // served last, for taking turns
    reg [31:0] bdat;                // the word: read from SRC, written to DST

    wire c0_word = c0_infl && !c0_busy_spi && !c0_start;
    wire c1_word = c1_infl && !spi1_busy_i && !c1_start;

    wire c0_req = (c0_cnt != 16'd0) &&
        (c0_pace == P_IN  ? c0_word :
         c0_pace == P_OUT ? (!c0_busy_spi && !c0_start) : 1'b1);
    wire c1_req = (CHANNELS > 1) && (c1_cnt != 16'd0) &&
        (c1_pace == P_IN  ? c1_word :
         c1_pace == P_OUT ? (!spi1_busy_i && !c1_start) : 1'b1);

    wire pick = c1_req && (!c0_req || !last);

    wire c0_busy = (c0_cnt != 16'd0) || (state != S_IDLE && !bch) || c0_kick ||
                   c0_start || (c0_infl && c0_busy_spi);
    wire c1_busy = (c1_cnt != 16'd0) || (state != S_IDLE && bch) || c1_kick ||
                   c1_start || (c1_infl && spi1_busy_i);

    // the bus address: the channel's SRC while reading, DST after
    wire wr_side = state[1];        // S_GAP, S_WR
    wire [29:0] cur = bch ? (wr_side ? c1_dst : c1_src) : (wr_side ? c0_dst : c0_src);
    assign m_adr_o = { cur, 2'b00 };
    assign m_we_o = (state == S_WR);
    assign m_dat_o = bdat;

    // One value for every pointer to load: the CPU's register write,
    // or the word after the one just acked.
    wire cpu_wr = wb_cyc_i && wb_stb_i && !wb_ack_o && wb_we_i;
    wire [29:0] ptr_d = cpu_wr ? wb_dat_i[31:2] : (cur + 30'd1);
    wire acked = m_cyc_o && m_ack_i;
    wire rd_ack = acked && (state == S_RD);
    wire wr_ack = acked && (state == S_WR);

    always @(posedge wb_clk_i) begin

        // -- pointers: one shared input, separate enables --
        if ((cpu_wr && wb_adr_i == 4'd0) || (rd_ack && !bch && !c0_sfix)) c0_src <= ptr_d;
        if ((cpu_wr && wb_adr_i == 4'd1) || (wr_ack && !bch && !c0_dfix)) c0_dst <= ptr_d;
        // (built either way: with one channel nothing reads them, and
        // synthesis drops them)
        if ((cpu_wr && wb_adr_i == 4'd4) || (rd_ack && bch && !c1_sfix)) c1_src <= ptr_d;
        if ((cpu_wr && wb_adr_i == 4'd5) || (wr_ack && bch && !c1_dfix)) c1_dst <= ptr_d;
        if (rd_ack) bdat <= m_dat_i;

        if (wb_rst_i) begin
            c0_cnt <= 16'd0; c1_cnt <= 16'd0;
            c0_kick <= 1'b0; c1_kick <= 1'b0;
            c0_infl <= 1'b0; c1_infl <= 1'b0;
            c0_start <= 1'b0; c1_start <= 1'b0;
            state <= S_IDLE;
            m_cyc_o <= 1'b0; m_stb_o <= 1'b0;
            bch <= 1'b0; last <= 1'b0;
            wb_ack_o <= 1'b0;
        end else begin

            // -- receives: started when the master is free --
            c0_start <= 1'b0;
            c1_start <= 1'b0;
            if (c0_kick && !c0_busy_spi && !c0_start) begin
                c0_start <= 1'b1;
                c0_kick <= 1'b0;
                c0_infl <= 1'b1;
            end
            if (CHANNELS > 1 && c1_kick && !spi1_busy_i && !c1_start) begin
                c1_start <= 1'b1;
                c1_kick <= 1'b0;
                c1_infl <= 1'b1;
            end

            // -- the engine: a read, a cycle off the bus, a write --
            case (state)
                S_IDLE:
                    if (c0_req || c1_req) begin
                        bch <= pick;
                        last <= pick;
                        state <= S_RD;
                        m_cyc_o <= 1'b1;
                        m_stb_o <= 1'b1;
                        // the word in DATA is being taken
                        if (!pick) c0_infl <= 1'b0; else c1_infl <= 1'b0;
                    end
                S_RD:
                    if (m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        state <= S_GAP;
                        // DATA has been read: the next receive can go
                        if (!bch) begin
                            if (c0_pace == P_IN && c0_cnt > 16'd1) c0_kick <= 1'b1;
                        end else begin
                            if (c1_pace == P_IN && c1_cnt > 16'd1) c1_kick <= 1'b1;
                        end
                    end
                S_GAP: begin
                    m_cyc_o <= 1'b1;
                    m_stb_o <= 1'b1;
                    state <= S_WR;
                end
                default:    // S_WR
                    if (m_ack_i) begin
                        m_cyc_o <= 1'b0;
                        m_stb_o <= 1'b0;
                        state <= S_IDLE;
                        if (!bch) begin
                            if (c0_cnt != 16'd0) c0_cnt <= c0_cnt - 16'd1;
                        end else begin
                            if (c1_cnt != 16'd0) c1_cnt <= c1_cnt - 16'd1;
                        end
                    end
            endcase

            // -- registers --
            //
            // Last in the block, so a write here wins over the engine's
            // own update in the same cycle (a stop must stop).
            wb_ack_o <= 1'b0;
            if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
                wb_ack_o <= 1'b1;
                if (wb_we_i) begin
                    if (wb_adr_i == 4'd3) begin
                        c0_cnt <= wb_dat_i[0] ? wb_dat_i[31:16] : 16'd0;
                        c0_sfix <= wb_dat_i[1];
                        c0_dfix <= wb_dat_i[2];
                        c0_pace <= wb_dat_i[4:3];
                        c0_port <= wb_dat_i[5];
                        c0_kick <= wb_dat_i[0] && (wb_dat_i[4:3] == P_IN);
                        c0_infl <= 1'b0;
                    end
                    if (CHANNELS > 1 && wb_adr_i == 4'd7) begin
                        c1_cnt <= wb_dat_i[0] ? wb_dat_i[31:16] : 16'd0;
                        c1_sfix <= wb_dat_i[1];
                        c1_dfix <= wb_dat_i[2];
                        c1_pace <= wb_dat_i[4:3];
                        c1_kick <= wb_dat_i[0] && (wb_dat_i[4:3] == P_IN);
                        c1_infl <= 1'b0;
                    end
                end
            end
        end
    end

    // reads: registered, no reset (nothing reads it before an ack)
    always @(posedge wb_clk_i)
        case (wb_adr_i)
            4'd3:  wb_dat_o <= { c0_cnt, 15'd0, c0_busy };
            4'd7:  wb_dat_o <= (CHANNELS > 1) ? { c1_cnt, 15'd0, c1_busy } : 32'd0;
            4'd14: wb_dat_o <= CHANNELS;
            4'd15: wb_dat_o <= MAGIC;
            default: wb_dat_o <= 32'd0;
        endcase

endmodule
