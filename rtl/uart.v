/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * UART -- a 16550-compatible serial port.
 *
 * -- Provenance --
 *
 * Written from scratch against the 16550 REGISTER INTERFACE and
 * against what this tree's software actually does with it
 * (sw/bios/bios.c's uart_init()/putchar()/getchar(), sw/os/uart.c's
 * ISR, sw/common/zuart.c's UART1 API). No code, structure or naming
 * is taken from rtl/ext/uart16550, which it replaces -- that core is
 * LGPL and this one is under the same license as the rest of this
 * repo. The compatibility is with the programming model, which is a
 * published hardware interface, not with an implementation.
 *
 * Drop-in for uart_top: same wishbone port names, same register
 * offsets, same bit meanings in every field this tree reads or
 * writes. Nothing in sw/ changes.
 *
 * -- Why replace a working core --
 *
 * Not for speed. rtl/ext/uart16550 was never on the critical path on
 * any board here -- it does not appear in a single nextpnr critical
 * path report, and the timing differences between builds with and
 * without it are placement variance on a design whose real
 * constraints are rtl/mtu.v's address adder and the GPU scanout
 * cluster.
 *
 * For AREA, and for the license. Measured standalone on ECP5 with
 * -abc9, uart_top is 583 LUT4 / 308 FF / 4 LUTRAM, which is a great
 * deal for 8N1 at one rate. Most of it is machinery this tree never
 * uses: a debug register interface, a full modem control block whose
 * four inputs rtl/sysctl.v ties to 1'b1, per-FIFO-entry error bits
 * three deep, and selectable receive trigger levels nothing selects.
 *
 * This block implements what is used and acknowledges the rest.
 *
 * -- What is implemented --
 *
 *   0x00  RBR / THR / DLL    receive, transmit, divisor low
 *   0x04  IER / DLM          interrupt enable, divisor high
 *   0x08  IIR (r) / FCR (w)  interrupt id, FIFO control
 *   0x0c  LCR                word length, stop bits, parity, DLAB
 *   0x14  LSR                line status
 *
 * 16-byte transmit and receive FIFOs, 5-8 data bits, 1 or 2 stop
 * bits, none/even/odd parity, and a 16x oversampling receiver -- the
 * whole of what sw/common/zuart.c's z_uart1_config() can ask for.
 *
 * -- What is NOT implemented, and why that is safe --
 *
 * MCR (0x10), MSR (0x18) and SCR (0x1c) are DECODED AND ACKED but
 * read back zero and discard writes. There are no modem lines on any
 * board here -- rtl/sysctl.v tied cts/dsr/ri/dcd to 1'b1 on the old
 * core, so MSR was a constant already -- and nothing in sw/ reads any
 * of the three.
 *
 * Acking them is not optional and is the one thing that must not be
 * "optimised" later. An address nothing decodes gets no ack on this
 * bus and the CPU waits for it forever; see rtl/uart_null.v's header
 * for the same hazard discovered the hard way. Every offset in the
 * window answers.
 *
 * Receive trigger level (FCR bits 7:6) is accepted and ignored: the
 * level is always 1 byte. sw/bios/bios.c and sw/os/uart.c both write
 * FCR = 0b111, which selects 1 byte anyway, and a build that asked
 * for 14 would get more interrupts than it wanted rather than fewer
 * -- the safe direction. Because the level is 1, the character
 * timeout interrupt (IIR 0x0c) can never be needed and is not
 * generated; sw/os/uart.c handles it identically to RDA in any case.
 *
 * Break GENERATION (LCR bit 6) is not implemented. Break DETECTION
 * is -- sw/common/zuart.c reads LSR's BI bit.
 *
 * Receiver line status and modem interrupts (IER bits 2 and 3) are
 * stored and read back but never assert int_o. Nothing sets them.
 *
 * -- One deliberate deviation from the real part --
 *
 * PE, FE and BI are STICKY GLOBAL bits cleared when LSR is read,
 * rather than travelling with each byte through the receive FIFO.
 *
 * The real 16550 tags every FIFO entry with its own three error bits,
 * so LSR describes the byte at the head of the FIFO. That is three
 * extra bits of storage on every entry and a mux to go with them, and
 * it buys the ability to say WHICH byte in a burst was corrupt.
 * Nothing here wants to know that: sw/os/uart.c reads LSR bit 7,
 * discards one byte and returns, and sw/common/zuart.c accumulates
 * the bits into its own err_sticky word precisely because it is
 * polling and expects to catch errors between reads rather than per
 * byte (see lsr_poll() there).
 *
 * So the semantics are those of the earlier 16450: an error sets the
 * bit, the bit stays set until somebody reads LSR, and bit 7 is the
 * OR of the three. Software that polls sees every error. Software
 * that wanted to attribute one to a particular byte cannot -- and
 * none exists here.
 *
 * -- Divisor --
 *
 * Baud is wb_clk_i / (16 * divisor), the 16550's own arithmetic,
 * which is what sw/common/zuart.c's baud_div() computes against
 * Z_SYSCLK_HZ. A divisor of 0 is treated as 1 rather than dividing by
 * zero and stopping the bit clock -- the real part's behaviour there
 * is undefined and a dead transmitter is a bad way to find out that
 * DLL was never written.
 */

`default_nettype none

module uart_wb #(
	// Receive and transmit FIFO depth, entries. Must be a power of
	// two, and 16 is what the 16550 has -- sw/common/zuart.h's header
	// does arithmetic with that number to work out what a polled
	// reader can sustain, so changing it makes that documentation
	// wrong rather than merely stale.
	parameter FIFO_DEPTH = 16,
	parameter FIFO_BITS = 4
)
(
	input wire wb_clk_i,
	input wire wb_rst_i,

	// Word-addressed, like rtl/uart_null.v and rtl/usb_cdc_uart.v:
	// rtl/sysctl.v passes wbm_adr_sel_word, so register n is at byte
	// offset 4n, which is where sw/common/zeitlos.h's reg_uart*_
	// macros put it.
	//
	// Taken at full width rather than as the 3 bits actually used.
	// uart_top declared 3, so every build resized the port and yosys
	// said so once per instance; the upper bits are free to ignore
	// here and the log is quieter for it. Same for the data ports,
	// which uart_top declared as 8 bits -- leaving the top 24 bits of
	// rtl/sysctl.v's wbs_uart0_dat_o undriven.
	input wire [25:0] wb_adr_i,
	input wire [31:0] wb_dat_i,
	output wire [31:0] wb_dat_o,
	input wire wb_we_i,
	input wire [3:0] wb_sel_i,
	input wire wb_stb_i,
	input wire wb_cyc_i,
	output reg wb_ack_o,

	output wire stx_pad_o,
	input wire srx_pad_i,

	output wire int_o
);

	// -- register indices, as WORDS --
	localparam [2:0] REG_RBR = 3'd0;	// 0x00  RBR / THR / DLL
	localparam [2:0] REG_IER = 3'd1;	// 0x04  IER / DLM
	localparam [2:0] REG_IIR = 3'd2;	// 0x08  IIR (r) / FCR (w)
	localparam [2:0] REG_LCR = 3'd3;	// 0x0c  LCR
	localparam [2:0] REG_LSR = 3'd5;	// 0x14  LSR

	// -- transmit engine states --
	localparam [2:0] TX_IDLE = 3'd0;
	localparam [2:0] TX_START = 3'd1;
	localparam [2:0] TX_DATA = 3'd2;
	localparam [2:0] TX_PARITY = 3'd3;
	localparam [2:0] TX_STOP1 = 3'd4;
	localparam [2:0] TX_STOP2 = 3'd5;

	// -- receive engine states --
	localparam [2:0] RX_IDLE = 3'd0;
	localparam [2:0] RX_START = 3'd1;
	localparam [2:0] RX_DATA = 3'd2;
	localparam [2:0] RX_PARITY = 3'd3;
	localparam [2:0] RX_STOP = 3'd4;

	// -- register file --
	//
	// All declarations at module scope. House rule for this tree
	// (plain Verilog, no local declarations) and not merely style:
	// the GateMate flow reads these files through `read -sv` and the
	// ice40/ecp5 ones do not, so anything needing SystemVerilog
	// scoping builds on one target and not the others.
	reg [7:0] reg_ier;
	reg [7:0] reg_lcr;
	reg [7:0] reg_dll;
	reg [7:0] reg_dlm;
	reg fifo_en;

	reg [31:0] dat_r;

	// -- line status, the sticky half --
	//
	// See the header for why these are global and read-to-clear
	// rather than per-FIFO-entry.
	reg err_oe;
	reg err_pe;
	reg err_fe;
	reg err_bi;

	reg thre_int;
	reg tx_empty_d;
	reg ier1_d;

	// -- FIFOs --
	//
	// Pointers carry one bit more than the depth needs, so that full
	// and empty are distinguishable without a separate count: equal
	// pointers are empty, equal low bits with differing top bits are
	// full. Reads are asynchronous, which is what makes these infer
	// as distributed LUT RAM rather than as block RAM -- a DP16KD for
	// sixteen bytes would be an absurd trade on a board that has four
	// of them left.
	reg [7:0] rx_fifo [0:FIFO_DEPTH-1];
	reg [FIFO_BITS:0] rx_wr;
	reg [FIFO_BITS:0] rx_rd;

	reg [7:0] tx_fifo [0:FIFO_DEPTH-1];
	reg [FIFO_BITS:0] tx_wr;
	reg [FIFO_BITS:0] tx_rd;

	wire rx_empty;
	wire rx_full;
	wire tx_empty;
	wire tx_full;
	wire [7:0] rx_head;
	wire [7:0] tx_head;

	// -- baud generator --
	reg [15:0] baud_cnt;
	wire [15:0] divisor;
	wire tick16;

	// -- transmit engine --
	reg [2:0] tx_state;
	reg [3:0] tx_phase;
	reg [2:0] tx_bit;
	reg [7:0] tx_sr;
	reg tx_par;
	reg tx_line;

	// -- receive engine --
	reg [1:0] rx_sync;
	reg [2:0] rx_state;
	reg [3:0] rx_phase;
	reg [2:0] rx_bit;
	reg [7:0] rx_sr;
	reg rx_par;
	reg rx_any;
	reg rx_push;
	reg [7:0] rx_data;
	reg rx_err_pe;
	reg rx_err_fe;
	reg rx_err_bi;

	// -- line configuration, decoded from LCR --
	wire [2:0] word_last;
	wire parity_en;
	wire parity_even;
	wire two_stop;
	wire dlab;

	// -- bus access decode --
	//
	// bus_acc is high for exactly one cycle per transfer: wb_ack_o is
	// registered and clears the cycle after, so the !wb_ack_o term
	// makes this a single pulse even while wb_stb_i is still
	// asserted. Every side effect is gated on it, which is what stops
	// one read of RBR popping two bytes.
	wire bus_acc;
	wire bus_rd;
	wire bus_wr;
	wire [2:0] reg_sel;

	wire rbr_read;
	wire thr_write;
	wire lsr_read;
	wire iir_read;

	wire [7:0] lsr_value;
	wire [7:0] iir_value;
	wire err_any;
	wire rda_int;

	assign bus_acc = wb_cyc_i && wb_stb_i && !wb_ack_o;
	assign bus_rd = bus_acc && !wb_we_i;
	assign bus_wr = bus_acc && wb_we_i;
	assign reg_sel = wb_adr_i[2:0];

	assign dlab = reg_lcr[7];
	// LCR[1:0] is (word length - 5), so this is the index of the top
	// data bit: 4 for a 5-bit word, 7 for an 8-bit one.
	assign word_last = {1'b1, reg_lcr[1:0]};
	assign two_stop = reg_lcr[2];
	assign parity_en = reg_lcr[3];
	assign parity_even = reg_lcr[4];

	assign rbr_read = bus_rd && (reg_sel == REG_RBR) && !dlab && !rx_empty;
	assign thr_write = bus_wr && (reg_sel == REG_RBR) && !dlab;
	assign lsr_read = bus_rd && (reg_sel == REG_LSR);
	assign iir_read = bus_rd && (reg_sel == REG_IIR);

	assign rx_empty = (rx_wr == rx_rd);
	assign rx_full = (rx_wr[FIFO_BITS-1:0] == rx_rd[FIFO_BITS-1:0]) &&
		(rx_wr[FIFO_BITS] != rx_rd[FIFO_BITS]);
	assign tx_empty = (tx_wr == tx_rd);
	assign tx_full = (tx_wr[FIFO_BITS-1:0] == tx_rd[FIFO_BITS-1:0]) &&
		(tx_wr[FIFO_BITS] != tx_rd[FIFO_BITS]);

	assign rx_head = rx_fifo[rx_rd[FIFO_BITS-1:0]];
	assign tx_head = tx_fifo[tx_rd[FIFO_BITS-1:0]];

	// A divisor of zero would stop the bit clock outright. Treated as
	// one instead -- see the header.
	assign divisor = ({reg_dlm, reg_dll} == 16'd0) ? 16'd1 : {reg_dlm, reg_dll};
	assign tick16 = (baud_cnt == 16'd0);

	assign stx_pad_o = tx_line;

	// -- line status --
	//
	// THRE (bit 5) is set when the transmit FIFO is EMPTY, which is
	// the real part's contract and not the cheaper "there is room".
	// The difference matters to code this tree does not contain yet:
	// the standard 16550 idiom is to see THRE once and then write a
	// full FIFO's worth, and that is only safe if THRE means empty.
	// Reporting "room" instead would silently drop fifteen of those
	// sixteen bytes.
	//
	// Everything here writes one byte per LSR read (sw/bios/bios.c's
	// putchar(), sw/os/uart.c's tx_pump(), sw/common/zuart.c's
	// z_uart1_write()), so today the FIFO buys nothing on transmit.
	// That is a software-side improvement available later, and it is
	// available precisely BECAUSE this bit is honest.
	assign err_any = err_pe || err_fe || err_bi;
	assign lsr_value = {
		err_any,			// 7: error in receive FIFO
		tx_empty && (tx_state == TX_IDLE),	// 6: TEMT
		tx_empty,			// 5: THRE
		err_bi,				// 4: BI
		err_fe,				// 3: FE
		err_pe,				// 2: PE
		err_oe,				// 1: OE
		!rx_empty			// 0: DR
	};

	// -- interrupt identification --
	//
	// Real priority order, minus the two sources that cannot fire
	// here: receiver line status (IER bit 2) and modem status (IER
	// bit 3) are stored and never asserted, and the character timeout
	// cannot occur because the trigger level is one byte. So this is
	// RDA over THRE, and the "no interrupt pending" encoding when
	// neither is up.
	//
	// Bits 7:6 report FIFOs enabled when FCR bit 0 is set, purely so
	// this reads like the part it is imitating. sw/os/uart.c tests
	// bit 0 for "anything pending" and bits 3:1 for which, and is
	// unaffected either way.
	assign rda_int = reg_ier[0] && !rx_empty;
	assign iir_value = {
		fifo_en, fifo_en,
		4'b0000,
		rda_int ? 2'b10 : (thre_int ? 2'b01 : 2'b00),
		(rda_int || thre_int) ? 1'b0 : 1'b1
	};

	// LEVEL-SENSITIVE, like the part it replaces. rtl/sysctl.v keeps
	// bit 4 clear in LATCHED_IRQ for exactly this reason: latching a
	// level source re-fires the instant the handler returns.
	assign int_o = rda_int || (reg_ier[1] && thre_int);

	assign wb_dat_o = dat_r;

	// -- baud generator --
	//
	// One tick every `divisor` clocks; sixteen ticks to a bit. Free
	// running rather than gated on the engines being busy, so that
	// the receiver's mid-bit sampling stays aligned to the same grid
	// the transmitter uses and neither has to restart a counter at
	// the moment it is least able to afford the jitter.
	always @(posedge wb_clk_i) begin
		if (wb_rst_i)
			baud_cnt <= 16'd0;
		else if (tick16)
			baud_cnt <= divisor - 16'd1;
		else
			baud_cnt <= baud_cnt - 16'd1;
	end

	// -- transmitter --
	//
	// A state per frame field rather than a preloaded shift register
	// wide enough for the worst case. The alternative -- assembling
	// start, data, parity and one or two stop bits into a twelve-bit
	// word at load time -- needs variable shifts by an amount derived
	// from LCR, which is a barrel shifter's worth of LUTs to save a
	// three-bit state register.
	always @(posedge wb_clk_i) begin

		if (wb_rst_i) begin

			tx_state <= TX_IDLE;
			tx_phase <= 4'd0;
			tx_bit <= 3'd0;
			tx_sr <= 8'h00;
			tx_par <= 1'b0;
			tx_line <= 1'b1;
			tx_rd <= 0;

		end else if (tick16) begin

			case (tx_state)

				TX_IDLE: begin
					tx_line <= 1'b1;
					if (!tx_empty) begin
						tx_sr <= tx_head;
						tx_rd <= tx_rd + 1;
						// Parity accumulator seeded so that the
						// finished sum is even or odd as asked.
						// LCR[4] set is even parity.
						tx_par <= ~parity_even;
						tx_line <= 1'b0;
						tx_phase <= 4'd0;
						tx_bit <= 3'd0;
						tx_state <= TX_START;
					end
				end

				TX_START: begin
					tx_phase <= tx_phase + 1;
					if (tx_phase == 4'd15) begin
						tx_line <= tx_sr[0];
						tx_par <= tx_par ^ tx_sr[0];
						tx_state <= TX_DATA;
					end
				end

				TX_DATA: begin
					tx_phase <= tx_phase + 1;
					if (tx_phase == 4'd15) begin
						if (tx_bit == word_last) begin
							if (parity_en) begin
								tx_line <= tx_par;
								tx_state <= TX_PARITY;
							end else begin
								tx_line <= 1'b1;
								tx_state <= TX_STOP1;
							end
						end else begin
							tx_sr <= {1'b0, tx_sr[7:1]};
							tx_line <= tx_sr[1];
							tx_par <= tx_par ^ tx_sr[1];
							tx_bit <= tx_bit + 1;
						end
					end
				end

				TX_PARITY: begin
					tx_phase <= tx_phase + 1;
					if (tx_phase == 4'd15) begin
						tx_line <= 1'b1;
						tx_state <= TX_STOP1;
					end
				end

				TX_STOP1: begin
					tx_phase <= tx_phase + 1;
					if (tx_phase == 4'd15)
						tx_state <= two_stop ? TX_STOP2 : TX_IDLE;
				end

				TX_STOP2: begin
					tx_phase <= tx_phase + 1;
					if (tx_phase == 4'd15)
						tx_state <= TX_IDLE;
				end

				default: tx_state <= TX_IDLE;

			endcase

		end

	end

	// -- receiver --
	//
	// Two flops on srx_pad_i before anything looks at it. The line is
	// asynchronous to wb_clk_i by definition -- it is somebody else's
	// bit clock -- and a metastable sample feeding a state machine is
	// the kind of fault that shows up as one corrupt byte an hour.
	//
	// Sampling is at the middle of each bit: eight ticks from the
	// falling edge that started the frame, then every sixteen. The
	// start bit is re-checked at its own midpoint so that a glitch on
	// an idle line does not shift in a byte of noise.
	always @(posedge wb_clk_i) begin

		rx_push <= 1'b0;

		if (wb_rst_i) begin

			rx_sync <= 2'b11;
			rx_state <= RX_IDLE;
			rx_phase <= 4'd0;
			rx_bit <= 3'd0;
			rx_sr <= 8'h00;
			rx_par <= 1'b0;
			rx_any <= 1'b0;
			rx_data <= 8'h00;
			rx_err_pe <= 1'b0;
			rx_err_fe <= 1'b0;
			rx_err_bi <= 1'b0;

		end else begin

			rx_sync <= {rx_sync[0], srx_pad_i};

			if (tick16) begin

				case (rx_state)

					RX_IDLE: begin
						if (!rx_sync[1]) begin
							rx_phase <= 4'd0;
							rx_state <= RX_START;
						end
					end

					RX_START: begin
						rx_phase <= rx_phase + 1;
						if (rx_phase == 4'd7) begin
							if (rx_sync[1]) begin
								// Glitch, not a start bit.
								rx_state <= RX_IDLE;
							end else begin
								rx_phase <= 4'd0;
								rx_bit <= 3'd0;
								rx_sr <= 8'h00;
								rx_par <= ~parity_even;
								rx_any <= 1'b0;
								rx_err_pe <= 1'b0;
								rx_err_fe <= 1'b0;
								rx_state <= RX_DATA;
							end
						end
					end

					RX_DATA: begin
						rx_phase <= rx_phase + 1;
						if (rx_phase == 4'd15) begin
							// Right-justified as it arrives, LSB
							// first, so a 5-bit word lands in
							// rx_sr[4:0] with the top bits clear --
							// which is what the 16550 reports and
							// what software expects to read back.
							rx_sr <= {rx_sync[1], rx_sr[7:1]};
							rx_par <= rx_par ^ rx_sync[1];
							rx_any <= rx_any | rx_sync[1];
							rx_phase <= 4'd0;
							if (rx_bit == word_last)
								rx_state <= parity_en ? RX_PARITY : RX_STOP;
							else
								rx_bit <= rx_bit + 1;
						end
					end

					RX_PARITY: begin
						rx_phase <= rx_phase + 1;
						if (rx_phase == 4'd15) begin
							rx_err_pe <= (rx_par != rx_sync[1]);
							rx_phase <= 4'd0;
							rx_state <= RX_STOP;
						end
					end

					RX_STOP: begin
						rx_phase <= rx_phase + 1;
						if (rx_phase == 4'd15) begin
							// A stop bit read as 0 is a framing
							// error; a whole frame of zeros with it
							// is a break. Only the FIRST stop bit is
							// checked even in two-stop mode -- the
							// second carries no information a
							// receiver can act on, and waiting for it
							// would only delay resynchronising to the
							// next start edge.
							rx_err_fe <= ~rx_sync[1];
							rx_err_bi <= ~rx_sync[1] && !rx_any;
							// The byte is delivered even when it is
							// malformed. Software is told through
							// LSR and decides; silently dropping it
							// would turn a visible error into a
							// missing character.
							rx_data <= rx_sr >> (3'd7 - word_last);
							rx_push <= 1'b1;
							rx_state <= RX_IDLE;
						end
					end

					default: rx_state <= RX_IDLE;

				endcase

			end

		end

	end

	// -- FIFOs, status flags and the register file --
	//
	// One block, because these interact: a receive push and a bus
	// read of RBR can land in the same cycle and the pointer
	// arithmetic has to agree about which happened.
	always @(posedge wb_clk_i) begin

		wb_ack_o <= 1'b0;

		if (wb_rst_i) begin

			reg_ier <= 8'h00;
			reg_lcr <= 8'h00;
			reg_dll <= 8'h00;
			reg_dlm <= 8'h00;
			fifo_en <= 1'b0;
			dat_r <= 32'h0;
			rx_wr <= 0;
			rx_rd <= 0;
			tx_wr <= 0;
			err_oe <= 1'b0;
			err_pe <= 1'b0;
			err_fe <= 1'b0;
			err_bi <= 1'b0;
			thre_int <= 1'b0;
			tx_empty_d <= 1'b1;
			ier1_d <= 1'b0;

		end else begin

			// -- receive push --
			//
			// A full FIFO drops the byte and raises overrun, which is
			// the real part's behaviour: the alternative is
			// overwriting one that software has not read yet, and
			// losing the OLDEST byte of a burst is worse than losing
			// the newest because it desynchronises anything framed.
			if (rx_push) begin
				if (rx_full) begin
					err_oe <= 1'b1;
				end else begin
					rx_fifo[rx_wr[FIFO_BITS-1:0]] <= rx_data;
					rx_wr <= rx_wr + 1;
					err_pe <= err_pe | rx_err_pe;
					err_fe <= err_fe | rx_err_fe;
					err_bi <= err_bi | rx_err_bi;
				end
			end

			// -- transmit interrupt --
			//
			// EDGE, not level, and that distinction is the whole of
			// this block's correctness. Raised when the transmit FIFO
			// BECOMES empty, or when IER bit 1 is enabled while it
			// already is -- both of which the real part does -- and
			// cleared by reading IIR or writing THR.
			//
			// A level ("interrupt while empty") passes every
			// functional test and then storms: reading IIR clears the
			// flag and the next cycle re-raises it, so an ISR that
			// finds nothing to send returns into itself forever.
			// sw/os/uart.c happens to survive that, because it only
			// sets IER bit 1 while it has something queued and clears
			// it again when the ring drains -- but relying on the
			// driver to avoid a hardware livelock is not a contract
			// worth writing down.
			tx_empty_d <= tx_empty;
			ier1_d <= reg_ier[1];

			if (tx_empty && !tx_empty_d)
				thre_int <= 1'b1;

			if (reg_ier[1] && !ier1_d && tx_empty)
				thre_int <= 1'b1;

			if (!reg_ier[1])
				thre_int <= 1'b0;

			// -- bus --
			if (bus_acc) begin

				wb_ack_o <= 1'b1;

				if (wb_we_i) begin

					case (reg_sel)

						REG_RBR: begin
							if (dlab) begin
								reg_dll <= wb_dat_i[7:0];
							end else begin
								// A write to a full FIFO is dropped.
								// Software is expected to have
								// checked THRE; the real part does
								// the same thing and there is no
								// status bit to report it with.
								if (!tx_full) begin
									tx_fifo[tx_wr[FIFO_BITS-1:0]] <= wb_dat_i[7:0];
									tx_wr <= tx_wr + 1;
								end
								thre_int <= 1'b0;
							end
						end

						REG_IER: begin
							if (dlab)
								reg_dlm <= wb_dat_i[7:0];
							else
								reg_ier <= wb_dat_i[7:0];
						end

						// FCR. Bit 0 enables the FIFOs (reported back
						// through IIR and nothing else), bit 1 clears
						// the receive FIFO, bit 2 the transmit one.
						// Bits 7:6 select a trigger level and are
						// ignored -- see the header.
						REG_IIR: begin
							fifo_en <= wb_dat_i[0];
							if (wb_dat_i[1]) begin
								rx_rd <= rx_wr;
								err_oe <= 1'b0;
								err_pe <= 1'b0;
								err_fe <= 1'b0;
								err_bi <= 1'b0;
							end
							if (wb_dat_i[2])
								tx_wr <= tx_rd;
						end

						REG_LCR: reg_lcr <= wb_dat_i[7:0];

						// MCR, MSR, SCR and everything else in the
						// window: accepted, discarded, acked. See the
						// header -- the ack is the load-bearing part.
						default: ;

					endcase

				end else begin

					case (reg_sel)

						REG_RBR: begin
							if (dlab) begin
								dat_r <= {24'h0, reg_dll};
							end else begin
								dat_r <= {24'h0, rx_head};
								if (!rx_empty)
									rx_rd <= rx_rd + 1;
							end
						end

						REG_IER: dat_r <= dlab ?
							{24'h0, reg_dlm} : {24'h0, reg_ier};

						// Reading IIR clears a pending transmit
						// interrupt, as on the real part. This is
						// what stops sw/os/uart.c's handler being
						// re-entered forever once its ring is empty
						// and it has stopped writing THR.
						REG_IIR: begin
							dat_r <= {24'h0, iir_value};
							thre_int <= 1'b0;
						end

						REG_LCR: dat_r <= {24'h0, reg_lcr};

						// Reading LSR clears the error bits. Note
						// this is written AFTER the receive push
						// above, so an error arriving in the same
						// cycle as the read is not lost -- it lands
						// in the bit this read is clearing, and the
						// value returned already includes it.
						REG_LSR: begin
							dat_r <= {24'h0, lsr_value};
							err_oe <= 1'b0;
							err_pe <= 1'b0;
							err_fe <= 1'b0;
							err_bi <= 1'b0;
						end

						// MCR, MSR, SCR: zero, acked.
						default: dat_r <= 32'h0;

					endcase

				end

			end

		end

	end

endmodule

`default_nettype wire
