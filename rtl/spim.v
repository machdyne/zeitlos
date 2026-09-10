/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Hardware SPI master (replaces rtl/spibb.v and rtl/spibb_eth.v).
 *
 * One module, instantiated once per device: the SD card and the
 * ENC28J60 ethernet controller both hang off plain SPI and differ only
 * in clock rate and whether they have an interrupt line. Two copies of
 * a shift register would be two places to fix the next bug.
 *
 * -- Why this exists --
 *
 * spibb.v exposed the four SPI pins as GPIO and left the software to
 * toggle them. That made the SPI clock rate an emergent property of
 * compiler codegen: every SCLK edge cost one or more wishbone cycles,
 * so SCLK ran at whatever speed the generated store loop happened to
 * execute at.
 *
 * That is not a theoretical objection. The rate changed when the
 * toolchain moved from GCC 8.2/rv32i to GCC 15.2/rv32im, it would
 * change again the moment an instruction cache is enabled on a board
 * (rtl/cache.v), and it changed when a read-modify-write that looked
 * redundant was removed from the driver -- that read was a full
 * wishbone cycle, and deleting it roughly doubled SCLK and removed the
 * card's data setup time at the same time.
 *
 * Here SCLK is a divider off wb_clk_i. It is the same on every board,
 * at every optimisation level, under every compiler, forever. Setup
 * and hold are correct by construction rather than by accident.
 *
 * Speed is the secondary benefit, but it is large: bit-banging costs
 * roughly 100 CPU cycles per BIT, this costs roughly 48 cycles per
 * BYTE, which is about an 18x improvement.
 *
 * -- Version 1: stall-on-DATA, and 32-bit transfers --
 *
 * `sdbench` on hardware (docs/sdcard.md) measured the SD path at 190
 * CPU cycles per byte against 32 cycles of actual wire time at DIV=1.
 * Layers above this module -- the card, the command protocol, FatFs --
 * accounted for under 15% between them. Essentially all of the cost
 * was software crossing the wishbone bus three or more times per byte:
 * a write to DATA, a poll loop over STATUS, and a read of DATA.
 *
 * Two additions remove most of that, neither of which needs a bus
 * master, an arbiter port, or a single bit of block RAM:
 *
 *   1. A DATA access STALLS while busy instead of being ignored (write)
 *      or returning stale data (read). The driver no longer polls at
 *      all: `write DATA; read DATA` is a complete exchange.
 *
 *   2. A transfer can be 32 bits instead of 8 (CTRL bit 1). One bus
 *      access moves four bytes.
 *
 * Both are additive and default off, so an unmodified driver behaves
 * exactly as before -- but a driver that uses them against an OLD
 * bitstream would be silently wrong, so MAGIC is now "SPI1" and
 * software must check it before taking the fast path. See
 * Z_SPISD_MAGIC in sw/common/zeitlos.h.
 *
 * -- What stalling costs --
 *
 * The arbiter holds a grant for a whole transaction (rtl/arbiter_main.v),
 * so a stalled access holds the main bus for the duration of the
 * transfer: 32 cycles for a byte at DIV=1, 128 for a 32-bit word, and
 * proportionally more at a larger divider. The CPU previously spun in
 * a poll loop and held the bus nearly as hard, so this is closer to
 * honest accounting than to a regression.
 *
 * The case to keep in mind is card INITIALISATION, which runs at
 * DEFAULT_DIV (59): a byte there stalls the bus for ~960 cycles. The
 * audio mixer wants eight words per sample period (~1090 cycles at
 * 44.1kHz), so a stall that long could starve it. Initialisation
 * happens once, at boot, before anything is playing -- but a future
 * caller that drops the clock back down while audio is running would
 * be the one to watch.
 *
 * -- Register map (word addressed, like every simple slave here) --
 *
 *   0  DATA    write: start a transfer, sending this byte (or word,
 *                     if CTRL.XFER32 is set)
 *              read:  what the last transfer received
 *
 *              A DATA access while busy STALLS -- the wishbone ack is
 *              withheld until the shift register is free. So the whole
 *              full-duplex exchange is `write DATA; read DATA`, with
 *              no polling, and back-to-back writes need no polling
 *              either.
 *
 *              STATUS.BUSY is still there and still correct, for a
 *              driver that wants to look without blocking, and for
 *              every existing user of this module.
 *
 *   1  STATUS  bit 0  BUSY   1 while a transfer is in progress
 *              bit 1  CS     current chip-select level
 *              bit 2  INT    device interrupt pin (active low, so 0
 *                            means the device is asserting it)
 *              others reserved, read 0
 *
 *   2  CTRL    bit 0  CS     chip select. Write 1 to ASSERT (drives
 *                            the pin LOW, since SD /CS is active low).
 *                            Resets to 0 = deasserted.
 *              bit 8..15 DIV clock divider, see below. Resets to
 *                            DEFAULT_DIV.
 *
 *   3  MAGIC   fixed 32'h5350_4930 ("SPI0"), so software can tell
 *              this apart from spibb.v on an older bitstream -- an
 *              undecoded read does not fault on this bus, it returns
 *              whatever the mux resolves to.
 *
 * -- Clock divider --
 *
 * SCLK = wb_clk_i / (2 * (DIV + 1)).
 *
 * At 48MHz: DIV=59 gives 400kHz (required during card init), DIV=1
 * gives 12MHz, DIV=0 gives 24MHz. Cards must be clocked at 400kHz or
 * below until they leave idle state, which is why this is runtime
 * settable rather than a parameter.
 *
 * -- SPI mode 0 --
 *
 * CPOL=0, CPHA=0, MSB first, which is what SD cards require in SPI
 * mode. MOSI is set on the falling edge and MISO is sampled on the
 * rising edge, giving the card a full half-period of setup time --
 * the thing the bit-banged driver kept losing.
 */

