/*
 * Testbench for rtl/spim.v.
 *
 * Checks the two things version 1 added -- the DATA stall and 32-bit
 * transfers -- and, just as importantly, that 8-bit transfers still
 * behave exactly as they did, since every existing driver and every
 * shipped bitstream depends on that.
 *
 * The slave model is SPI mode 0 to match the master: it presents a bit
 * on the falling edge of SCK and samples MOSI on the rising edge, with
 * the first bit presented when CS asserts. That ordering is the whole
 * point of the test -- a byte-order or off-by-one-bit error in the
 * wide path is invisible in a register dump and shows up as a
 * corrupted sector.
 *
 *   iverilog -o /tmp/tb_spim rtl/tb/tb_spim.v rtl/spim.v && /tmp/tb_spim
 */

`timescale 1ns / 1ps

module tb_spim;

    reg clk = 0;
    reg rst = 1;
    always #5 clk = ~clk;           // 100MHz-ish; the divider is what matters

    reg [31:0] adr = 0, dat_w = 0;
    wire [31:0] dat_r;
    reg we = 0, stb = 0, cyc = 0;
    wire ack;

    wire cs_n, sck, mosi;
    reg miso = 1;

    integer errors = 0;

    spim_wb #(.DEFAULT_DIV(8'd1)) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
        .wb_we_i(we), .wb_sel_i(4'hF), .wb_stb_i(stb), .wb_ack_o(ack),
        .wb_cyc_i(cyc),
        .spi_cs_n(cs_n), .spi_sck(sck), .spi_mosi(mosi), .spi_miso(miso),
        .spi_int(1'b1)
    );

    /* -- slave model, SPI mode 0 -- */
    reg [31:0] slave_tx = 0;        // what the slave will send, MSB first
    reg [31:0] slave_rx = 0;        // what the slave has received
    reg [31:0] slave_sr = 0;
    reg sck_d = 0;

    /* Stages the next pattern without toggling CS -- sdmm.c holds CS
     * low across many transfers, so the model needs to be reloadable
     * mid-session.
     *
     * Written on the NEGATIVE edge, and that is not a stylistic
     * choice. The first version pulsed a `slave_reload` flag that the
     * posedge always block read, and it never fired: the task cleared
     * the flag at the same posedge the model sampled it, and Verilog
     * does not order a procedural block against an always block at
     * the same edge. The symptom was MISO stuck at 0 and every receive
     * check failing while every transmit check passed -- which reads
     * exactly like a broken receive path in the DUT.
     *
     * Driving the state directly on the opposite edge has no race to
     * lose. */
    task slave_set(input [31:0] pattern);
        begin
            @(negedge clk);
            slave_tx = pattern;
            slave_sr = pattern;
            slave_rx = 0;
            miso = pattern[31];
        end
    endtask

    always @(posedge clk) begin
        sck_d <= sck;
        if (cs_n) begin
            slave_sr <= slave_tx;
            miso <= slave_tx[31];
        end else begin
            if (sck && !sck_d) begin            // rising: sample MOSI
                slave_rx <= { slave_rx[30:0], mosi };
            end
            if (!sck && sck_d) begin            // falling: present next
                slave_sr <= { slave_sr[30:0], 1'b1 };
                miso <= slave_sr[30];
            end
        end
    end

    /* -- wishbone helpers --
     *
     * wb_write/wb_read wait for ack rather than assuming one, which is
     * what makes the stall testable at all: a cycle count would pass
     * whether or not the ack was withheld. */
    task wb_write(input [31:0] a, input [31:0] d);
        begin
            @(posedge clk);
            adr <= a; dat_w <= d; we <= 1; stb <= 1; cyc <= 1;
            @(posedge clk);
            while (!ack) @(posedge clk);
            stb <= 0; cyc <= 0; we <= 0;
            @(posedge clk);
        end
    endtask

    task wb_read(input [31:0] a, output [31:0] d);
        begin
            @(posedge clk);
            adr <= a; we <= 0; stb <= 1; cyc <= 1;
            @(posedge clk);
            while (!ack) @(posedge clk);
            d = dat_r;
            stb <= 0; cyc <= 0;
            @(posedge clk);
        end
    endtask

    /* Counts how many clocks a wishbone access took, so that "the ack
     * was withheld" is an observation and not an assumption. */
    integer stall_cycles;
    task wb_write_timed(input [31:0] a, input [31:0] d);
        begin
            stall_cycles = 0;
            @(posedge clk);
            adr <= a; dat_w <= d; we <= 1; stb <= 1; cyc <= 1;
            @(posedge clk);
            while (!ack) begin stall_cycles = stall_cycles + 1; @(posedge clk); end
            stb <= 0; cyc <= 0; we <= 0;
            @(posedge clk);
        end
    endtask

    task check(input [255:0] name, input [31:0] got, input [31:0] want);
        begin
            if (got !== want) begin
                $display("FAIL %0s: got %08x want %08x", name, got, want);
                errors = errors + 1;
            end else begin
                $display("ok   %0s = %08x", name, got);
            end
        end
    endtask

    reg [31:0] v;

    initial begin
        repeat (4) @(posedge clk);
        rst <= 0;
        repeat (4) @(posedge clk);

        /* -- the version register -- */
        wb_read(32'd3, v);
        check("MAGIC", v, 32'h5350_4931);

        /* -- CTRL: assert CS, DIV=1, 8-bit -- */
        wb_write(32'd2, 32'h0000_0101);
        wb_read(32'd2, v);
        check("CTRL readback", v, 32'h0000_0101);

        /* -- 8-bit transfer, the compatibility case --
         *
         * Send 0x5A, expect to receive the slave's first byte. */
        slave_set(32'hA5_00_00_00);
        wb_write(32'd0, 32'h0000_005A);
        wb_read(32'd0, v);                      // stalls until done
        check("8-bit rx", v, 32'h0000_00A5);
        check("8-bit tx (slave saw)", slave_rx[7:0], 32'h0000_005A);

        /* -- the stall itself --
         *
         * A DATA write immediately after another must be held off
         * until the first transfer finishes. At DIV=1 a byte is 8 bits
         * x 2 half-periods x 2 cycles = 32 cycles, so an ack that
         * arrives in fewer than ~20 means the stall is not working. */
        slave_set(32'h3C_00_00_00);
        wb_write(32'd0, 32'h0000_00F0);         // starts a transfer
        wb_write_timed(32'd0, 32'h0000_00F0);   // must wait for it
        if (stall_cycles < 20) begin
            $display("FAIL stall: second DATA write acked after only %0d cycles",
                     stall_cycles);
            errors = errors + 1;
        end else begin
            $display("ok   stall: second DATA write waited %0d cycles",
                     stall_cycles);
        end

        /* Drain the second transfer before moving on.
         *
         * It is still in flight -- the write started it and nothing
         * has read the result. Reloading the slave model on top of a
         * transfer in progress corrupts the NEXT test's data, which
         * is how this was found: the 32-bit receive check failed with
         * a plausible-looking wrong value while every other check
         * passed. A driver has the same obligation. */
        wb_read(32'd0, v);

        /* -- 32-bit transfer, and the byte order --
         *
         * The word written is 0x44332211. Byte 0 of a little-endian
         * buffer is 0x11, and 0x11 must go out FIRST. The slave
         * receives MSB-first into slave_rx, so after 32 bits it should
         * hold 0x11223344 -- the reverse of the word written, which is
         * exactly right and is the assertion that catches a lane
         * mistake.
         */
        wb_write(32'd2, 32'h0000_0103);         // CS on, XFER32, DIV=1
        wb_read(32'd2, v);
        check("CTRL XFER32 readback", v, 32'h0000_0103);

        slave_set(32'hDE_AD_BE_EF);
        wb_write(32'd0, 32'h4433_2211);
        wb_read(32'd0, v);

        check("32-bit tx (slave saw)", slave_rx, 32'h1122_3344);

        /* Received MSB-first: 0xDE arrived first, so it must appear at
         * bits 7:0 -- byte 0 of the caller's buffer. */
        check("32-bit rx (byte order)", v, 32'hEFBE_ADDE);

        /* -- back to 8-bit, to prove the mode is not sticky -- */
        wb_write(32'd2, 32'h0000_0101);
        slave_set(32'h7E_00_00_00);
        wb_write(32'd0, 32'h0000_0001);
        wb_read(32'd0, v);
        check("8-bit again", v, 32'h0000_007E);

        $display("");
        if (errors == 0) $display("tb_spim: all checks passed");
        else $display("tb_spim: %0d FAILURES", errors);
        $finish;
    end

    initial begin
        #2000000;
        $display("tb_spim: TIMEOUT -- a stalled access never acked?");
        $finish;
    end

endmodule
