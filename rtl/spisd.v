/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Hardware SPI master for SD cards (replaces rtl/spibb.v).
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
 * -- Register map (word addressed, like every simple slave here) --
 *
 *   0  DATA    write: start a transfer, sending this byte
 *              read:  the byte received by the last transfer
 *
 *              A write while busy is IGNORED (see BUSY). Reading
 *              DATA does not start anything, so the usual
 *              full-duplex exchange is: write, poll BUSY, read.
 *
 *   1  STATUS  bit 0  BUSY   1 while a transfer is in progress
 *              bit 1  CS     current chip-select level
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

module spisd_wb #(
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

    output reg sd_ss,
    output reg sd_sck,
    output reg sd_mosi,
    input sd_miso
);

    localparam MAGIC = 32'h5350_4930;   // "SPI0"

    reg [7:0] shift_tx;      // what is left to send, MSB first
    reg [7:0] shift_rx;      // what has been received so far
    reg [7:0] data_rx;       // last completed byte, readable at DATA
    reg [3:0] bit_cnt;       // bits remaining in this transfer
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

            sd_ss <= 1'b1;          // deasserted (active low)
            sd_sck <= 1'b0;         // CPOL=0 idles low
            sd_mosi <= 1'b1;        // idle high, as MMC/SD expect
            shift_tx <= 8'hFF;
            shift_rx <= 8'h00;
            data_rx <= 8'hFF;
            bit_cnt <= 4'd0;
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
                        sd_sck <= 1'b1;
                        shift_rx <= { shift_rx[6:0], sd_miso };
                        sck_phase <= 1'b1;
                    end else begin
                        // falling edge: present the next bit, giving
                        // the card a full half period of setup before
                        // it samples on the next rising edge
                        sd_sck <= 1'b0;
                        sck_phase <= 1'b0;

                        if (bit_cnt == 4'd1) begin
                            // shift_rx already holds the whole byte:
                            // the 8th and final sample happened on the
                            // rising edge just gone. Shifting one more
                            // bit in here would drop bit 7 and append
                            // a stale MISO level.
                            busy <= 1'b0;
                            data_rx <= shift_rx;
                            sd_mosi <= 1'b1;
                            bit_cnt <= 4'd0;
                        end else begin
                            bit_cnt <= bit_cnt - 4'd1;
                            // [7], not [6]: shift_tx holds the bits
                            // still to send left-aligned, so its MSB
                            // is the next one out. Taking [6] would
                            // skip a bit.
                            sd_mosi <= shift_tx[7];
                            shift_tx <= { shift_tx[6:0], 1'b1 };
                        end
                    end
                end

            end

            // -- wishbone ------------------------------------------

            if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin

                wb_ack_o <= 1'b1;

                if (wb_we_i) begin

                    case (wb_adr_i)

                        32'd0: begin
                            // Ignored while busy rather than queued or
                            // clobbering the transfer in flight. The
                            // driver polls BUSY first; silently
                            // corrupting a byte would be worse than
                            // doing nothing.
                            if (!busy) begin
                                shift_tx <= { wb_dat_i[6:0], 1'b1 };
                                sd_mosi <= wb_dat_i[7];   // MSB first
                                shift_rx <= 8'h00;
                                bit_cnt <= 4'd8;
                                clk_cnt <= 8'd0;
                                sck_phase <= 1'b0;
                                sd_sck <= 1'b0;
                                busy <= 1'b1;
                            end
                        end

                        32'd2: begin
                            cs_assert <= wb_dat_i[0];
                            sd_ss <= ~wb_dat_i[0];   // active low
                            clk_div <= wb_dat_i[15:8];
                        end

                        default: ;

                    endcase

                end else begin

                    case (wb_adr_i)
                        32'd0: wb_dat_o <= { 24'b0, data_rx };
                        32'd1: wb_dat_o <= { 30'b0, cs_assert, busy };
                        32'd2: wb_dat_o <= { 16'b0, clk_div, 7'b0, cs_assert };
                        32'd3: wb_dat_o <= MAGIC;
                        default: wb_dat_o <= 32'b0;
                    endcase

                end

            end

        end

    end

endmodule
