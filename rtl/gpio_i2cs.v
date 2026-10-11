/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * GPIO I2C slave -- MIRROR MODE. Lets a real I2C master (a Zwölf
 * module's LS10, bit-banging its C/D bus) talk to chips that only exist
 * in software (sw/apps/bench's virtual parts), without software in the
 * transaction. docs/gpio.md, "I2C slave"; docs/bench.md says what it is
 * for.
 *
 * rtl/gpio.v instantiates one of these when `GPIO_I2CS_SLOTS is not 0
 * (rtl/boards.vh: off by default, opt-in) and puts its registers at
 * 0xe000_2100.
 *
 * -- What it does --
 *
 * Each SLOT is one emulated chip: an I2C address, a 256-register image
 * (the MIRROR), a register pointer and an auto-increment rule. The
 * master's transactions are answered entirely from the mirror:
 *
 *   write   address, then a command byte that sets the slot's pointer,
 *           then data bytes, each stored at the pointer (if that
 *           register is writable) and logged, the pointer advancing
 *   read    bytes from the pointer, the pointer advancing
 *   both    write-then-read with a repeated start: set the pointer,
 *           read from it
 *   probe   the address alone: ACK if a slot answers it, NACK if not
 *
 * The pointer is per slot and PERSISTS between transactions, as on the
 * real chips: a read with no command byte first starts where the last
 * transaction left it. Auto-increment per slot: none, linear (wrapping
 * at 255), or PAIR TOGGLE (pointer XOR 1), which is what a TCA9535/9555
 * does inside its register pairs.
 *
 * A write to a register whose writable bit is clear is ACKed and does
 * not change the mirror -- a TCA's input port does the same -- but is
 * still logged, so software sees everything the master did.
 *
 * Software keeps the mirror current (IMADDR/IMDATA) and applies the
 * side effects of writes by draining the log (ILOG). For reads it gets
 * a sticky per-slot flag and the register the last read started at
 * (enough to release a TCA's INT after its input port is read). It
 * polls: there is no interrupt, by design (docs/gpio.md).
 *
 * -- What it never does --
 *
 * It never stretches the clock: SCL is input only. The LS10 waits about
 * 1 ms for a stretched clock and then carries on as if it were high, so
 * stretching would corrupt a transaction rather than slow it. SDA is
 * driven low or released, never driven high. No 10-bit addresses, no
 * general call.
 *
 * -- Pins --
 *
 * SCL and SDA are any two pins of any port (ICTL). While ICTL.EN is set
 * both are OWNED, as an engine owns its pins (rtl/gpio_stream.v): DIR
 * and OUT stop reaching them, so zgpio's read-modify-write on the other
 * pins of the port cannot disturb the bus. Inputs come from gpio.v's
 * two-flop synchroniser and pass a glitch filter here (FILT clocks of
 * agreement, about 250 ns); the LS10's ~40 kHz and 400 kHz fast mode
 * are both easy at 48 MHz.
 *
 * -- One block RAM --
 *
 * A DP16KD in 2048x9 mode, one write port and one read port (the shape
 * yosys maps to block RAM; rtl/gpio_stream.v, "The FIFO RAM", says why
 * not two of each):
 *
 *   0-1023      the mirror: { slot[1:0], reg[7:0] }, each a byte and its
 *               writable bit
 *   1024-2047   the write log: 512 entries, two words each,
 *               { slot[1:0], reg[7:1] } then { reg[0], data[7:0] }
 *
 * One sequencer owns both ports and does one job at a time: the I2C
 * side's (fetch a byte to send; store a written byte and log it), or
 * the CPU's (IMDATA, ILOG). The I2C side goes first and its jobs take
 * four clocks at most; the CPU's take three. I2C runs in microseconds,
 * so neither side ever waits long enough to matter.
 *
 * -- Bus --
 *
 * Same as rtl/gpio_stream.v: gpio.v holds `acc` until `done`.
 */

module gpio_i2cs #(
	parameter NPINS = 8,		// GPIO pins built: NPORTS * 8
	parameter SLOTS = 4,		// 1, 2 or 4 emulated chips
	parameter FILT = 12			// glitch filter, clocks
)
(
	input clk,
	input rst,

	input acc,
	input we,
	input [3:0] ra,
	input [31:0] wd,
	output reg done,
	output reg [31:0] rdo,

	input [63:0] pin_in,
	output [63:0] own,
	output [63:0] own_dir,
	output [63:0] own_out
);

	// ------------------------------------------------------------------
	// Configuration
	// ------------------------------------------------------------------

	reg en;
	reg [5:0] scl_pin, sda_pin;

	reg [6:0] s_addr [0:3];
	reg [3:0] s_valid;
	reg [1:0] s_inc [0:3];
	reg [7:0] s_ptr [0:3];
	reg [7:0] s_rdstart [0:3];
	reg [3:0] s_rd;				// sticky: a read began on this slot

	reg [9:0] imaddr;			// { slot, reg } for IMDATA

	// log pointers: 10 bits over 512 entries, top bit for full/empty
	reg [9:0] lw, lr;
	wire [9:0] llevel = lw - lr;
	wire lfull = llevel[9];
	reg lovf;

	localparam INC_NONE = 2'd0, INC_LINEAR = 2'd1, INC_PAIR = 2'd2;

	function [7:0] next_ptr;
		input [7:0] p;
		input [1:0] m;
		begin
			case (m)
			INC_LINEAR: next_ptr = p + 8'd1;
			INC_PAIR: next_ptr = p ^ 8'd1;
			default: next_ptr = p;
			endcase
		end
	endfunction

	// ------------------------------------------------------------------
	// Pins and the glitch filter
	// ------------------------------------------------------------------

	wire scl_ok = (scl_pin < NPINS);
	wire sda_ok = (sda_pin < NPINS);
	wire live = en && scl_ok && sda_ok && (scl_pin != sda_pin);

	// Only the pins this build has: a 64-way mux and decoder per line
	// for the pins that might exist would be most of this block's logic.
	wire [NPINS-1:0] pins = pin_in[NPINS-1:0];
	wire scl_raw = pins[scl_pin];
	wire sda_raw = pins[sda_pin];

	reg scl_f, sda_f, scl_p, sda_p;
	reg [3:0] scl_c, sda_c;

	always @(posedge clk) begin
		if (rst || !live) begin
			scl_f <= 1'b1; sda_f <= 1'b1;
			scl_p <= 1'b1; sda_p <= 1'b1;
			scl_c <= 4'd0; sda_c <= 4'd0;
		end else begin
			scl_p <= scl_f;
			sda_p <= sda_f;
			if (scl_raw == scl_f) scl_c <= 4'd0;
			else if (scl_c == FILT - 1) begin scl_f <= scl_raw; scl_c <= 4'd0; end
			else scl_c <= scl_c + 4'd1;
			if (sda_raw == sda_f) sda_c <= 4'd0;
			else if (sda_c == FILT - 1) begin sda_f <= sda_raw; sda_c <= 4'd0; end
			else sda_c <= sda_c + 4'd1;
		end
	end

	// A start or stop is SDA moving while SCL is high. A master may move
	// SDA the instant after it pulls SCL low (I2C allows a hold time of
	// 0 ns), and the two filters can then flip a clock apart -- SDA's
	// first -- which would look like a stop or a start. The I2C spec
	// makes the slave bridge that: here, SCL must still be high at the
	// pin (scl_raw) too. After a falling clock it has been low there for
	// a whole filter time by the time SDA's change is seen; a real start
	// or stop happens with SCL high for microseconds.
	wire start = scl_f && scl_p && scl_raw && sda_p && !sda_f;
	wire stop = scl_f && scl_p && scl_raw && !sda_p && sda_f;
	wire scl_rise = scl_f && !scl_p;
	wire scl_fall = !scl_f && scl_p;

	reg sda_low;				// we pull SDA low

	wire [NPINS-1:0] scl_bit = { {(NPINS-1){1'b0}}, 1'b1 } << scl_pin;
	wire [NPINS-1:0] sda_bit = { {(NPINS-1){1'b0}}, 1'b1 } << sda_pin;
	assign own = live ? { {(64-NPINS){1'b0}}, scl_bit | sda_bit } : 64'd0;
	assign own_dir = (live && sda_low) ? { {(64-NPINS){1'b0}}, sda_bit } : 64'd0;
	assign own_out = 64'd0;		// open drain: the only level driven is low

	// ------------------------------------------------------------------
	// The RAM
	// ------------------------------------------------------------------

	reg [8:0] mem [0:2047];
	reg [8:0] q;

	reg m_we;
	reg [10:0] m_waddr, m_raddr;
	reg [8:0] m_wdata;

	always @(posedge clk) begin
		if (m_we) mem[m_waddr] <= m_wdata;
	end

	always @(posedge clk) begin
		q <= mem[m_raddr];
	end

	// ------------------------------------------------------------------
	// The I2C protocol
	// ------------------------------------------------------------------

	localparam P_IDLE = 3'd0,	// not addressed: wait for a start
	           P_RECV = 3'd1,	// receiving a byte (address, command, data)
	           P_ACK = 3'd2,	// holding SDA low for our ACK
	           P_SEND = 3'd3,	// sending a byte
	           P_MACK = 3'd4;	// the master's ACK or NACK after a byte we sent

	localparam B_ADDR = 2'd0, B_CMD = 2'd1, B_DATA = 2'd2;

	reg [2:0] ps;
	reg [1:0] what;				// what the byte being received is
	reg [3:0] bitn;
	reg [7:0] sh;
	reg [1:0] slot;
	reg after_send;				// P_ACK ends by starting to send
	reg mack;					// the master ACKed

	// the jobs the sequencer does for this side
	reg j_fetch, j_store;		// requested
	reg [7:0] j_reg, j_data;
	reg [1:0] j_slot;
	reg [7:0] tx;				// the byte to send, once fetched

	// which slot answers an address: the lowest valid match
	reg hit;
	reg [1:0] hit_slot;
	integer k;
	always @(*) begin
		hit = 1'b0;
		hit_slot = 2'd0;
		for (k = 3; k >= 0; k = k - 1)
			if (k < SLOTS && s_valid[k] && s_addr[k] == sh[7:1]) begin
				hit = 1'b1;
				hit_slot = k[1:0];
			end
	end

	// ------------------------------------------------------------------
	// The sequencer, the CPU's registers, and the protocol, in one
	// process: the RAM's ports, the pointers and the flags each have one
	// driver.
	// ------------------------------------------------------------------

	// The RAM's read address is registered, and so is its output, so a
	// read set up in one state is in q two states later: each read has
	// a state of its own to wait in.
	localparam Q_IDLE = 4'd0,
	           Q_F1 = 4'd1, Q_F2 = 4'd2,			// fetch a byte to send
	           Q_S1 = 4'd3, Q_S2 = 4'd4,			// store a written byte:
	           Q_LOGA = 4'd5, Q_LOGB = 4'd6,		//   its writable bit, then log
	           Q_C1 = 4'd7, Q_C2 = 4'd8,			// CPU: IMDATA read
	           Q_L1 = 4'd9, Q_L2 = 4'd10,			// CPU: ILOG, first word
	           Q_L3 = 4'd11, Q_L4 = 4'd12;			//   second word
	reg [3:0] qs;

	localparam C_IDLE = 2'd0, C_MEM = 2'd1, C_WAIT = 2'd2;
	reg [1:0] cs;
	reg [8:0] la;				// a log entry's first word

	reg [7:0] ssh;				// the byte being sent, shifting out MSB first

	// CPU registers (word offsets)
	localparam R_ICTL = 4'd0, R_ISTAT = 4'd1, R_IMADDR = 4'd2, R_IMDATA = 4'd3,
	           R_ILOG = 4'd8, R_IFLUSH = 4'd9;	// ISLOT0-3 are 4-7

	wire [1:0] ris = ra[1:0];	// ISLOT index
	wire is_slot = (ra[3:2] == 2'b01);
	wire slot_built = (ris < SLOTS);

	reg [31:0] rmux;
	always @(*) begin
		case (ra)
		R_ICTL: rmux = { 10'd0, sda_pin, 2'd0, scl_pin, 7'd0, en };
		R_ISTAT: rmux = { 11'd0, live, s_rd, 2'd0, (ps != P_IDLE), lovf, 2'd0, llevel };
		R_IMADDR: rmux = { 22'd0, imaddr };
		default: rmux = (is_slot && slot_built) ?
			{ s_ptr[ris], s_rdstart[ris], 6'd0, s_inc[ris], s_valid[ris], s_addr[ris] } : 32'd0;
		endcase
	end

	wire [7:0] ptr_now = s_ptr[slot];
	wire [7:0] ptr_next = next_ptr(s_ptr[slot], s_inc[slot]);

	integer i;

	always @(posedge clk) begin
		done <= 1'b0;
		m_we <= 1'b0;

		if (rst) begin
			en <= 1'b0;
			scl_pin <= 6'd0;
			sda_pin <= 6'd1;
			for (i = 0; i < 4; i = i + 1) begin
				s_addr[i] <= 7'd0;
				s_inc[i] <= 2'd0;
				s_ptr[i] <= 8'd0;
				s_rdstart[i] <= 8'd0;
			end
			s_valid <= 4'd0;
			s_rd <= 4'd0;
			imaddr <= 10'd0;
			lw <= 10'd0;
			lr <= 10'd0;
			lovf <= 1'b0;
			ps <= P_IDLE;
			what <= B_ADDR;
			bitn <= 4'd0;
			sh <= 8'd0;
			ssh <= 8'd0;
			slot <= 2'd0;
			after_send <= 1'b0;
			mack <= 1'b0;
			sda_low <= 1'b0;
			j_fetch <= 1'b0;
			j_store <= 1'b0;
			j_slot <= 2'd0;
			j_reg <= 8'd0;
			j_data <= 8'd0;
			tx <= 8'hff;
			qs <= Q_IDLE;
			cs <= C_IDLE;
			rdo <= 32'd0;
			la <= 9'd0;
			m_raddr <= 11'd0;
			m_waddr <= 11'd0;
			m_wdata <= 9'd0;
		end else begin

			// -- the CPU's plain registers --

			case (cs)
			C_IDLE: if (acc) begin
				if (ra == R_IMDATA || (ra == R_ILOG && !we)) begin
					cs <= C_MEM;		// the sequencer finishes it
				end else begin
					if (we) begin
						case (ra)
						R_ICTL: begin
							en <= wd[0];
							scl_pin <= wd[13:8];
							sda_pin <= wd[21:16];
						end
						R_ISTAT: begin
							if (wd[12]) lovf <= 1'b0;
							s_rd <= s_rd & ~wd[19:16];
						end
						R_IMADDR: imaddr <= wd[9:0];
						R_IFLUSH: if (wd[0]) begin
							lr <= lw;
							lovf <= 1'b0;
						end
						default: if (is_slot && slot_built) begin
							s_addr[ris] <= wd[6:0];
							s_valid[ris] <= wd[7];
							s_inc[ris] <= wd[9:8];
							if (wd[15]) s_ptr[ris] <= wd[31:24];
						end
						endcase
					end
					rdo <= rmux;
					done <= 1'b1;
					cs <= C_WAIT;
				end
			end
			C_WAIT: if (!acc) cs <= C_IDLE;
			default: ;
			endcase

			// -- the sequencer: the I2C side's jobs first, then the CPU's --

			case (qs)
			Q_IDLE: begin
				if (j_fetch) begin
					m_raddr <= { 1'b0, j_slot, j_reg };
					qs <= Q_F1;
				end else if (j_store) begin
					m_raddr <= { 1'b0, j_slot, j_reg };
					qs <= Q_S1;
				end else if (cs == C_MEM) begin
					if (ra == R_IMDATA && we) begin
						m_we <= 1'b1;
						m_waddr <= { 1'b0, imaddr };
						m_wdata <= wd[8:0];
						imaddr <= imaddr + 10'd1;
						done <= 1'b1;
						cs <= C_WAIT;
					end else if (ra == R_IMDATA) begin
						m_raddr <= { 1'b0, imaddr };
						qs <= Q_C1;
					end else if (llevel == 10'd0) begin
						rdo <= 32'd0;		// ILOG with nothing logged
						done <= 1'b1;
						cs <= C_WAIT;
					end else begin
						m_raddr <= { 1'b1, lr[8:0], 1'b0 };
						qs <= Q_L1;
					end
				end
			end

			Q_F1: qs <= Q_F2;
			Q_F2: begin
				tx <= q[7:0];
				j_fetch <= 1'b0;
				qs <= Q_IDLE;
			end

			Q_S1: qs <= Q_S2;
			Q_S2: begin
				// a register that is not writable keeps its value: the
				// master still got its ACK, and the log still says what
				// it tried
				if (q[8]) begin
					m_we <= 1'b1;
					m_waddr <= { 1'b0, j_slot, j_reg };
					m_wdata <= { 1'b1, j_data };
				end
				qs <= Q_LOGA;
			end
			Q_LOGA: begin
				if (lfull) begin
					lovf <= 1'b1;			// dropped, and the flag says so
					j_store <= 1'b0;
					qs <= Q_IDLE;
				end else begin
					m_we <= 1'b1;
					m_waddr <= { 1'b1, lw[8:0], 1'b0 };
					m_wdata <= { j_slot, j_reg[7:1] };
					qs <= Q_LOGB;
				end
			end
			Q_LOGB: begin
				m_we <= 1'b1;
				m_waddr <= { 1'b1, lw[8:0], 1'b1 };
				m_wdata <= { j_reg[0], j_data };
				lw <= lw + 10'd1;
				j_store <= 1'b0;
				qs <= Q_IDLE;
			end

			Q_C1: qs <= Q_C2;
			Q_C2: begin
				rdo <= { 23'd0, q };
				imaddr <= imaddr + 10'd1;
				done <= 1'b1;
				cs <= C_WAIT;
				qs <= Q_IDLE;
			end

			Q_L1: qs <= Q_L2;
			Q_L2: begin
				la <= q;
				m_raddr <= { 1'b1, lr[8:0], 1'b1 };
				qs <= Q_L3;
			end
			Q_L3: qs <= Q_L4;
			Q_L4: begin
				// { valid, slot, reg, data }
				rdo <= { 1'b1, 5'd0, la[8:7], 8'd0, la[6:0], q[8], q[7:0] };
				lr <= lr + 10'd1;
				done <= 1'b1;
				cs <= C_WAIT;
				qs <= Q_IDLE;
			end

			default: qs <= Q_IDLE;
			endcase

			// -- the I2C protocol --
			//
			// SDA only ever changes while SCL is low (just after a
			// filtered falling edge), so nothing here can look like a
			// start or a stop to the master.

			if (!live) begin
				ps <= P_IDLE;
				sda_low <= 1'b0;
			end else if (start) begin
				// a start, or a repeated start, from any state
				ps <= P_RECV;
				what <= B_ADDR;
				bitn <= 4'd0;
				sda_low <= 1'b0;
			end else if (stop) begin
				ps <= P_IDLE;
				sda_low <= 1'b0;
			end else begin
				case (ps)

				P_RECV: begin
					if (scl_rise) begin
						sh <= { sh[6:0], sda_f };
						bitn <= bitn + 4'd1;
					end else if (scl_fall && bitn == 4'd8) begin
						bitn <= 4'd0;
						case (what)
						B_ADDR: begin
							if (hit) begin
								slot <= hit_slot;
								sda_low <= 1'b1;
								ps <= P_ACK;
								after_send <= sh[0];
								what <= B_CMD;
								if (sh[0]) begin
									// a read starts at the pointer
									j_fetch <= 1'b1;
									j_slot <= hit_slot;
									j_reg <= s_ptr[hit_slot];
									s_rd[hit_slot] <= 1'b1;
									s_rdstart[hit_slot] <= s_ptr[hit_slot];
								end
							end else begin
								ps <= P_IDLE;		// not ours: NACK, by silence
							end
						end
						B_CMD: begin
							s_ptr[slot] <= sh;
							sda_low <= 1'b1;
							ps <= P_ACK;
							after_send <= 1'b0;
							what <= B_DATA;
						end
						default: begin				// B_DATA
							j_store <= 1'b1;
							j_slot <= slot;
							j_reg <= ptr_now;
							j_data <= sh;
							s_ptr[slot] <= ptr_next;
							sda_low <= 1'b1;
							ps <= P_ACK;
							after_send <= 1'b0;
						end
						endcase
					end
				end

				// our ACK lasts the 9th clock; at its falling edge either
				// the first bit of a read goes out, or SDA is released for
				// the master's next byte
				P_ACK: if (scl_fall) begin
					if (after_send) begin
						sda_low <= !tx[7];
						ssh <= { tx[6:0], 1'b0 };
						bitn <= 4'd1;
						ps <= P_SEND;
					end else begin
						sda_low <= 1'b0;
						bitn <= 4'd0;
						ps <= P_RECV;
					end
				end

				// bits 6..0 at the following falling edges; after the
				// eighth, SDA is released for the master's ACK, the
				// pointer advances and the next byte is fetched
				P_SEND: if (scl_fall) begin
					if (bitn == 4'd8) begin
						sda_low <= 1'b0;
						s_ptr[slot] <= ptr_next;
						j_fetch <= 1'b1;
						j_slot <= slot;
						j_reg <= ptr_next;
						ps <= P_MACK;
					end else begin
						sda_low <= !ssh[7];
						ssh <= { ssh[6:0], 1'b0 };
						bitn <= bitn + 4'd1;
					end
				end

				P_MACK: begin
					if (scl_rise) begin
						mack <= !sda_f;
					end else if (scl_fall) begin
						if (mack) begin
							sda_low <= !tx[7];
							ssh <= { tx[6:0], 1'b0 };
							bitn <= 4'd1;
							ps <= P_SEND;
						end else begin
							ps <= P_IDLE;		// NACK: the master is done
						end
					end
				end

				default: ;						// P_IDLE: wait for a start
				endcase
			end

		end
	end

endmodule
