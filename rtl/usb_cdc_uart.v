/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB CDC-ACM console -- a 16550-shaped front end on a USB device.
 *
 * -- What this is --
 *
 * rtl/ext/usb_cdc (ulixxe, MIT) is a full-speed USB device that
 * enumerates as a CDC-ACM serial port: /dev/ttyACM0 on Linux,
 * /dev/cu.usbmodem* on macOS, a COM port on Windows. It hands this
 * module a plain byte pipe in each direction with valid/ready
 * handshaking.
 *
 * This module puts a 16550 register map in front of that pipe and sits
 * in the 0xf000_00xx window where rtl/uart.v otherwise sits. That
 * is the entire point of the exercise: sw/bios/bios.c, sw/os/uart.c
 * and sw/os/sh.c reach the console through sw/common/zeitlos.h's
 * reg_uart0_* macros, and none of them change. A board built with
 * `USB_CDC has a console on its USB-C socket and the software half of
 * the tree does not know the difference.
 *
 * See docs/usb_cdc.md.
 *
 * -- Why replace UART0 rather than sit beside it --
 *
 * Because the point is to free a PMOD connector. Obst has two and
 * Lakritz has one; the console owns one of them on both boards, and a
 * machine whose console arrives over the USB-C socket that was already
 * there (supplying power, and doing nothing else once the DFU
 * bootloader has handed over) needs no USB-UART PMOD at all. Keeping
 * both would leave the connector occupied and would cost the 16550's
 * ~720 LUT4-equivalents on top of this block's.
 *
 * `USB_CDC and `UART0 are therefore alternatives. rtl/sysctl.v gives
 * this module the cs_uart0 window when `USB_CDC is defined and does
 * not instantiate uart_top; rtl/boards.vh's per-board blocks pick one
 * or the other.
 *
 * -- The stall, and why it is the feature rather than the bug --
 *
 * A USB device does not exist until a host enumerates it, and a
 * CDC-ACM port is not drained until something opens it: Linux's
 * cdc_acm driver only submits read URBs from its open() path. The
 * BIOS, meanwhile, prints its banner within microseconds of reset.
 *
 * There is no buffer here to hold that banner -- Obst is at 52 of 56
 * DP16KD before this block exists, and every byte of storage in
 * rtl/ext/usb_cdc is flip-flops. So instead of buffering, this block
 * lets the backpressure through: THRE (LSR bit 5) reports "no room"
 * whenever the byte in the holding register has not yet been taken by
 * the USB side, and
 *
 *     while ((reg_uart0_lsr & 0x20) == 0);
 *
 * in sw/bios/bios.c's putchar() then blocks exactly where it should.
 * Open minicom and the whole banner from "ZB" onward comes out in
 * order, with nothing lost. That is better than a buffer would manage,
 * because it has no size limit -- the machine simply waits.
 *
 * -- ...and the timeout, which is not optional --
 *
 * If nothing ever opens the port, the above is a hang. Not a degraded
 * console: a dead machine. load_zeitlos() never runs, so there is no
 * kernel, no desktop and nothing on the VGA output either. A board
 * plugged into a phone charger would never boot.
 *
 * So a byte that has been stuck in the holding register for
 * STALL_CYCLES continuously gives up: tx_giveup goes high, THRE starts
 * reporting ready, and further writes are DISCARDED until the USB side
 * accepts something again. Ten seconds at 48MHz is the default -- long
 * enough that a human plugging in a cable and starting a terminal
 * wins, short enough that an unattended board boots.
 *
 * Recovery is automatic and needs no software involvement: the moment
 * in_ready_o rises (the host has opened the port and the endpoint FIFO
 * has drained), the stuck byte is taken, tx_giveup clears and normal
 * backpressure resumes. Output written during the gap is gone, which
 * is the correct trade -- it is the same bargain a real UART with
 * nothing attached makes on every character.
 *
 * -- What is NOT emulated --
 *
 * Baud rate, word length, parity and stop bits. DLL/DLM/LCR are
 * stored and read back so that sw/bios/bios.c's uart_init() sequence
 * behaves (in particular DLAB must work, or its divisor writes would
 * be transmitted as characters), but they change nothing: a CDC-ACM
 * link runs at USB speed and its line coding is advisory. Software
 * asking for 1 Mbaud gets whatever the bus gives it, which is more.
 *
 * -- FCR's receive flush, and why it earns its keep --
 *
 * Both FCR resets are real. Bit 2 clears the transmit holding
 * register; bit 1 drains usb_cdc's out_fifo by holding its ready line
 * high, since that FIFO has no flush input of its own.
 *
 * Bit 1 was originally accepted and ignored, on the reasoning that
 * bios.c and sw/os/uart.c each write 0b111 once at init when there is
 * nothing to flush. That reasoning was wrong here, and the way it was
 * wrong is specific to this block.
 *
 * The console blocks until a terminal opens the port (see above). A
 * terminal opening a port is also the moment it sends its greeting:
 * minicom's default modem init string, or ModemManager's AT probes on
 * a Linux box without the udev rule from docs/usb_cdc.md. So the BIOS
 * unblocks, finishes its banner, reaches its prompt loop and finds
 * bytes already waiting -- and sw/bios/bios.c treats ANY byte as
 * "the user interacted", which cancels autoboot permanently (its ctr
 * test is an exact equality that never matches again). The machine
 * sits at the BIOS prompt instead of booting, every time.
 *
 * On a real UART that was a race you usually won, because the console
 * was already draining before anyone opened a terminal. Here the two
 * events are the SAME event, so it is not a race at all.
 *
 * bios.c therefore writes FCR once more on the way into its prompt
 * loop -- after the banner has drained, which is the point at which
 * the console genuinely becomes usable -- and this makes that write
 * mean something.
 *
 * The modem control lines. MSR reads a constant with DCD, DSR and CTS
 * asserted, because there is no cable and nothing to be disconnected
 * from. MCR is stored and read back and drives nothing.
 *
 * -- Flow control comes free --
 *
 * The receive path has no overrun. rtl/ext/usb_cdc NAKs an OUT
 * transaction when its FIFO is full and the host retries, so a reader
 * that is slow makes the sender wait rather than losing bytes. The
 * 16550 this replaces had a 16-byte FIFO and an overrun bit, and
 * sw/common/zuart.h's header explains what that cost at high rates.
 * LSR bit 1 (OE) is therefore wired to 0 and means it.
 */