module spim_wb #(
    // Divider used out of reset. 59 -> 400kHz at 48MHz, which is safe
    // for card initialisation. Software raises the clock after the
    // card reports ready.
    parameter DEFAULT_DIV = 8'd59
) (
    input wb_clk_i,
    input wb_rst_i,
    input [31:0] wb_adr_i,
    input [31:0] wb_dat_i,
    output reg [31:0] wb_dat_o,
    input wb_we_i,
    input [3:0] wb_sel_i,
    input wb_stb_i,
    output reg wb_ack_o,
    input wb_cyc_i,

    output reg spi_cs_n,
    output reg spi_sck,
    output reg spi_mosi,
    input spi_miso,

    // Device interrupt line, readable in STATUS bit 2. The ENC28J60
    // has one (active low); the SD card does not, and ties it high.
    // Exposed rather than wired to the CPU's interrupt controller
    // because the driver polls it -- one register read is far cheaper
    // than an SPI transaction asking the chip whether it has a packet.
    input spi_int
);

    // "SPI1", not "SPI0". Bumped because the stall and the 32-bit mode
    // are things software must not assume: a driver that skips the
    // BUSY poll against a bitstream that does not stall reads DATA
    // mid-transfer and gets a byte that is half old and half new. A
    // version register exists precisely so that pairing cannot go
    // wrong silently -- see sdmm.c's own check.
    localparam MAGIC = 32'h5350_4931;   // "SPI1"

    // 32 bits wide so that one transfer can move four bytes. In 8-bit
    // mode only the top byte of shift_tx and the bottom byte of
    // shift_rx are used, which is exactly what the 8-bit version did.
    //
    // Cost of the widening: about seventy flip-flops and a handful of
    // LUTs. No block RAM -- a FIFO would need one and is not what this
    // needs. What buys the speed is fewer BUS ACCESSES per byte, not
    // buffering.
    reg [31:0] shift_tx;     // what is left to send, MSB first
    reg [31:0] shift_rx;     // what has been received so far
    reg [31:0] data_rx;      // what the last transfer received
    reg [5:0] bit_cnt;       // bits remaining in this transfer
    reg xfer32;              // 1 = 32-bit transfers, 0 = 8-bit
    reg [7:0] clk_div;
    reg [7:0] clk_cnt;
    reg busy;
    reg cs_assert;
    reg sck_phase;           // 0 = next edge is rising, 1 = falling

    // Tick once per SCLK half-period. Held at zero while idle so a new
    // transfer always begins with a full half-period before the first
    // edge, rather than however much of one happened to be left over.
    wire tick = (clk_cnt == clk_div);

    always @(posedge wb_clk_i) begin

        if (wb_rst_i) begin

            spi_cs_n <= 1'b1;          // deasserted (active low)
            spi_sck <= 1'b0;         // CPOL=0 idles low
            spi_mosi <= 1'b1;        // idle high, as MMC/SD expect
            shift_tx <= 32'hFFFFFFFF;
            shift_rx <= 32'h0;
            data_rx <= 32'hFF;
            bit_cnt <= 6'd0;
            xfer32 <= 1'b0;
            clk_div <= DEFAULT_DIV;
            clk_cnt <= 8'd0;
            busy <= 1'b0;
            cs_assert <= 1'b0;
            sck_phase <= 1'b0;
            wb_ack_o <= 1'b0;
            wb_dat_o <= 32'b0;

        end else begin

            wb_ack_o <= 1'b0;

            // -- shift engine --------------------------------------

            if (busy) begin

                if (!tick) begin
                    clk_cnt <= clk_cnt + 8'd1;
                end else begin
                    clk_cnt <= 8'd0;

                    if (!sck_phase) begin
                        // rising edge: the card has had a full half
                        // period to drive MISO, so sample it here
                        spi_sck <= 1'b1;
                        shift_rx <= { shift_rx[30:0], spi_miso };
                        sck_phase <= 1'b1;
                    end else begin
                        // falling edge: present the next bit, giving
                        // the card a full half period of setup before
                        // it samples on the next rising edge
                        spi_sck <= 1'b0;
                        sck_phase <= 1'b0;

                        if (bit_cnt == 6'd1) begin
                            // shift_rx already holds the whole
                            // transfer: the final sample happened on
                            // the rising edge just gone. Shifting one
                            // more bit in here would drop the first
                            // bit and append a stale MISO level.
                            busy <= 1'b0;
                            spi_mosi <= 1'b1;
                            bit_cnt <= 6'd0;

                            // Byte order.
                            //
                            // Bits arrive MSB-first, so after a 32-bit
                            // transfer the FIRST byte off the wire is
                            // in shift_rx[31:24]. The CPU is
                            // little-endian and will store this word
                            // to a buffer with bits 7:0 landing at
                            // byte 0 -- so the lanes are reversed here
                            // to put the first byte received where a
                            // byte-at-a-time driver would have put it.
                            //
                            // Doing this in software instead would
                            // give back a good part of what the wide
                            // transfer just bought: four shifts and
                            // three ors per word, on a machine where
                            // that is most of the cost.
                            if (xfer32)
                                data_rx <= { shift_rx[7:0], shift_rx[15:8],
                                             shift_rx[23:16], shift_rx[31:24] };
                            else
                                data_rx <= { 24'b0, shift_rx[7:0] };
                        end else begin
                            bit_cnt <= bit_cnt - 6'd1;
                            // [31], not [30]: shift_tx holds the bits
                            // still to send left-aligned, so its MSB
                            // is the next one out. Taking [30] would
                            // skip a bit.
                            spi_mosi <= shift_tx[31];
                            shift_tx <= { shift_tx[30:0], 1'b1 };
                        end
                    end
                end

            end

            // -- wishbone ------------------------------------------

            // A DATA access while busy is STALLED: no ack until the
            // shift register is free. Every other register acks
            // immediately, as before -- STATUS in particular has to,
            // or a driver polling BUSY would deadlock against the very
            // condition it is waiting for.
            //
            // Bounded by construction: the longest possible transfer
            // is 32 bits at the largest divider, 32 * 2 * 256 = 16384
            // cycles. It cannot hang.
            if (wb_cyc_i && wb_stb_i && !wb_ack_o &&
                !(wb_adr_i == 32'd0 && busy)) begin

                wb_ack_o <= 1'b1;

                if (wb_we_i) begin

                    case (wb_adr_i)

                        32'd0: begin
                            // Cannot be reached while busy -- the ack
                            // is withheld above until the shift
                            // register is free -- so the transfer in
                            // flight can never be clobbered. The
                            // `if (!busy)` that used to guard this is
                            // gone with the condition it tested.
                            shift_rx <= 32'h0;
                            clk_cnt <= 8'd0;
                            sck_phase <= 1'b0;
                            spi_sck <= 1'b0;
                            busy <= 1'b1;

                            if (xfer32) begin
                                // Lanes reversed for the same reason
                                // as on receive: byte 0 of the
                                // caller's buffer is in bits 7:0, and
                                // it has to go out first.
                                spi_mosi <= wb_dat_i[7];
                                shift_tx <= { wb_dat_i[6:0],
                                              wb_dat_i[15:8],
                                              wb_dat_i[23:16],
                                              wb_dat_i[31:24],
                                              1'b1 };
                                bit_cnt <= 6'd32;
                            end else begin
                                spi_mosi <= wb_dat_i[7];   // MSB first
                                shift_tx <= { wb_dat_i[6:0], 25'h1FFFFFF };
                                bit_cnt <= 6'd8;
                            end
                        end

                        32'd2: begin
                            cs_assert <= wb_dat_i[0];
                            spi_cs_n <= ~wb_dat_i[0];   // active low
                            xfer32 <= wb_dat_i[1];
                            clk_div <= wb_dat_i[15:8];
                        end

                        default: ;

                    endcase

                end else begin

                    case (wb_adr_i)
                        32'd0: wb_dat_o <= data_rx;
                        32'd1: wb_dat_o <= { 29'b0, spi_int, cs_assert, busy };
                        32'd2: wb_dat_o <= { 16'b0, clk_div, 6'b0,
                                             xfer32, cs_assert };
                        32'd3: wb_dat_o <= MAGIC;
                        default: wb_dat_o <= 32'b0;
                    endcase

                end

            end

        end

    end

endmodule
