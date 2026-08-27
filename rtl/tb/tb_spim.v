/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/spim.v
 *
 * The slave model is a real SPI mode 0 shift register, not a stub: it
 * samples MOSI on the rising edge and presents MISO on the falling
 * edge, exactly as an SD card does. That is the whole point -- the bug
 * this module exists to fix was a setup-time violation, so a model
 * that ignores edges would prove nothing.
 *
 * Run: iverilog -g2005 -o tb rtl/tb/tb_spim.v rtl/spim.v && ./tb
 */

`timescale 1ns / 1ps

module tb_spim;

    reg clk = 0;
    reg rst = 1;

    reg [31:0] adr = 0, dat_i = 0;
    reg we = 0, stb = 0, cyc = 0;
    reg [3:0] sel = 4'hF;
    wire [31:0] dat_o;
    wire ack;

    wire sd_ss, sd_sck, sd_mosi;
    wire sd_miso_w;
    reg sd_miso;

    spim_wb #(.DEFAULT_DIV(8'd1)) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(adr), .wb_dat_i(dat_i), .wb_dat_o(dat_o),
        .wb_we_i(we), .wb_sel_i(sel), .wb_stb_i(stb),
        .wb_ack_o(ack), .wb_cyc_i(cyc),
        .spi_cs_n(sd_ss), .spi_sck(sd_sck), .spi_mosi(sd_mosi),
        .spi_miso(sd_miso), .spi_int(1'b1)
    );

    always #5 clk = ~clk;

    // -- SPI mode 0 slave model ------------------------------------

    reg [7:0] slave_tx;        // byte the "card" will send
    reg [7:0] slave_rx;        // byte the "card" received
    reg [7:0] slave_sr;
    integer slave_bits;
    reg sck_d;

    initial begin
        slave_tx = 8'hFF; slave_rx = 8'h00; slave_sr = 8'h00;
        slave_bits = 0; sck_d = 0;
    end

    // MISO is driven COMBINATIONALLY from the current bit index rather
    // than assigned on the falling edge. Functionally the same for a
    // mode 0 slave -- the index only advances on rising edges, so the
    // level is stable across each falling edge and the whole of the
    // following half period -- but it removes any question of which
    // procedural block wins when the testbench also pokes sd_miso
    // between transfers. A model that races on MISO would fail a
    // correct master, which is worse than useless here.
    always @(*) begin
        if (sd_ss) sd_miso = 1'b1;              // released, idles high
        else sd_miso = slave_tx[7 - slave_bits];
    end

    always @(posedge clk) begin
        sck_d <= sd_sck;
        if (!sd_ss && sd_sck && !sck_d) begin
            // rising: slave samples MOSI, then advances
            slave_sr <= { slave_sr[6:0], sd_mosi };
            if (slave_bits == 7) begin
                slave_rx <= { slave_sr[6:0], sd_mosi };
                slave_bits <= 0;
            end else begin
                slave_bits <= slave_bits + 1;
            end
        end
    end

    // -- bus tasks -------------------------------------------------

    task wr(input [31:0] a, input [31:0] d);
        begin
            @(posedge clk); adr <= a; dat_i <= d; we <= 1; stb <= 1; cyc <= 1;
            @(posedge clk); while (!ack) @(posedge clk);
            stb <= 0; cyc <= 0; we <= 0; @(posedge clk);
        end
    endtask

    task rd(input [31:0] a, output [31:0] d);
        begin
            @(posedge clk); adr <= a; we <= 0; stb <= 1; cyc <= 1;
            @(posedge clk); while (!ack) @(posedge clk);
            d = dat_o;
            stb <= 0; cyc <= 0; @(posedge clk);
        end
    endtask

    reg [31:0] v;
    integer bad = 0;
    integer i;
    integer cyc_start, cyc_end;
    integer cycles;

    // full-duplex exchange, the way the driver will do it
    task xfer(input [7:0] send, input [7:0] card_sends, output [7:0] got);
        begin
            slave_tx = card_sends;
            wr(32'd0, send);
            rd(32'd1, v);
            while (v[0]) rd(32'd1, v);      // poll BUSY
            rd(32'd0, v);
            got = v[7:0];
        end
    endtask

    reg [7:0] got;

    initial begin
        repeat (4) @(posedge clk);
        rst = 0;
        repeat (2) @(posedge clk);

        rd(32'd3, v);
        $display("MAGIC = %08x (want 53504930)", v);
        if (v !== 32'h5350_4930) bad = bad + 1;

        // CS deasserted out of reset
        $display("reset: sd_ss=%b sd_sck=%b (want 1, 0)", sd_ss, sd_sck);
        if (sd_ss !== 1'b1 || sd_sck !== 1'b0) bad = bad + 1;

        // assert CS, fastest clock
        wr(32'd2, 32'h0000_0001);
        repeat (2) @(posedge clk);
        $display("after CS assert: sd_ss=%b (want 0)", sd_ss);
        if (sd_ss !== 1'b0) bad = bad + 1;

        // -- exchange a set of byte patterns both directions --------
        $display("");
        $display("-- full duplex exchange --");
        xfer(8'hA5, 8'h5A, got);
        $display("  sent A5 -> card got %02x (want A5) | card sent 5A -> got %02x (want 5A)",
            slave_rx, got);
        if (slave_rx !== 8'hA5) bad = bad + 1;
        if (got !== 8'h5A) bad = bad + 1;

        xfer(8'hFF, 8'h00, got);
        $display("  sent FF -> card got %02x | card sent 00 -> got %02x", slave_rx, got);
        if (slave_rx !== 8'hFF || got !== 8'h00) bad = bad + 1;

        xfer(8'h00, 8'hFF, got);
        $display("  sent 00 -> card got %02x | card sent FF -> got %02x", slave_rx, got);
        if (slave_rx !== 8'h00 || got !== 8'hFF) bad = bad + 1;

        xfer(8'h01, 8'h80, got);
        $display("  sent 01 -> card got %02x | card sent 80 -> got %02x", slave_rx, got);
        if (slave_rx !== 8'h01 || got !== 8'h80) bad = bad + 1;

        xfer(8'h40, 8'hFE, got);   // CMD0, then a data token
        $display("  sent 40 -> card got %02x | card sent FE -> got %02x", slave_rx, got);
        if (slave_rx !== 8'h40 || got !== 8'hFE) bad = bad + 1;

        // -- every value, both directions ---------------------------
        $display("");
        $display("-- exhaustive: all 256 values --");
        for (i = 0; i < 256; i = i + 1) begin
            xfer(i[7:0], ~i[7:0], got);
            if (slave_rx !== i[7:0]) begin
                $display("  FAIL tx %02x -> card saw %02x", i[7:0], slave_rx);
                bad = bad + 1;
            end
            if (got !== ~i[7:0]) begin
                $display("  FAIL rx: card sent %02x -> got %02x", ~i[7:0], got);
                bad = bad + 1;
            end
        end
        $display("  256 values exchanged both directions");

        // -- writes while busy are ignored, not corrupting -----------
        $display("");
        $display("-- write while busy is ignored --");
        slave_tx = 8'h3C;
        wr(32'd0, 8'hC3);
        wr(32'd0, 8'h0F);          // should be dropped
        rd(32'd1, v);
        while (v[0]) rd(32'd1, v);
        $display("  card received %02x (want C3, not 0F)", slave_rx);
        if (slave_rx !== 8'hC3) bad = bad + 1;

        // -- clock divider actually changes SCLK --------------------
        $display("");
        $display("-- clock divider --");
        wr(32'd2, { 16'h0, 8'd0, 8'h01 });    // DIV=0 -> fastest
        cyc_start = $time;
        xfer(8'hAA, 8'h55, got);
        cyc_end = $time;
        cycles = (cyc_end - cyc_start) / 10;
        $display("  DIV=0: %0d clk for one byte", cycles);

        wr(32'd2, { 16'h0, 8'd9, 8'h01 });    // DIV=9 -> 10x slower
        cyc_start = $time;
        xfer(8'hAA, 8'h55, got);
        cyc_end = $time;
        $display("  DIV=9: %0d clk for one byte", (cyc_end - cyc_start) / 10);
        if (((cyc_end - cyc_start) / 10) <= cycles) begin
            $display("  FAIL: divider had no effect");
            bad = bad + 1;
        end

        // -- deassert CS --------------------------------------------
        wr(32'd2, 32'h0000_0000);
        repeat (2) @(posedge clk);
        if (sd_ss !== 1'b1) bad = bad + 1;

        $display("");
        $display("=====================================");
        $display(" errors : %0d", bad);
        $display(" RESULT : %s", bad ? "FAIL" : "PASS");
        $display("=====================================");
        if (bad) $stop;
        $finish;
    end

    initial begin
        #20_000_000;
        $display("FAIL: timeout");
        $stop;
    end

endmodule