`default_nettype none

module usb_cdc_uart #(
	// USB device identity.
	//
	// The default is 16d0:116d, which is Machdyne's DFU bootloader
	// identity on these same boards -- SHARED WITH IT DELIBERATELY,
	// not by oversight.
	//
	// That PID is not per-board. Every board in
	// machdyne/tinydfu-bootloader uses 16d0:116d and they are told
	// apart by their product strings, so the PID already identifies a
	// function rather than a device. Hosts bind by interface class
	// anyway: dfu-util matches the DFU interface descriptor
	// (0xFE/0x01) and Linux's cdc_acm matches 0x02/0x02, so both
	// devices are found correctly even though they share an ID and
	// appear on the same socket a second apart during boot.
	//
	// THE COST IS ON WINDOWS. Driver binding there is keyed on the
	// hardware ID USB\VID_16D0&PID_116D, so anyone who ran Zadig to
	// install WinUSB for dfu-util has bound that ID to WinUSB -- and
	// the console can inherit that binding and produce no COM port.
	// bcdDevice does not rescue this and could not be set anyway; see
	// docs/usb_cdc.md, which also carries the udev rule Linux wants.
	//
	// Override per board from rtl/boards.vh's `USB_CDC_VID and
	// `USB_CDC_PID if a separate allocation is ever obtained. It is a
	// one-line change and nothing else depends on the value.
	parameter [15:0] VENDORID = 16'h16d0,
	parameter [15:0] PRODUCTID = 16'h116d,

	// Bulk endpoint packet size, bytes. Must be 8, 16, 32 or 64 (USB
	// 2.0 9.6.6 for full speed), and it is the dominant cost in this
	// whole block because rtl/ext/usb_cdc holds its FIFOs in
	// flip-flops -- measured on ECP5, the core alone is 1257 LUT4 at
	// 8, 1463 at 16, 1901 at 32 and 3038 at 64.
	//
	// 8 is the default deliberately. It is worth roughly what the
	// 1 Mbaud console it replaces was worth, which is all this needs
	// to be: the console is a console, and `xf` uploads over it are
	// no slower than they were. Raise it on a board with fabric to
	// spare (Lakritz) if a faster link is actually wanted; do not
	// raise it on Obst without re-checking timing, which has ~3%
	// margin at 48MHz before this block is added at all.
	parameter MAXPACKETSIZE = 8,

	// How long a byte may sit unaccepted before this block gives up
	// and starts discarding. See the header. 480_000_000 cycles is
	// ten seconds at 48MHz.
	//
	// Zero would disable the timeout and is NOT offered: the whole
	// reason it exists is that the failure it prevents is a dead
	// machine rather than a quiet console.
	parameter [31:0] STALL_CYCLES = 32'd480_000_000
)
(
	input wire wb_clk_i,
	input wire wb_rst_i,

	// Word-addressed, like rtl/uart_null.v and every other slave
	// here: rtl/sysctl.v passes wbm_adr_sel_word, so register n sits
	// at byte offset 4n, which is where sw/common/zeitlos.h's
	// reg_uart0_* macros put it.
	input wire [25:0] wb_adr_i,
	input wire [31:0] wb_dat_i,
	output wire [31:0] wb_dat_o,
	input wire wb_we_i,
	input wire [3:0] wb_sel_i,
	input wire wb_stb_i,
	input wire wb_cyc_i,
	output reg wb_ack_o,

	output wire int_o,

	// USB full-speed device port. Bidirectional because the line
	// turns around every transaction; the pull-up is driven high to
	// announce the device and released otherwise, so it is declared
	// inout to get a tri-state buffer rather than a driven zero (a
	// driven zero would pull D+ DOWN through the board's 1.5k, which
	// is not the same as being absent).
	inout wire usb_dp,
	inout wire usb_dn,
	inout wire usb_pu,

	// High while the host has this device configured. Exposed for
	// rtl/sysctl.v to put on an LED or hand to rtl/csrs.v; nothing
	// in the register map depends on it.
	output wire configured_o
);

	// -- 16550 register indices, as WORDS --
	//
	// Byte offsets are 4x these. RBR/THR/DLL share word 0 and
	// IER/DLM share word 1, selected by LCR bit 7 (DLAB) exactly as
	// on the real part.
	localparam [2:0] REG_RBR = 3'd0;	// 0x00 read  (also DLL w/ DLAB)
	localparam [2:0] REG_IER = 3'd1;	// 0x04       (also DLM w/ DLAB)
	localparam [2:0] REG_IIR = 3'd2;	// 0x08 read  (FCR on write)
	localparam [2:0] REG_LCR = 3'd3;	// 0x0c
	localparam [2:0] REG_MCR = 3'd4;	// 0x10
	localparam [2:0] REG_LSR = 3'd5;	// 0x14
	localparam [2:0] REG_MSR = 3'd6;	// 0x18
	localparam [2:0] REG_SCR = 3'd7;	// 0x1c

	// Modem status, constant. DCD (7), DSR (5) and CTS (4) asserted;
	// RI (6) and all four delta bits clear. There is no cable here to
	// report the state of, and reporting "everything is connected" is
	// what lets software that checks these proceed.
	localparam [7:0] MSR_VALUE = 8'hb0;

	// -- register file --
	//
	// Declared at module scope rather than inside the always block
	// below. That is a house rule for this tree (plain Verilog, no
	// local declarations) and not merely a style preference: yosys
	// accepts either, but the GateMate flow reads these files through
	// `read -sv` and the ice40/ecp5 ones do not, so anything that
	// needs SystemVerilog scoping builds on one target and not the
	// others.
	reg [7:0] reg_ier;
	reg [7:0] reg_lcr;
	reg [7:0] reg_mcr;
	reg [7:0] reg_scr;
	reg [7:0] reg_dll;
	reg [7:0] reg_dlm;
	reg fifo_en;

	reg [31:0] dat_r;

	// Transmit holding register. ONE byte, not a FIFO -- the depth
	// that matters is rtl/ext/usb_cdc's in_fifo behind it, and adding
	// a second buffer here would only move the point at which
	// backpressure starts without changing that it does.
	reg [7:0] thr_data;
	reg thr_full;

	reg thre_int;
	reg tx_giveup;
	reg [31:0] stall_ctr;

	// Receive flush, driven by FCR bit 1. See the header's note on
	// what this is actually for -- it is not housekeeping, it is how
	// the BIOS discards a terminal's modem init string.
	reg rx_flush;
	reg [3:0] rx_flush_idle;

	// -- byte pipes to/from rtl/ext/usb_cdc --
	wire [7:0] usb_out_data;
	wire usb_out_valid;
	wire usb_out_ready;
	wire [7:0] usb_in_data;
	wire usb_in_valid;
	wire usb_in_ready;
	wire [10:0] usb_frame;
	wire usb_cdc_configured;

	// -- USB line buffers --
	wire usb_dp_tx;
	wire usb_dn_tx;
	wire usb_tx_en;
	wire usb_dp_pu;

	// -- bus access decode --
	//
	// `bus_acc` is high for exactly one cycle per transfer: wb_ack_o
	// is registered and clears itself the cycle after, so the
	// !wb_ack_o term makes this a single pulse even though wb_stb_i
	// stays high until the ack is seen. Every side effect below is
	// gated on it, which is what stops a read of RBR from consuming
	// two bytes.
	wire bus_acc;
	wire bus_rd;
	wire bus_wr;
	wire [2:0] reg_sel;
	wire dlab;

	assign bus_acc = wb_cyc_i && wb_stb_i && !wb_ack_o;
	assign bus_rd = bus_acc && !wb_we_i;
	assign bus_wr = bus_acc && wb_we_i;
	assign reg_sel = wb_adr_i[2:0];
	assign dlab = reg_lcr[7];

	// -- receive --
	//
	// No FIFO of our own. usb_cdc's out_fifo holds up to
	// 2*MAXPACKETSIZE bytes and NAKs the host when it is full, so it
	// IS the 16550's receive FIFO and a better-behaved one -- there
	// is no overrun to report because there is no way to overrun it.
	//
	// The ready pulse is combinational and coincides with the cycle
	// dat_r is latched, so the byte handed to the bus is the byte
	// consumed. Qualified on usb_out_valid so that a read of an empty
	// port does not poke the FIFO.
	wire rx_take;
	assign rx_take = bus_rd && (reg_sel == REG_RBR) && !dlab && usb_out_valid;
	assign usb_out_ready = rx_take || (rx_flush && usb_out_valid);

	// -- transmit --
	wire thr_write;
	wire in_taken;
	wire lsr_thre;

	assign thr_write = bus_wr && (reg_sel == REG_RBR) && !dlab;
	assign usb_in_data = thr_data;
	assign usb_in_valid = thr_full;
	assign in_taken = thr_full && usb_in_ready;

	// THRE lies while tx_giveup is set. That is the timeout doing its
	// job -- see the header. Everywhere else it is the honest answer
	// to "is the holding register free".
	assign lsr_thre = !thr_full || tx_giveup;

	// -- line status --
	//
	// OE (1), PE (2), FE (3) and BI (4) are hardwired zero and mean
	// it: there is no line to receive a break or a framing error on,
	// and the receive path cannot overrun (see above). TEMT (6)
	// tracks THRE rather than the true "shift register also empty",
	// because usb_cdc does not expose whether its in_fifo has drained
	// and nothing in this tree reads TEMT.
	wire [7:0] lsr_value;
	assign lsr_value = {
		1'b0,			// 7: error in FIFO
		lsr_thre,		// 6: TEMT
		lsr_thre,		// 5: THRE
		1'b0,			// 4: BI
		1'b0,			// 3: FE
		1'b0,			// 2: PE
		1'b0,			// 1: OE
		usb_out_valid	// 0: DR
	};

	// -- interrupt identification --
	//
	// Two sources, in the real part's priority order: received data
	// available beats transmitter holding register empty.
	//
	// Bits 7:6 report FIFOs enabled when FCR bit 0 is set, purely so
	// this reads like the part it is imitating. sw/os/uart.c tests
	// bit 0 for "anything pending" and bits 3:1 for which, and is
	// unaffected either way.
	wire rda_int;
	wire [7:0] iir_value;

	assign rda_int = reg_ier[0] && usb_out_valid;
	assign iir_value = {
		fifo_en, fifo_en,
		4'b0000,
		rda_int ? 2'b10 : (thre_int ? 2'b01 : 2'b00),
		(rda_int || thre_int) ? 1'b0 : 1'b1
	};

	// LEVEL-SENSITIVE, like the 16550 it replaces. rtl/sysctl.v keeps
	// bit 4 clear in LATCHED_IRQ for exactly this reason: latching a
	// level source re-fires the instant the handler returns.
	assign int_o = rda_int || (reg_ier[1] && thre_int);

	assign wb_dat_o = dat_r;
	assign configured_o = usb_cdc_configured;

	// -- register file and transmit sequencing --
	always @(posedge wb_clk_i) begin

		wb_ack_o <= 1'b0;

		if (wb_rst_i) begin

			reg_ier <= 8'h00;
			reg_lcr <= 8'h00;
			reg_mcr <= 8'h00;
			reg_scr <= 8'h00;
			reg_dll <= 8'h00;
			reg_dlm <= 8'h00;
			fifo_en <= 1'b0;
			dat_r <= 32'h0;
			thr_data <= 8'h00;
			thr_full <= 1'b0;
			thre_int <= 1'b0;
			tx_giveup <= 1'b0;
			stall_ctr <= 32'h0;
			rx_flush <= 1'b0;
			rx_flush_idle <= 4'd0;

		end else begin

			// -- transmit hand-off and the give-up timer --
			//
			// Written before the bus section below so that a write
			// landing in the same cycle as a hand-off wins: the byte
			// usb_cdc sampled this cycle is gone, and the new one
			// belongs in the holding register immediately rather
			// than a transfer later.
			if (in_taken) begin
				thr_full <= 1'b0;
				thre_int <= 1'b1;
				// Recovery. Whatever made the USB side stop
				// accepting has passed -- the host enumerated, or
				// something opened the port -- so stop discarding.
				tx_giveup <= 1'b0;
				stall_ctr <= 32'h0;
			end else if (thr_full && !tx_giveup) begin
				if (stall_ctr >= STALL_CYCLES)
					tx_giveup <= 1'b1;
				else
					stall_ctr <= stall_ctr + 1;
			end

			if (!reg_ier[1])
				thre_int <= 1'b0;

			// -- receive flush --
			//
			// Drains usb_cdc's out_fifo by holding its ready line
			// high, since that FIFO has no flush input of its own.
			//
			// The idle counter is the part that is not obvious.
			// out_fifo's valid line DIPS between byte hand-offs, so
			// clearing this on the first low cycle would end the
			// flush after one byte and leave the rest of the packet
			// sitting there -- which looks exactly like the bug the
			// flush was added to fix. Sixteen consecutive idle
			// cycles is 333ns at 48MHz: far longer than any gap
			// within a packet, far shorter than the 1ms frame that
			// would bring the next one.
			//
			// Placed BEFORE the bus block below so that an FCR write
			// arriving while a flush is already winding down
			// restarts it rather than being swallowed.
			if (rx_flush) begin
				if (usb_out_valid)
					rx_flush_idle <= 4'd0;
				else if (rx_flush_idle == 4'hf)
					rx_flush <= 1'b0;
				else
					rx_flush_idle <= rx_flush_idle + 1;
			end

			// -- bus --
			if (bus_acc) begin

				wb_ack_o <= 1'b1;

				if (wb_we_i) begin

					case (reg_sel)

						REG_RBR: begin
							if (dlab) begin
								reg_dll <= wb_dat_i[7:0];
							end else begin
								// Accept only if the holding
								// register is free, or is being
								// freed this very cycle. Anything
								// else is a write that arrived
								// while tx_giveup was set, and it
								// is DROPPED -- which is the whole
								// contract of the timeout.
								if (!thr_full || in_taken) begin
									thr_data <= wb_dat_i[7:0];
									thr_full <= 1'b1;
									stall_ctr <= 32'h0;
								end
								// Writing THR clears a pending
								// transmit interrupt, as on the
								// real part -- otherwise
								// sw/os/uart.c's ISR would be
								// re-entered for a condition it has
								// just serviced.
								thre_int <= 1'b0;
							end
						end

						REG_IER: begin
							if (dlab)
								reg_dlm <= wb_dat_i[7:0];
							else
								reg_ier <= wb_dat_i[7:0];
						end

						// FCR. Bit 0 enables the FIFOs (reported
						// back through IIR and nothing else), bit 2
						// resets the transmit side. Bit 1 asks to
						// reset the RECEIVE FIFO and is accepted and
						// ignored -- that buffer lives in
						// rtl/ext/usb_cdc and has no flush input.
						// See the header.
						REG_IIR: begin
							fifo_en <= wb_dat_i[0];
							if (wb_dat_i[1]) begin
								rx_flush <= 1'b1;
								rx_flush_idle <= 4'd0;
							end
							if (wb_dat_i[2]) begin
								thr_full <= 1'b0;
								tx_giveup <= 1'b0;
								stall_ctr <= 32'h0;
							end
						end

						REG_LCR: reg_lcr <= wb_dat_i[7:0];
						REG_MCR: reg_mcr <= wb_dat_i[7:0];
						REG_MSR: ;	// read-only
						REG_LSR: ;	// read-only
						REG_SCR: reg_scr <= wb_dat_i[7:0];

						default: ;

					endcase

				end else begin

					case (reg_sel)

						REG_RBR: dat_r <= dlab ?
							{24'h0, reg_dll} : {24'h0, usb_out_data};

						REG_IER: dat_r <= dlab ?
							{24'h0, reg_dlm} : {24'h0, reg_ier};

						// Reading IIR clears a pending transmit
						// interrupt, as on the real part. This is
						// what stops sw/os/uart.c's handler from
						// being re-entered forever once the ring is
						// empty and it has stopped writing THR.
						REG_IIR: begin
							dat_r <= {24'h0, iir_value};
							thre_int <= 1'b0;
						end

						REG_LCR: dat_r <= {24'h0, reg_lcr};
						REG_MCR: dat_r <= {24'h0, reg_mcr};
						REG_LSR: dat_r <= {24'h0, lsr_value};
						REG_MSR: dat_r <= {24'h0, MSR_VALUE};
						REG_SCR: dat_r <= {24'h0, reg_scr};

						default: dat_r <= 32'h0;

					endcase

				end

			end

		end

	end

	// -- the device itself --
	//
	// USE_APP_CLK is 0 and app_clk_i is wired to the same clock,
	// which is the cheap configuration: usb_cdc wants 12MHz *
	// BIT_SAMPLES and sys_clk on every board that can have this is
	// exactly 48MHz, so BIT_SAMPLES = 4 lands on it with no PLL
	// output of its own and no clock-domain crossing to pay for.
	//
	// A board clocked at anything other than 48MHz needs a dedicated
	// 48MHz output and USE_APP_CLK = 1; rtl/boards.vh's `USB_CDC
	// guard rejects that case rather than building something that
	// enumerates intermittently.
	usb_cdc #(
		.VENDORID(VENDORID),
		.PRODUCTID(PRODUCTID),
		.CHANNELS(1),
		.IN_BULK_MAXPACKETSIZE(MAXPACKETSIZE),
		.OUT_BULK_MAXPACKETSIZE(MAXPACKETSIZE),
		.BIT_SAMPLES(4),
		.USE_APP_CLK(0),
		.APP_CLK_FREQ(48)
	) usb_cdc_i (
		.clk_i(wb_clk_i),
		.rstn_i(~wb_rst_i),
		.app_clk_i(wb_clk_i),

		.out_data_o(usb_out_data),
		.out_valid_o(usb_out_valid),
		.out_ready_i(usb_out_ready),

		.in_data_i(usb_in_data),
		.in_valid_i(usb_in_valid),
		.in_ready_o(usb_in_ready),

		.frame_o(usb_frame),
		.configured_o(usb_cdc_configured),

		.dp_pu_o(usb_dp_pu),
		.tx_en_o(usb_tx_en),
		.dp_tx_o(usb_dp_tx),
		.dn_tx_o(usb_dn_tx),
		.dp_rx_i(usb_dp),
		.dn_rx_i(usb_dn)
	);

	// -- line buffers --
	//
	// Same shape as rtl/ext/usb_hid_host's own, and for the same
	// reason: a plain conditional continuous assignment is what every
	// toolchain in this tree infers a bidirectional pad from, without
	// a vendor primitive that would have to be written three times.
	//
	// One driver each. usb_tx_en comes from the core's transmit state
	// machine and nothing else touches these nets.
	assign usb_dp = usb_tx_en ? usb_dp_tx : 1'bz;
	assign usb_dn = usb_tx_en ? usb_dn_tx : 1'bz;

	// The 1.5k pull-up on D+ (R6 on Lakritz, the equivalent part on
	// Obst) is switched by this pin. Driven HIGH to announce a
	// full-speed device and released to high impedance otherwise --
	// never driven low, which would pull D+ down through that
	// resistor and is a different thing from being unplugged.
	//
	// Releasing it is also how a re-enumeration would be forced, if
	// something ever wants one: hold this low for a few milliseconds
	// and the host sees a disconnect. Nothing does today.
	assign usb_pu = usb_dp_pu ? 1'b1 : 1'bz;

endmodule

`default_nettype wire
