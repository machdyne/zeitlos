/*
 * tb_dma.v -- rtl/dma.v on a small copy of the SoC's main bus: a
 * "CPU" (this testbench), the DMA beside a reader standing in for the
 * audio mixer (rtl/arbiter_dma.v, as sysctl.v has them), two real
 * spim_wb masters with an SPI device each, and a memory that answers
 * after a random wait.
 *
 *   make -C rtl/tests dma
 *
 * The devices send a counting byte stream and record every byte they
 * receive, so the order on the wire is checked against memory exactly
 * as a byte-at-a-time driver would have seen it.
 *
 * Checks: SPI in (pace 1) and SPI out (pace 2) on each channel, a
 * memory copy, both channels and the mixer at once, a stop part way
 * (no write after BUSY clears, none past the end, the next job whole),
 * one-word jobs, the CPU's own SPI transfers afterwards, that the DMA
 * never touches an SPI master's DATA while it is busy (which would
 * stall the bus), and a one-channel build. Then the time for a sector.
 */

`timescale 1ns/1ps

module spidev (
    input cs_n, input sck, input mosi, output reg miso,
    input [7:0] seed
);
    reg [7:0] tx, rx;
    reg [7:0] next;
    reg [3:0] bits;
    reg [7:0] got [0:4095];
    integer n_got;
    initial begin
        n_got = 0; bits = 0; next = seed; tx = seed; miso = 1;
    end
    always @(negedge cs_n) begin
        bits = 0; tx = next; next = next + 8'd1; miso = tx[7];
    end
    always @(posedge sck) if (!cs_n) begin
        rx = { rx[6:0], mosi };
        bits = bits + 1;
        if (bits == 8) begin
            got[n_got] = rx; n_got = n_got + 1; bits = 0;
        end
    end
    always @(negedge sck) if (!cs_n) begin
        if (bits == 0) begin
            tx = next; next = next + 8'd1;
        end else begin
            tx = { tx[6:0], 1'b1 };
        end
        miso = tx[7];
    end
endmodule

module tb_dma #(parameter DUT_CH = 2);
    reg clk = 0;
    always #5 clk = ~clk;
    reg rst = 1;

    integer errors = 0;
    integer i, t;

    // -- masters --------------------------------------------------------

    // the CPU (tasks below)
    reg [31:0] c_adr = 0, c_dat = 0;
    reg c_we = 0, c_cyc = 0;
    wire [31:0] c_rdat;
    wire c_ack;

    // the "mixer": single reads of 0x3f00.. (never written), checked
    reg [31:0] x_adr = 32'h3f00;
    reg x_cyc = 0;
    wire [31:0] x_dat;
    wire x_ack;
    integer x_reads = 0;

    // the DMA's master
    wire [31:0] d_adr, d_dat_o, d_dat_i;
    wire d_we, d_stb, d_cyc, d_ack;
    wire [3:0] d_sel;

    // port 2 (mixer + DMA), then the main bus (CPU + port 2)
    wire [31:0] p_adr, p_dat_o, p_dat_i;
    wire p_we, p_stb, p_cyc, p_ack, dma_granted;
    wire [3:0] p_sel;
    wire [31:0] b_adr, b_dat_o, b_dat_i;
    wire b_we, b_stb, b_cyc, b_ack, p_granted;
    wire [3:0] b_sel;

    wb_arbiter2 arb_p2 (
        .clk(clk), .rst(rst),
        .a_adr_i(x_adr), .a_dat_i(32'h0), .a_dat_o(x_dat), .a_we_i(1'b0), .a_sel_i(4'hf),
        .a_stb_i(x_cyc), .a_cyc_i(x_cyc), .a_ack_o(x_ack),
        .b_adr_i(d_adr), .b_dat_i(d_dat_o), .b_dat_o(d_dat_i), .b_we_i(d_we), .b_sel_i(d_sel),
        .b_stb_i(d_stb), .b_cyc_i(d_cyc), .b_ack_o(d_ack),
        .s_adr_o(p_adr), .s_dat_o(p_dat_o), .s_dat_i(p_dat_i), .s_we_o(p_we), .s_sel_o(p_sel),
        .s_stb_o(p_stb), .s_cyc_o(p_cyc), .s_ack_i(p_ack),
        .grant_b(dma_granted));

    wb_arbiter2 arb_main (
        .clk(clk), .rst(rst),
        .a_adr_i(c_adr), .a_dat_i(c_dat), .a_dat_o(c_rdat), .a_we_i(c_we), .a_sel_i(4'hf),
        .a_stb_i(c_cyc), .a_cyc_i(c_cyc), .a_ack_o(c_ack),
        .b_adr_i(p_adr), .b_dat_i(p_dat_o), .b_dat_o(p_dat_i), .b_we_i(p_we), .b_sel_i(p_sel),
        .b_stb_i(p_stb), .b_cyc_i(p_cyc), .b_ack_o(p_ack),
        .s_adr_o(b_adr), .s_dat_o(b_dat_o), .s_dat_i(b_dat_i), .s_we_o(b_we), .s_sel_o(b_sel),
        .s_stb_o(b_stb), .s_cyc_o(b_cyc), .s_ack_i(b_ack),
        .grant_b(p_granted));

    // -- slaves: DMA registers 0x7b.., spi0 0xb0.., spi1 0x61.., memory --
    wire cs_dma  = b_adr[31:24] == 8'h7b;
    wire cs_one  = b_adr[31:24] == 8'h7a;    // the one-channel build
    wire cs_spi0 = b_adr[31:24] == 8'hb0;
    wire cs_spi1 = b_adr[31:24] == 8'h61;
    wire cs_mem  = !(cs_dma || cs_one || cs_spi0 || cs_spi1);

    wire [31:0] dma_dat, one_dat, s0_dat, s1_dat;
    wire dma_ack, one_ack, s0_ack, s1_ack;
    reg [31:0] mem_dat = 0;
    reg mem_ack = 0;
    assign b_dat_i = cs_dma ? dma_dat : cs_one ? one_dat : cs_spi0 ? s0_dat : cs_spi1 ? s1_dat : mem_dat;
    assign b_ack = cs_dma ? dma_ack : cs_one ? one_ack : cs_spi0 ? s0_ack : cs_spi1 ? s1_ack : mem_ack;

    reg [31:0] mem [0:4095];
    integer wait_n = 0;
    integer writes = 0;
    always @(posedge clk) begin
        mem_ack <= 1'b0;
        if (b_cyc && b_stb && cs_mem && !mem_ack) begin
            if (wait_n == 0) wait_n = 1 + ($random & 7);
            wait_n = wait_n - 1;
            if (wait_n == 0) begin
                mem_ack <= 1'b1;
                if (b_we) begin
                    mem[b_adr[13:2]] <= b_dat_o;
                    if (p_granted && dma_granted) writes = writes + 1;
                end else mem_dat <= mem[b_adr[13:2]];
            end
        end
    end

    // -- spims and their devices ----------------------------------------
    wire s0_cs, s0_sck, s0_mosi, s0_miso, s1_cs, s1_sck, s1_mosi, s1_miso;
    wire d0_start, d1_start, d0_busy, d1_busy;

    spim_wb #(.DEFAULT_DIV(8'd0)) spi0 (
        .wb_clk_i(clk), .wb_rst_i(rst), .wb_adr_i({30'd0, b_adr[3:2]}), .wb_dat_i(b_dat_o),
        .wb_dat_o(s0_dat), .wb_we_i(b_we), .wb_sel_i(4'hf), .wb_stb_i(b_stb && cs_spi0),
        .wb_ack_o(s0_ack), .wb_cyc_i(b_cyc && cs_spi0),
        .spi_cs_n(s0_cs), .spi_sck(s0_sck), .spi_mosi(s0_mosi), .spi_miso(s0_miso),
        .spi_int(1'b1), .dma_start_i(d0_start), .dma_busy_o(d0_busy));
    spim_wb #(.DEFAULT_DIV(8'd1)) spi1 (
        .wb_clk_i(clk), .wb_rst_i(rst), .wb_adr_i({30'd0, b_adr[3:2]}), .wb_dat_i(b_dat_o),
        .wb_dat_o(s1_dat), .wb_we_i(b_we), .wb_sel_i(4'hf), .wb_stb_i(b_stb && cs_spi1),
        .wb_ack_o(s1_ack), .wb_cyc_i(b_cyc && cs_spi1),
        .spi_cs_n(s1_cs), .spi_sck(s1_sck), .spi_mosi(s1_mosi), .spi_miso(s1_miso),
        .spi_int(1'b1), .dma_start_i(d1_start), .dma_busy_o(d1_busy));

    spidev dv0 (.cs_n(s0_cs), .sck(s0_sck), .mosi(s0_mosi), .miso(s0_miso), .seed(8'h10));
    spidev dv1 (.cs_n(s1_cs), .sck(s1_sck), .mosi(s1_mosi), .miso(s1_miso), .seed(8'h80));

    dma_wb #(.CHANNELS(DUT_CH)) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(b_adr[5:2]), .wb_dat_i(b_dat_o), .wb_dat_o(dma_dat),
        .wb_we_i(b_we), .wb_stb_i(b_stb && cs_dma), .wb_cyc_i(b_cyc && cs_dma), .wb_ack_o(dma_ack),
        .m_adr_o(d_adr), .m_dat_o(d_dat_o), .m_dat_i(d_dat_i), .m_we_o(d_we),
        .m_sel_o(d_sel), .m_stb_o(d_stb), .m_cyc_o(d_cyc), .m_ack_i(d_ack),
        .spi0_start_o(d0_start), .spi0_busy_i(d0_busy),
        .spi1_start_o(d1_start), .spi1_busy_i(d1_busy));

    wire one_cyc;
    dma_wb #(.CHANNELS(1)) one (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(b_adr[5:2]), .wb_dat_i(b_dat_o), .wb_dat_o(one_dat),
        .wb_we_i(b_we), .wb_stb_i(b_stb && cs_one), .wb_cyc_i(b_cyc && cs_one), .wb_ack_o(one_ack),
        .m_adr_o(), .m_dat_o(), .m_dat_i(32'h0), .m_we_o(), .m_sel_o(), .m_stb_o(), .m_cyc_o(one_cyc),
        .m_ack_i(1'b0),
        .spi0_start_o(), .spi0_busy_i(1'b0), .spi1_start_o(), .spi1_busy_i(1'b0));

    // -- watchers --------------------------------------------------------

    // the mixer
    always @(posedge clk) begin
        if (rst) x_cyc <= 0;
        else if (x_cyc && x_ack) begin
            if (x_dat !== (32'hfeed0000 | x_adr[11:2])) begin
                $display("FAIL: mixer read %h at %h", x_dat, x_adr); errors = errors + 1;
            end
            x_reads = x_reads + 1;
            x_cyc <= 0;
            x_adr <= 32'h3f00 + (($random & 63) << 2);
        end else if (!x_cyc && ($random & 15) == 0) x_cyc <= 1;
    end

    // the DMA never reaches an SPI master's DATA while it is busy
    always @(posedge clk)
        if (b_cyc && b_stb && !b_ack && p_granted && dma_granted &&
            ((cs_spi0 && b_adr[3:2] == 0 && d0_busy) || (cs_spi1 && b_adr[3:2] == 0 && d1_busy))) begin
            $display("FAIL: DMA at a busy SPI master's DATA (%h) -- that stalls the bus", b_adr);
            errors = errors + 1;
        end

    // -- the CPU ----------------------------------------------------------

    reg [31:0] rv;
    task wr(input [31:0] a, input [31:0] v);
        begin
            @(negedge clk);
            c_adr = a; c_dat = v; c_we = 1; c_cyc = 1;
            @(posedge clk); while (!c_ack) @(posedge clk);
            @(negedge clk); c_cyc = 0; c_we = 0;
        end
    endtask

    task rd(input [31:0] a);
        begin
            @(negedge clk);
            c_adr = a; c_we = 0; c_cyc = 1;
            @(posedge clk); while (!c_ack) @(posedge clk);
            #1 rv = c_rdat;
            @(negedge clk); c_cyc = 0;
        end
    endtask

    task wait_idle(input integer ch);
        begin
            rv = 1; t = 0;
            while (rv[0] && t < 200000) begin rd(32'h7b00000c + ch * 16); t = t + 1; end
            if (rv[0]) begin $display("FAIL: channel %0d never finished", ch); errors = errors + 1; end
        end
    endtask

    // CTRL: GO, SRC fixed, DST fixed, PACE, words
    function [31:0] ctrl(input sfix, input dfix, input [1:0] pace, input [15:0] words);
        ctrl = { words, 11'd0, pace, dfix, sfix, 1'b1 };
    endfunction

    function [7:0] mbyte(input integer byte_addr);
        mbyte = mem[byte_addr >> 2] >> (8 * (byte_addr & 3));
    endfunction

    integer base1, w0, t0, n0;

    initial begin
        for (i = 0; i < 4096; i = i + 1) mem[i] = 32'hdead0000 | i;
        for (i = 32'h3f00 >> 2; i < (32'h3f00 >> 2) + 64; i = i + 1) mem[i] = 32'hfeed0000 | i[9:0];
        repeat (4) @(posedge clk);
        rst = 0;
        repeat (2) @(posedge clk);

        rd(32'h7b00003c);
        if (rv !== 32'h444d4131) begin $display("FAIL: magic %h", rv); errors = errors + 1; end
        rd(32'h7b000038);
        if (rv !== DUT_CH) begin $display("FAIL: info %0d", rv); errors = errors + 1; end

        if (DUT_CH == 1) begin
            // one channel for both SPI masters, CTRL.PORT choosing
            wr(32'hb0000008, 32'h0000_0001);
            wr(32'h61000008, 32'h0000_0103);
            // in on port 1 (spi1), 64 words
            base1 = dv1.n_got;
            wr(32'h7b000000, 32'h61000000); wr(32'h7b000004, 32'h0400);
            wr(32'h7b00000c, ctrl(1, 0, 1, 64) | 32'h20);
            wait_idle(0);
            for (i = 0; i < 256; i = i + 1)
                if (mbyte(32'h400 + i) !== ((8'h80 + base1 + i) & 8'hff)) begin
                    if (errors < 10) $display("FAIL: one channel, in on port 1, byte %0d = %h", i, mbyte(32'h400 + i));
                    errors = errors + 1;
                end
            if (dv0.n_got != 0) begin $display("FAIL: port 1 job clocked the SD card"); errors = errors + 1; end
            // out on port 1
            base1 = dv1.n_got;
            for (i = 0; i < 16; i = i + 1) mem[(32'h2000 >> 2) + i] = 32'h01020304 * (i + 1);
            wr(32'h7b000000, 32'h2000); wr(32'h7b000004, 32'h61000000);
            wr(32'h7b00000c, ctrl(0, 1, 2, 16) | 32'h20);
            wait_idle(0);
            rd(32'h61000004); while (rv[0]) rd(32'h61000004);
            for (i = 0; i < 64; i = i + 1)
                if (dv1.got[base1 + i] !== mbyte(32'h2000 + i)) begin
                    if (errors < 10) $display("FAIL: one channel, out on port 1, byte %0d", i);
                    errors = errors + 1;
                end
            // and in on port 0, the SD card's
            wr(32'h7b000000, 32'hb0000000); wr(32'h7b000004, 32'h0800);
            wr(32'h7b00000c, ctrl(1, 0, 1, 32));
            wait_idle(0);
            for (i = 0; i < 128; i = i + 1)
                if (mbyte(32'h800 + i) !== ((8'h10 + i) & 8'hff)) begin
                    if (errors < 10) $display("FAIL: one channel, in on port 0, byte %0d", i);
                    errors = errors + 1;
                end
            // channel 1 is not there
            wr(32'h7b00001c, ctrl(0, 0, 0, 16));
            rd(32'h7b00001c);
            if (rv !== 0) begin $display("FAIL: one channel: channel 1 answers %h", rv); errors = errors + 1; end
            if (errors == 0) $display("tb_dma (one channel): all checks passed");
            else $display("tb_dma (one channel): %0d errors", errors);
            $finish;
        end
        rd(32'h7a000038);
        if (rv !== 1) begin $display("FAIL: one-channel info %0d", rv); errors = errors + 1; end
        wr(32'h7a00001c, ctrl(0, 0, 0, 16));
        rd(32'h7a00001c);
        if (rv !== 0 || one_cyc) begin $display("FAIL: one-channel build ran channel 1"); errors = errors + 1; end

        // chip selects on (the CPU's job); spi1 in 32-bit mode for pace 2
        wr(32'hb0000008, 32'h0000_0001);
        wr(32'h61000008, 32'h0000_0103);

        // -- 1. SPI in, channel 0: 128 words (a sector) at 0x400 ---------
        wr(32'h7b000000, 32'hb0000000);
        wr(32'h7b000004, 32'h400);
        wr(32'h7b00000c, ctrl(1, 0, 1, 128));
        wait_idle(0);
        for (i = 0; i < 512; i = i + 1)
            if (mbyte(32'h400 + i) !== ((8'h10 + i) & 8'hff)) begin
                if (errors < 10) $display("FAIL: spi in, byte %0d = %h", i, mbyte(32'h400 + i));
                errors = errors + 1;
            end
        if (mem[(32'h400 >> 2) + 128] !== (32'hdead0000 | ((32'h400 >> 2) + 128))) begin
            $display("FAIL: spi in wrote past the end"); errors = errors + 1;
        end
        for (i = 0; i < dv0.n_got; i = i + 1)
            if (dv0.got[i] !== 8'hff) begin $display("FAIL: spi in sent %h", dv0.got[i]); errors = errors + 1; end
        if (dv0.n_got != 512) begin $display("FAIL: spi in, %0d bytes on the wire", dv0.n_got); errors = errors + 1; end

        // -- 2. SPI out, channel 1: 37 words from 0x2000 ------------------
        for (i = 0; i < 64; i = i + 1) mem[(32'h2000 >> 2) + i] = { i[7:0] + 8'd3, i[7:0] + 8'd2, i[7:0] + 8'd1, i[7:0] };
        wr(32'h7b000010, 32'h2000);
        wr(32'h7b000014, 32'h61000000);
        wr(32'h7b00001c, ctrl(0, 1, 2, 37));
        wait_idle(1);
        rd(32'h61000004);    // and the last word off the wire
        while (rv[0]) rd(32'h61000004);
        if (dv1.n_got != 37 * 4) begin $display("FAIL: spi out, %0d bytes", dv1.n_got); errors = errors + 1; end
        for (i = 0; i < dv1.n_got; i = i + 1)
            if (dv1.got[i] !== mbyte(32'h2000 + i)) begin
                if (errors < 10) $display("FAIL: spi out, byte %0d = %h, want %h", i, dv1.got[i], mbyte(32'h2000 + i));
                errors = errors + 1;
            end

        // -- 3. memory to memory, channel 0: 300 words 0x1000 -> 0x3000 ---
        for (i = 0; i < 300; i = i + 1) mem[(32'h1000 >> 2) + i] = 32'hc0de0000 + i * 7;
        wr(32'h7b000000, 32'h1000); wr(32'h7b000004, 32'h3000);
        wr(32'h7b00000c, ctrl(0, 0, 0, 300));
        wait_idle(0);
        for (i = 0; i < 300; i = i + 1)
            if (mem[(32'h3000 >> 2) + i] !== 32'hc0de0000 + i * 7) begin
                if (errors < 10) $display("FAIL: copy, word %0d", i);
                errors = errors + 1;
            end

        // -- 4. both channels at once: SPI in on 0, SPI out on 1 ----------
        base1 = dv1.n_got;
        for (i = 0; i < 100; i = i + 1) mem[(32'h2800 >> 2) + i] = 32'h11223344 ^ (i * 32'h01010101);
        wr(32'h7b000000, 32'hb0000000); wr(32'h7b000004, 32'h0800);
        wr(32'h7b000010, 32'h2800);     wr(32'h7b000014, 32'h61000000);
        wr(32'h7b00000c, ctrl(1, 0, 1, 100));
        wr(32'h7b00001c, ctrl(0, 1, 2, 100));
        wait_idle(0); wait_idle(1);
        rd(32'h61000004); while (rv[0]) rd(32'h61000004);
        for (i = 0; i < 400; i = i + 1)
            if (mbyte(32'h800 + i) !== ((8'h10 + 512 + i) & 8'hff)) begin
                if (errors < 10) $display("FAIL: both, spi in byte %0d = %h", i, mbyte(32'h800 + i));
                errors = errors + 1;
            end
        for (i = 0; i < 400; i = i + 1)
            if (dv1.got[base1 + i] !== mbyte(32'h2800 + i)) begin
                if (errors < 10) $display("FAIL: both, spi out byte %0d", i);
                errors = errors + 1;
            end

        // -- 5. a stop part way: BUSY clears, nothing lands after ---------
        wr(32'h7b000000, 32'hb0000000); wr(32'h7b000004, 32'h0c00);
        wr(32'h7b00000c, ctrl(1, 0, 1, 1000));
        repeat (700) @(posedge clk);
        wr(32'h7b00000c, 32'h0);
        wait_idle(0);
        w0 = writes;
        repeat (300) @(posedge clk);
        if (writes != w0) begin $display("FAIL: a write after the stop"); errors = errors + 1; end
        rd(32'h7b00000c);
        if (rv[31:16] != 0) begin $display("FAIL: words left after a stop: %0d", rv[31:16]); errors = errors + 1; end
        // and the next job is whole: none of the stopped one's words in it
        n0 = dv0.n_got;
        wr(32'h7b000004, 32'h0e00);
        wr(32'h7b00000c, ctrl(1, 0, 1, 8));
        wait_idle(0);
        for (i = 0; i < 32; i = i + 1)
            if (mbyte(32'h0e00 + i) !== ((8'h10 + n0 + i) & 8'hff)) begin
                if (errors < 10) $display("FAIL: after a stop, byte %0d = %h", i, mbyte(32'h0e00 + i));
                errors = errors + 1;
            end

        // -- 6. one word each way ----------------------------------------
        base1 = dv1.n_got;
        mem[32'h2f00 >> 2] = 32'ha1b2c3d4;
        wr(32'h7b000010, 32'h2f00); wr(32'h7b000014, 32'h61000000);
        wr(32'h7b00001c, ctrl(0, 1, 2, 1));
        wait_idle(1);
        rd(32'h61000004); while (rv[0]) rd(32'h61000004);
        if (dv1.n_got != base1 + 4 || dv1.got[base1] !== 8'hd4 || dv1.got[base1 + 3] !== 8'ha1) begin
            $display("FAIL: one word out"); errors = errors + 1;
        end
        mem[32'h0f00 >> 2] = 0; mem[(32'h0f00 >> 2) + 1] = 32'h5a5a5a5a;
        wr(32'h7b000000, 32'hb0000000); wr(32'h7b000004, 32'h0f00);
        wr(32'h7b00000c, ctrl(1, 0, 1, 1));
        wait_idle(0);
        if (mem[(32'h0f00 >> 2) + 1] !== 32'h5a5a5a5a || mem[32'h0f00 >> 2] === 0) begin
            $display("FAIL: one word in"); errors = errors + 1;
        end

        // -- 7. the CPU's own transfers still work afterwards --------------
        wr(32'hb0000000, 32'h0000_00a5);
        rd(32'hb0000000);
        if (dv0.got[dv0.n_got - 1] !== 8'ha5) begin $display("FAIL: CPU transfer after DMA"); errors = errors + 1; end

        // -- 7b. the other way round: SPI out on channel 0, in on 1 -------
        wr(32'hb0000008, 32'h0000_0003);        // spi0: 32-bit, for pace 2
        n0 = dv0.n_got; base1 = dv1.n_got;
        wr(32'h7b000000, 32'h2000); wr(32'h7b000004, 32'hb0000000);
        wr(32'h7b000010, 32'h61000000); wr(32'h7b000014, 32'h3400);
        wr(32'h7b00000c, ctrl(0, 1, 2, 20));
        wr(32'h7b00001c, ctrl(1, 0, 1, 20));
        wait_idle(0); wait_idle(1);
        rd(32'hb0000004); while (rv[0]) rd(32'hb0000004);
        if (dv0.n_got != n0 + 80) begin $display("FAIL: out on 0, %0d bytes", dv0.n_got - n0); errors = errors + 1; end
        for (i = 0; i < 80; i = i + 1) begin
            if (dv0.got[n0 + i] !== mbyte(32'h2000 + i)) begin
                if (errors < 10) $display("FAIL: out on 0, byte %0d", i);
                errors = errors + 1;
            end
            if (mbyte(32'h3400 + i) !== ((8'h80 + base1 + i) & 8'hff)) begin
                if (errors < 10) $display("FAIL: in on 1, byte %0d = %h", i, mbyte(32'h3400 + i));
                errors = errors + 1;
            end
        end
        wr(32'hb0000008, 32'h0000_0001);

        if (x_reads < 100) begin $display("FAIL: the mixer got only %0d reads", x_reads); errors = errors + 1; end

        // -- 8. how long things take -------------------------------------
        t0 = $time;
        wr(32'h7b000000, 32'hb0000000); wr(32'h7b000004, 32'h400);
        wr(32'h7b00000c, ctrl(1, 0, 1, 128));
        wait_idle(0);
        $display("tb_dma: a sector in at DIV=0: %0d cycles (the wire: %0d)", ($time - t0) / 10, 512 * 16);
        wr(32'h61000008, 32'h0000_0003);        // spi1 at DIV=0 too
        t0 = $time;
        wr(32'h7b000010, 32'h2000); wr(32'h7b000014, 32'h61000000);
        wr(32'h7b00001c, ctrl(0, 1, 2, 128));
        wait_idle(1);
        $display("tb_dma: a sector out at DIV=0: %0d cycles (the wire: %0d)", ($time - t0) / 10, 512 * 16);
        t0 = $time;
        wr(32'h7b000000, 32'h1000); wr(32'h7b000004, 32'h3000);
        wr(32'h7b00000c, ctrl(0, 0, 0, 375));
        wait_idle(0);
        $display("tb_dma: 1500 bytes copied: %0d cycles (memory waits 1-8 a word here)", ($time - t0) / 10);

        if (errors == 0) $display("tb_dma: all checks passed");
        else $display("tb_dma: %0d errors", errors);
        $finish;
    end

    initial begin
        #20000000;
        $display("FAIL: timeout");
        $finish;
    end
endmodule
