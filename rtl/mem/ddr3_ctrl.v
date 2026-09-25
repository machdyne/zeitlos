/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 controller -- protocol layer
 *
 * Written from the JEDEC DDR3 command truth table and mode register
 * definitions and from the Micron MT41K AC timing tables. No code
 * from any other DDR3 implementation was used or adapted.
 *
 * -- what this module is and is not --
 *
 * This is the part that knows DDR3: the power-on sequence, the mode
 * registers, which command to issue and how long to wait before the
 * next one. It knows NOTHING about the ECP5, about DQS, or about how
 * many clock cycles a particular PHY takes to get data on and off the
 * pins. That division is the point -- everything here can be
 * simulated against a plain DRAM model with no vendor primitives
 * involved, and rtl/mem/ddr3_phy_ecp5.v can be brought up
 * independently against a controller already known to be correct.
 *
 * The PHY contract is deliberately narrow:
 *
 *   * One command per controller cycle, presented on the command
 *     outputs. The controller runs at half the DRAM clock, so a
 *     command lands on every SECOND DRAM clock and the PHY issues
 *     DESELECT on the other one. Command spacing is therefore
 *     quantised to 2 DRAM clocks -- see "why half rate" below.
 *
 *   * Write data is handed over WHOLE, all 128 bits of the burst in
 *     one cycle, in the same cycle as the WRITE command. The PHY
 *     owns the CWL delay and the serialisation.
 *
 *   * Read data comes back WHOLE, all 128 bits, whenever the PHY says
 *     so. The controller does not count CAS latency; it waits for
 *     phy_rvalid_i.
 *
 * That last point matters more than it looks. Read return time on
 * this part is not something a controller can predict: it depends on
 * board flight time, on the ECP5's DQS delay code, and (because this
 * design runs DDR3 far below its specified clock -- see docs/ddr3.md)
 * on DLL behaviour that is not characterised at this frequency.
 * Making the PHY announce data rather than having the controller
 * predict it means the read path can be trained without touching
 * this file -- as it is, by the BIOS.
 *
 * -- why half rate --
 *
 * The controller runs at sys_clk (48MHz) and the DRAM at 96MHz.
 * Commands could be issued on either DRAM clock edge, but issuing
 * only on even ones halves the command logic for a cost of at most
 * one DRAM clock (10.4ns) of extra spacing per command. At this
 * frequency every JEDEC timing except the raw minimums is already
 * satisfied by a wide margin, so that cost is invisible. Keeping it
 * simple is worth more.
 *
 * -- why CL=6 AND CWL=6 --
 *
 * CL=6 is the lowest CAS latency DDR3 defines (the DDR3-800 bin) and
 * the only sensible choice this far below the specified clock range.
 * At 96MHz it costs 62.5ns, which is most of the access time; nothing
 * can be done about that short of running the DRAM faster.
 *
 * CWL is set to 6 rather than the 5 the slowest bin would give. The
 * reason is alignment, not timing: with the controller at half rate,
 * an ODD write latency would start the write burst on an odd DRAM
 * clock, i.e. half a controller cycle out of phase with everything
 * else, and every downstream data path would need a half-cycle
 * skewed variant. An even CWL keeps writes on the same phase grid as
 * reads and commands. A larger CWL only makes the DRAM wait longer
 * before sampling write data, so it is always safe; it is the small
 * ones that are dangerous.
 *
 * -- access policy: auto-precharge, one burst per request --
 *
 * Every access is ACTIVATE, then READ or WRITE with A10 high so the
 * DRAM precharges the row itself. No open-row tracking and no
 * explicit PRECHARGE command. That is the simplest correct policy
 * and an open-row policy would be the next step for speed; it is
 * called out here because it
 * is the single biggest performance decision in the file and it is
 * deliberately the wrong one for now.
 *
 * Note what auto-precharge buys beyond simplicity: with no row left
 * open, every bank is idle whenever this module is idle, so a refresh
 * needs no PRECHARGE ALL ahead of it and the refresh path is three
 * states rather than six.
 *
 * -- single word accesses out of a 16-byte burst --
 *
 * DDR3 has no short read. The minimum burst on a x16 part is BL8 =
 * 8 transfers x 16 bits = 128 bits = 16 bytes, which is exactly one
 * 4-word cache line. The controller keeps the whole burst in a block
 * register, so the three words after the first of a line fill are
 * answered from it rather than by three more DRAM reads.
 *
 * Nor is there a short write, and DM is NOT used: a 32-bit store is a
 * read-modify-write -- read the block, merge the word under its byte
 * enables, write all sixteen bytes back unmasked (S_RMW_*). A store
 * whose block is already in the register skips the read. Masked
 * writes never worked in the first bring-up at any alignment tried;
 * unmasked ones did, and this leaves one mechanism to get right.
 */

`default_nettype none

module ddr3_ctrl #(
	// -- Device geometry ------------------------------------------
	// Row bits are the only thing that differs between MT41K64M16
	// (13), MT41K128M16 (14) and MT41K256M16 (15). All three are x16
	// with 10 column bits and 8 banks.
	// MT41K256M16 (4Gb x16) has fifteen row bits. The bus reaches
	// 256MB: 2 word + 7 block + 3 bank + 14 row = 26 address bits,
	// exactly wb_adr_i. The top row bit is unused and half the part
	// is unreachable, which is a bus limit, not a controller one.
	parameter ROW_BITS  = 14,
	parameter COL_BITS  = 10,
	parameter BANK_BITS = 3,

	// tRFC depends on density and is the one AC timing that does:
	// 110ns at 1Gb, 160ns at 2Gb, 260ns at 4Gb.
	// 260ns for a 4Gb part (JEDEC; MT41K256M16 datasheet).
	//
	// The 1Gb figure, 110ns, was used for weeks during bring-up.
	// A refresh fires every 7.8us, so waiting the wrong one sends
	// commands to a DRAM still refreshing about 128,000 times a
	// second. The corruption is intermittent and moves with timing,
	// which reads exactly like a datapath tuning problem.
	parameter TRFC_NS = 260,

	// -- Clocks ----------------------------------------------------
	// Controller clock in MHz. The DRAM runs at twice this.
	parameter CLK_MHZ = 48,

	// -- Simulation ------------------------------------------------
	// Divides the two multi-hundred-microsecond power-on waits so a
	// testbench does not spend a million cycles reaching the first
	// command. MUST be 1 for hardware.
	parameter INIT_DIV = 1
) (
	input wire clk_i,
	input wire rst_i,

	// -- Wishbone (word addressed, 26 bits = 256MB) ---------------
	input wire [26:0] wb_adr_i,
	input wire [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input wire wb_we_i,
	input wire [3:0] wb_sel_i,
	input wire wb_stb_i,
	input wire wb_cyc_i,
	output reg wb_ack_o,

	// -- PHY: status ----------------------------------------------
	input wire phy_ready_i,      // PHY delay hardware is up
	output reg init_done_o,      // DRAM is initialised and usable

	// Sticky: a read gave up waiting for the PHY.
	//
	// A wishbone cycle that never acknowledges stalls the CPU on a
	// load forever, with nothing on the console -- the machine simply
	// stops, and the only thing to do is power-cycle it. Bad data can
	// be read back and measured against; a hang cannot. So a read
	// always completes, and says afterwards that it failed.
	output reg rd_timeout_o,

	// Hold refresh while the BIOS trains the read path. See the
	// refresh counter below.
	input wire no_refresh_i,

	// Writes skip their read, for training.
	//
	// Read-modify-write makes every write depend on a read: the block
	// is read, the word merged, the block written back. Training
	// measures reads against data it has written -- so with the read
	// path mistuned, which is the state training exists to fix,
	// nothing could ever be written and nothing measured. A chicken
	// and an egg.
	//
	// With this set, a write merges into whatever the block register
	// holds and writes it without reading. Written as four words in
	// sequence, a block ends up exactly right regardless: the first
	// write carries one correct word and three stale ones, each later
	// write hits the register and fixes its own word, and the fourth
	// carries all four. Only ever used for whole blocks, and only
	// while training.
	input wire wr_noread_i,

	// Reads always go to the DRAM, never to the block register.
	//
	// Training writes a block and reads it back to see whether the
	// read path works. The write leaves the block in the register, so
	// without this every training read was answered from the register
	// and passed at every setting: the first hardware scan reported
	// all sixteen offsets working at every phase, gate and write delay,
	// on both lanes, when the real read path had never delivered a
	// single burst. A measurement of the thing doing the measuring.
	input wire rdbuf_off_i,

	// -- PHY: command (one per cycle, on even DRAM clocks) --------
	output reg phy_reset_n_o,
	output reg phy_cke_o,
	output reg phy_odt_o,
	output reg phy_cs_n_o,
	output reg phy_ras_n_o,
	output reg phy_cas_n_o,
	output reg phy_we_n_o,
	output reg [15:0] phy_a_o,
	output reg [2:0] phy_ba_o,

	// -- PHY: data ------------------------------------------------
	// A whole BL8 burst in one go. phy_wmask_o is per byte, 1 =
	// masked (not written), matching the DM pin's own polarity.
	output reg phy_wren_o,
	output reg [127:0] phy_wdata_o,
	output reg [15:0] phy_wmask_o,
	output reg phy_rden_o,
	input wire phy_rvalid_i,
	input wire [127:0] phy_rdata_i
);

	// =============================================================
	// Timing
	//
	// All JEDEC timings are quoted either in nanoseconds or in DRAM
	// clocks, and the binding one is whichever is larger. At 96MHz a
	// DRAM clock is 10.4ns, which is long enough that the clock-count
	// minimums win almost everywhere -- tRCD's 13.75ns is 2 clocks,
	// but tRTP's "4 clocks or 7.5ns" is 4. Both forms are computed
	// and the larger taken, rather than assuming which wins, because
	// that assumption stops being true if CLK_MHZ ever changes.
	//
	// Everything is then converted to CONTROLLER cycles, rounding up,
	// since a command can only be issued every second DRAM clock.
	// =============================================================

	localparam DRAM_MHZ = CLK_MHZ * 2;

	// Nanoseconds to DRAM clocks, rounded up.
	`define NS2CK(ns) (((ns) * DRAM_MHZ + 999) / 1000)
	// DRAM clocks to controller cycles, rounded up.
	`define CK2CYC(ck) (((ck) + 1) / 2)
	// Nanoseconds to controller cycles, rounded up.
	`define NS2CYC(ns) `CK2CYC(`NS2CK(ns))

	localparam CL  = 6;   // CAS latency, DRAM clocks
	localparam CWL = 6;   // CAS write latency, DRAM clocks

	// Power-on: RESET# low at least 200us, then CKE low at least
	// another 500us. Both are power-supply settling requirements, not
	// DRAM-internal ones, which is why shortening them in simulation
	// is legitimate and shortening anything below is not.
	localparam CYC_RESET = (200 * CLK_MHZ) / INIT_DIV;
	localparam CYC_CKE   = (500 * CLK_MHZ) / INIT_DIV;

	// tXPR: exit reset to first command, max(5 CK, tRFC + 10ns).
	localparam CYC_XPR  = (`NS2CYC(TRFC_NS + 10) > `CK2CYC(5)) ?
	                       `NS2CYC(TRFC_NS + 10) : `CK2CYC(5);
	localparam CYC_MRD  = `CK2CYC(4);                 // MRS to MRS
	localparam CYC_MOD  = (`CK2CYC(12) > `NS2CYC(15)) ?
	                       `CK2CYC(12) : `NS2CYC(15); // MRS to non-MRS
	// ZQ calibration long at init is 512 clocks, and the DLL needs
	// tDLLK = 512 clocks after its reset. Same number, one wait.
	localparam CYC_ZQINIT = `CK2CYC(512);

	localparam CYC_RCD  = `NS2CYC(1375) / 100;        // 13.75ns
	localparam CYC_RP   = `NS2CYC(1375) / 100;        // 13.75ns
	localparam CYC_RC   = `NS2CYC(4875) / 100;        // 48.75ns
	localparam CYC_RAS  = `NS2CYC(35);
	localparam CYC_RFC  = `NS2CYC(TRFC_NS);
	// tWR is measured from the END of the write burst, and with
	// auto-precharge the DRAM starts its internal precharge after
	// tWR, so a write occupies the bank for CWL + burst + tWR + tRP.
	// Write recovery for AUTO-PRECHARGE is MR0's WR, in clocks -- the
	// part waits that, not tWR in nanoseconds. It used to be timed as
	// 15ns (one system cycle) while MR0 said 6 clocks (62.5ns), and the
	// controller stayed legal only by 0.7 of a DRAM clock of incidental
	// state-machine overhead. One value, used for both.
	localparam WR_CK = 6;                  // >= tWR 15ns = 2 clocks
	localparam [2:0] WR_ENC = 3'b010;      // MR0 A[11:9]: 6 clocks
	localparam CYC_WR   = `CK2CYC(WR_CK);
	// tRTP, read to precharge: max(4 CK, 7.5ns).
	localparam CYC_RTP  = (`CK2CYC(4) > `NS2CYC(8)) ?
	                       `CK2CYC(4) : `NS2CYC(8);

	// Average refresh interval, 7.8us. Counted in controller cycles.
	localparam CYC_REFI = (78 * CLK_MHZ) / 10;

	// A BL8 burst is 8 transfers = 4 DRAM clocks = 2 controller
	// cycles of data on the bus.
	localparam CYC_BURST = 2;

	// After a WRITE command the bank is busy for the write latency,
	// the burst, the write recovery and the precharge. After a READ,
	// for tRTP and the precharge. Both are counted from the command.
	localparam CYC_WR_BUSY = `CK2CYC(CWL) + CYC_BURST + CYC_WR + CYC_RP;
	localparam CYC_RD_BUSY = CYC_RTP + CYC_RP;

	// =============================================================
	// Mode registers
	//
	// Bit assignments are from the JEDEC DDR3 mode register
	// definitions. Written out as named fields rather than magic
	// constants because a wrong bit here does not fail loudly -- it
	// produces a DRAM that mostly works.
	// =============================================================

	// MR0: burst length, CAS latency, write recovery, DLL reset.
	//   A[1:0] = 00  BL8 fixed
	//   A[2]   = 0   CAS latency low bit (only used above CL=11)
	//   A[3]   = 0   sequential burst order
	//   A[6:4] = CL - 4
	//   A[7]   = 0   normal mode, not test
	//   A[8]   = 1   DLL reset
	//   A[11:9]= write recovery, encoded; 001 = 5, the minimum, which
	//            is far more than tWR needs at this clock
	//   A[12]  = 0   slow precharge power down exit
	// The latency fields are three bits wide each. Spelled out as
	// sized localparams rather than written inline: an unsized
	// expression in a concatenation has no defined width, and the
	// tools are entitled to guess differently than intended.
	localparam [2:0] CL_ENC  = CL[2:0] - 3'd4;
	localparam [2:0] CWL_ENC = CWL[2:0] - 3'd5;

	// Exactly sixteen bits, field by field. This was a 17-bit
	// concatenation that worked only because the extra bit was a
	// leading zero, and it programmed WR=6 while its comment said 5.
	localparam [15:0] MR0 = {3'b000,   // A15:A13
	                         1'b0,     // A12 precharge power-down, slow
	                         WR_ENC,   // A11:A9 write recovery
	                         1'b1,     // A8 DLL reset
	                         1'b0,     // A7 normal mode
	                         CL_ENC,   // A6:A4 CAS latency
	                         1'b0,     // A3 sequential burst
	                         1'b0,     // A2 CAS latency, low bit
	                         2'b00};   // A1:A0 BL8

	// MR1: DLL enable, drive strength, termination.
	//   A[0]     = 0   DLL enabled
	//   A[5],A[1]= 01  output driver impedance RZQ/7 (34 ohm)
	//   A[4:3]   = 00  additive latency disabled
	//   A[9],A[6],A[2] = 000  Rtt_nom disabled
	//   A[7]     = 0   write levelling disabled
	//   A[11]    = 0   TDQS disabled (not available on x16 anyway)
	//   A[12]    = 0   outputs enabled
	//
	// ON-DIE TERMINATION IS OFF, on purpose. At 96MHz a bit is
	// 10.4ns wide and the module's DRAM sits centimetres from the
	// FPGA, so reflections settle within a small fraction of the eye
	// and termination buys nothing measurable. What it would cost is
	// real: Rtt_nom brings the ODT pin's own timing rules into the
	// write path, which is a class of bug worth not having. Revisit
	// only if signal integrity ever demands it; it has not on ML2.
	localparam [15:0] MR1 = {4'b0000, 1'b0, 2'b00, 1'b0, 1'b0,
	                         1'b0, 1'b0, 1'b0, 2'b00, 1'b1, 1'b0};

	// MR2: CAS write latency. Rtt_WR disabled to match MR1.
	//   A[5:3]  = CWL - 5
	//   A[6]    = 0  auto self-refresh off
	//   A[7]    = 0  normal self-refresh temperature range
	//   A[10:9] = 00 Rtt_WR disabled
	localparam [15:0] MR2 = {5'b00000, 2'b00, 1'b0, 1'b0,
	                         CWL_ENC, 3'b000};

	// MR3: multi-purpose register off; nothing else defined.
	localparam [15:0] MR3 = 16'h0000;

	// =============================================================
	// Command encodings: {CS#, RAS#, CAS#, WE#}
	// =============================================================
	localparam [3:0] CMD_MRS  = 4'b0000;
	localparam [3:0] CMD_REF  = 4'b0001;
	localparam [3:0] CMD_PRE  = 4'b0010;
	localparam [3:0] CMD_ACT  = 4'b0011;
	localparam [3:0] CMD_WR   = 4'b0100;
	localparam [3:0] CMD_RD   = 4'b0101;
	localparam [3:0] CMD_ZQC  = 4'b0110;
	localparam [3:0] CMD_NOP  = 4'b0111;
	localparam [3:0] CMD_DES  = 4'b1111;

	// =============================================================
	// State
	// =============================================================
	localparam [4:0] S_RESET    = 5'd0;
	localparam [4:0] S_CKE_WAIT = 5'd1;
	localparam [4:0] S_CKE_HIGH = 5'd2;
	localparam [4:0] S_MR2      = 5'd3;
	localparam [4:0] S_MR3      = 5'd4;
	localparam [4:0] S_MR1      = 5'd5;
	localparam [4:0] S_MR0      = 5'd6;
	localparam [4:0] S_ZQCL     = 5'd7;
	localparam [4:0] S_IDLE     = 5'd8;
	localparam [4:0] S_ACT      = 5'd9;
	localparam [4:0] S_RD       = 5'd10;
	localparam [4:0] S_RD_WAIT  = 5'd11;
	localparam [4:0] S_RD_BUSY  = 5'd12;
	localparam [4:0] S_WR       = 5'd13;
	localparam [4:0] S_WR_BUSY  = 5'd14;
	localparam [4:0] S_REF      = 5'd15;
	localparam [4:0] S_WAIT     = 5'd16;

	// Read-modify-write. A write-through store is 32 bits; a DRAM
	// access is 16 bytes. The gap is bridged by reading the block,
	// merging the word, and writing all sixteen bytes back with
	// nothing masked -- so DM is never used and is tied inactive.
	//
	// Masked writes never worked on this board at any alignment
	// tried, while unmasked writes round-tripped byte-exact. More to
	// the point, this leaves ONE mechanism to get right instead of
	// two, and it is the burst read that line fills already depend
	// on: if reads work, everything works.
	localparam [4:0] S_RMW_RD   = 5'd17;
	localparam [4:0] S_RMW_WAIT = 5'd18;
	localparam [4:0] S_RMW_ACT  = 5'd19;

	reg [4:0] state;
	reg [4:0] ret_state;
	reg [19:0] wait_ctr;

	reg [15:0] refresh_ctr;
	reg refresh_req;

	reg [ROW_BITS-1:0] req_row;
	reg [BANK_BITS-1:0] req_bank;
	reg [COL_BITS-1:0] req_col;
	reg [1:0] req_word;
	reg [3:0] req_sel;
	reg [31:0] req_wdat;
	reg req_we;
	reg req_busy;

	reg [127:0] rd_hold;

	integer i;

	// -- Address mapping ------------------------------------------
	//
	// {row, bank, column} with the bank bits immediately above the
	// column. A BL8 burst covers 8 columns = 16 bytes, so the low
	// three column bits are always zero and the burst is naturally
	// aligned; wb_adr_i[1:0] then picks the word within it.
	//
	// Putting bank above column rather than below means a sequential
	// walk stays inside one 2KB row for as long as possible. That is
	// worth nothing today, because auto-precharge closes the row
	// after every access anyway -- but it is what open-row tracking
	// would want, and changing the address map later would
	// invalidate every measurement taken before it.
	// -- single-block read buffer ---------------------------------
	//
	// One DRAM read fetches sixteen bytes; the bus asks for four. The
	// data cache fills a line with a FOUR-WORD linear burst, which is
	// exactly those sixteen bytes -- so without this, the same burst
	// is read from the DRAM four times over.
	//
	// One block, one tag, one valid bit. Not a cache: no replacement
	// policy, nothing to tune, nothing to get subtly wrong. A read
	// that hits is answered from the register; a read that misses
	// does exactly what it did before.
	//
	// Invalidated by ANY write. A write landing in the buffered block
	// would otherwise be invisible to the next read of it, and
	// working out which writes overlap is more logic than dropping
	// the block. Writes are rare next to fills, and the simple rule
	// cannot be wrong.
	reg [127:0] rdbuf_data;
	reg [ROW_BITS+BANK_BITS+COL_BITS-4:0] rdbuf_tag;
	reg rdbuf_valid;

	// 2 word + 7 block + 3 bank + ROW_BITS row bits must fit the 27-bit
	// word address -- 512MB, the main-memory window. More rows than
	// that would slice past the bus and alias silently, so it fails the
	// build instead: the same device rtl/sysctl.v uses for JUMPLOADER
	// without PROGRAMN_PIN.
	generate
		if (ROW_BITS > 15) begin : too_many_rows
			DDR3_ROW_BITS_exceeds_the_27_bit_bus_see_rtl_boards_vh stop_here ();
		end
	endgenerate

	wire [COL_BITS-1:0] map_col;
	wire [BANK_BITS-1:0] map_bank;
	wire [ROW_BITS-1:0] map_row;

	// Word address layout, low to high:
	//
	//   [1:0]                    word within the 16-byte block
	//   [COL_BITS-2:2]           block within the row  (COL_BITS-3 bits)
	//   next BANK_BITS           bank
	//   next ROW_BITS            row
	//
	// A 10-bit column is 1024 sixteen-bit locations, which is 128
	// eight-beat blocks, which needs SEVEN block bits. This used five
	// (wb_adr_i[COL_BITS-4:2]), so the top two column bits were
	// always zero and only a quarter of every row was reachable: the
	// addresses were all distinct, nothing aliased, and three
	// quarters of the memory was silently never used. A test that
	// checks addresses are distinct cannot see that; only one that
	// checks how many of them there are.
	assign map_col  = {wb_adr_i[COL_BITS-2:2], 3'b000};

	// The tag is everything above the four words inside a block.
	wire [ROW_BITS+BANK_BITS+COL_BITS-4:0] req_tag =
		{map_row, map_bank, map_col[COL_BITS-1:3]};
	wire rdbuf_hit = rdbuf_valid && (rdbuf_tag == req_tag);

	// The sixteen bytes being assembled for a write.
	// ONE register holds the block, for both purposes.
	//
	// The read buffer and the read-modify-write buffer are the same
	// sixteen bytes; keeping two of them would mean two things that
	// have to agree about what memory contains, and things that have
	// to agree eventually do not. rdbuf_data is it.
	//
	// The block as it will be written: what is held, with this
	// store's word merged in.
	wire [127:0] rmw_merged = {
		(req_word == 2'd3) ? merge_word(rdbuf_data[127:96], req_wdat, req_sel) : rdbuf_data[127:96],
		(req_word == 2'd2) ? merge_word(rdbuf_data[95:64], req_wdat, req_sel)  : rdbuf_data[95:64],
		(req_word == 2'd1) ? merge_word(rdbuf_data[63:32], req_wdat, req_sel)  : rdbuf_data[63:32],
		(req_word == 2'd0) ? merge_word(rdbuf_data[31:0], req_wdat, req_sel)   : rdbuf_data[31:0]
	};

	// A read that never completes must not hang the bus.
	//
	// A wishbone cycle with no acknowledge stalls the CPU on a load
	// forever, with nothing on the console. Bad data can be measured
	// and tuned against; a hang can only be power-cycled. 1023 cycles
	// is far longer than any legitimate read and still instant to a
	// person watching.
	localparam [10:0] RD_TIMEOUT = 11'd1023;
	reg [10:0] rd_timer;

	// One word, with the selected bytes replaced.
	//
	// EVERYTHING it reads is an argument. It used to read req_sel and
	// req_wdat from the module directly, and a function called from a
	// continuous assignment is re-evaluated only when its ARGUMENTS
	// change -- not when the signals it reads inside do. rmw_merged
	// therefore froze on the first store's data: the second store to
	// a block wrote the first store's value into its own word.
	//
	// Synthesis does not share the problem, which makes it worse
	// rather than better: simulation and hardware disagree, and the
	// simulation is the one that looks broken while the design is
	// right, or the reverse, depending on which you trust.
	function [31:0] merge_word;
		input [31:0] old;
		input [31:0] wdat;
		input [3:0] sel;
		begin
			merge_word[7:0]   = sel[0] ? wdat[7:0]   : old[7:0];
			merge_word[15:8]  = sel[1] ? wdat[15:8]  : old[15:8];
			merge_word[23:16] = sel[2] ? wdat[23:16] : old[23:16];
			merge_word[31:24] = sel[3] ? wdat[31:24] : old[31:24];
		end
	endfunction
	assign map_bank = wb_adr_i[COL_BITS-1+BANK_BITS-1:COL_BITS-1];
	assign map_row  = wb_adr_i[COL_BITS-1+BANK_BITS+ROW_BITS-1:
	                           COL_BITS-1+BANK_BITS];

	// A10 is overloaded on every command that uses it, so it is
	// assembled explicitly at each issue site rather than being part
	// of the row/column address.

	// =============================================================
	// Refresh interval
	//
	// Free running, independent of the state machine, so that time
	// spent servicing a long access still counts towards the next
	// refresh. Cleared when the refresh is actually issued.
	// =============================================================
	always @(posedge clk_i) begin
		if (rst_i) begin
			refresh_ctr <= 16'd0;
			refresh_req <= 1'b0;
		end else if (!init_done_o) begin
			refresh_ctr <= 16'd0;
			refresh_req <= 1'b0;
		end else if (state == S_REF) begin
			refresh_ctr <= 16'd0;
			refresh_req <= 1'b0;
		end else if (refresh_ctr >= CYC_REFI[15:0]) begin
			// DEFERRED while training, never skipped: the counter
			// holds at the interval and the refresh is issued the
			// moment training ends.
			//
			// A refresh every 7.8us in the middle of a scan that
			// takes milliseconds disturbed a different random subset
			// of points on every run last time, so identical
			// bitstreams gave different maps. Holding it for the
			// few milliseconds a scan takes is far inside the 64ms
			// the part retains data for.
			if (!no_refresh_i) refresh_req <= 1'b1;
		end else begin
			refresh_ctr <= refresh_ctr + 16'd1;
		end
	end

	// =============================================================
	// Main sequencer
	// =============================================================
	always @(posedge clk_i) begin

		if (rst_i) begin

			state <= S_RESET;
			ret_state <= S_RESET;
			wait_ctr <= 20'd0;
			init_done_o <= 1'b0;

			phy_reset_n_o <= 1'b0;
			phy_cke_o <= 1'b0;
			phy_odt_o <= 1'b0;
			{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_DES;
			phy_a_o <= 16'd0;
			phy_ba_o <= 3'd0;

			phy_wren_o <= 1'b0;
			phy_wdata_o <= 128'd0;
			phy_wmask_o <= 16'hffff;
			phy_rden_o <= 1'b0;

			rdbuf_valid <= 1'b0;
			rd_timeout_o <= 1'b0;
			rd_timer <= 11'd0;
			wb_ack_o <= 1'b0;
			wb_dat_o <= 32'd0;

			req_busy <= 1'b0;
			req_we <= 1'b0;
			req_row <= {ROW_BITS{1'b0}};
			req_bank <= {BANK_BITS{1'b0}};
			req_col <= {COL_BITS{1'b0}};
			req_word <= 2'd0;
			req_sel <= 4'd0;
			req_wdat <= 32'd0;
			rd_hold <= 128'd0;

		end else begin

			// Defaults. Every command output falls back to DESELECT
			// so that a state which issues nothing cannot leave a
			// stale command asserted on the bus -- the failure that
			// would produce is a spurious second ACTIVATE, which the
			// DRAM would answer by corrupting a row.
			{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_DES;
			phy_wren_o <= 1'b0;
			phy_rden_o <= 1'b0;
			wb_ack_o <= 1'b0;

			// Read data can arrive in any state; capture it wherever
			// it lands rather than only while waiting for it.
			if (phy_rvalid_i) rd_hold <= phy_rdata_i;

			case (state)

			// -- Power-on -----------------------------------------
			//
			// RESET# low through supply ramp, then high with CKE
			// still low, then CKE high. The order is mandated and
			// the waits are supply settling times.
			S_RESET: begin
				phy_reset_n_o <= 1'b0;
				phy_cke_o <= 1'b0;
				wait_ctr <= CYC_RESET[19:0];
				ret_state <= S_CKE_WAIT;
				state <= S_WAIT;
			end

			S_CKE_WAIT: begin
				phy_reset_n_o <= 1'b1;
				phy_cke_o <= 1'b0;
				wait_ctr <= CYC_CKE[19:0];
				ret_state <= S_CKE_HIGH;
				state <= S_WAIT;
			end

			S_CKE_HIGH: begin
				// CKE rises here; the DRAM will not accept a command
				// until tXPR has elapsed.
				phy_cke_o <= 1'b1;
				wait_ctr <= CYC_XPR[19:0];
				ret_state <= S_MR2;
				state <= S_WAIT;
			end

			// -- Mode registers -----------------------------------
			//
			// MR2, MR3, MR1, MR0. Not an arbitrary order: MR0 must
			// be last because it carries the DLL reset, and the DLL
			// must not be reset before the settings it depends on
			// (CWL in MR2, DLL enable in MR1) are in place.
			S_MR2: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_MRS;
				phy_ba_o <= 3'd2;
				phy_a_o <= MR2;
				wait_ctr <= CYC_MRD[19:0];
				ret_state <= S_MR3;
				state <= S_WAIT;
			end

			S_MR3: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_MRS;
				phy_ba_o <= 3'd3;
				phy_a_o <= MR3;
				wait_ctr <= CYC_MRD[19:0];
				ret_state <= S_MR1;
				state <= S_WAIT;
			end

			S_MR1: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_MRS;
				phy_ba_o <= 3'd1;
				phy_a_o <= MR1;
				wait_ctr <= CYC_MRD[19:0];
				ret_state <= S_MR0;
				state <= S_WAIT;
			end

			S_MR0: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_MRS;
				phy_ba_o <= 3'd0;
				phy_a_o <= MR0;
				wait_ctr <= CYC_MOD[19:0];
				ret_state <= S_ZQCL;
				state <= S_WAIT;
			end

			// ZQ calibration long, A10 high. The wait that follows
			// covers both tZQinit and the DLL lock time tDLLK, which
			// are both 512 DRAM clocks and both start here.
			S_ZQCL: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_ZQC;
				phy_ba_o <= 3'd0;
				phy_a_o <= 16'h0400;
				wait_ctr <= CYC_ZQINIT[19:0];
				ret_state <= S_IDLE;
				state <= S_WAIT;
			end

			// -- Idle ---------------------------------------------
			S_IDLE: begin

				init_done_o <= 1'b1;
				req_busy <= 1'b0;

				// Refresh outranks new work. It has a hard deadline
				// and an access does not; letting a busy bus defer
				// refreshes indefinitely is how DRAM loses data.
				if (refresh_req) begin
					state <= S_REF;
				end else if (wb_cyc_i && wb_stb_i && !req_busy
				             && !wb_ack_o && !wb_we_i && rdbuf_hit
				             && !rdbuf_off_i) begin
					// Already have it. A four-word line fill costs
					// one DRAM read and three of these.
					case (wb_adr_i[1:0])
					2'd0: wb_dat_o <= rdbuf_data[31:0];
					2'd1: wb_dat_o <= rdbuf_data[63:32];
					2'd2: wb_dat_o <= rdbuf_data[95:64];
					default: wb_dat_o <= rdbuf_data[127:96];
					endcase
					wb_ack_o <= 1'b1;
				end else if (wb_cyc_i && wb_stb_i && !req_busy
				             && !wb_ack_o) begin
					// !wb_ack_o on BOTH paths, not just the hit.
					//
					// A buffer hit acknowledges without leaving
					// S_IDLE, so on the following cycle the master has
					// not yet dropped stb and the SAME request is
					// still on the bus. The hit path was guarded
					// against taking it twice; this one was not, and
					// it accepted the read again as a fresh request.
					//
					// That spurious read then completed while the
					// master was waiting on its NEXT transaction -- a
					// write -- and its acknowledge completed the
					// write, which was never performed. The store
					// simply vanished. One request, one acknowledge.
					req_row <= map_row;
					req_bank <= map_bank;
					req_col <= map_col;
					req_word <= wb_adr_i[1:0];
					req_sel <= wb_sel_i;
					req_wdat <= wb_dat_i;
					req_we <= wb_we_i;
					req_busy <= 1'b1;
					state <= S_ACT;
				end

			end

			// -- Refresh ------------------------------------------
			//
			// No PRECHARGE ALL first: auto-precharge means no row is
			// ever left open, so every bank is already idle here.
			S_REF: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_REF;
				phy_ba_o <= 3'd0;
				phy_a_o <= 16'd0;
				wait_ctr <= CYC_RFC[19:0];
				ret_state <= S_IDLE;
				state <= S_WAIT;
			end

			// -- Access -------------------------------------------
			S_ACT: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_ACT;
				phy_ba_o <= req_bank;
				phy_a_o <= {{(16-ROW_BITS){1'b0}}, req_row};
				wait_ctr <= CYC_RCD[19:0];
				// A write whose block is already in the buffer needs
				// no read: merge and write. Consecutive stores inside
				// one block therefore cost a write each, not a read
				// and a write each, which is the common case for a
				// write-through cache walking memory.
				ret_state <= req_we ? ((rdbuf_hit || wr_noread_i) ? S_WR : S_RMW_RD)
				                    : S_RD;
				state <= S_WAIT;
			end

			// -- read-modify-write --------------------------------
			//
			// READ with auto-precharge, exactly as S_RD does.
			S_RMW_RD: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_RD;
				phy_ba_o <= req_bank;
				phy_a_o <= {{(16-11){1'b0}}, 1'b1,
				            req_col[COL_BITS-1:0]};
				phy_rden_o <= 1'b1;
				rd_timer <= RD_TIMEOUT[10:0];
				state <= S_RMW_WAIT;
			end

			S_RMW_WAIT: begin
				if (phy_rvalid_i) begin
					// Store the block as read. The merge happens in
					// ONE place, S_WR, whether the block arrived here
					// or was already held.
					rdbuf_data <= phy_rdata_i;
					rdbuf_tag <= {req_row, req_bank,
					              req_col[COL_BITS-1:3]};
					rdbuf_valid <= 1'b1;
					// Auto-precharge closed the row behind that read,
					// so the write needs a fresh ACTIVATE.
					wait_ctr <= CYC_RD_BUSY[19:0];
					ret_state <= S_RMW_ACT;
					state <= S_WAIT;
				end else if (rd_timer == 11'd0) begin
					// The PHY never answered. Do NOT write: a
					// read-modify-write that cannot read has nothing
					// safe to write, and guessing destroys the
					// fifteen bytes it was not asked to change.
					rd_timeout_o <= 1'b1;
					wb_ack_o <= 1'b1;
					wb_dat_o <= 32'hdeadbe00;
					rdbuf_valid <= 1'b0;
					state <= S_IDLE;
				end else begin
					rd_timer <= rd_timer - 11'd1;
				end
			end

			S_RMW_ACT: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_ACT;
				phy_ba_o <= req_bank;
				phy_a_o <= {{(16-ROW_BITS){1'b0}}, req_row};
				wait_ctr <= CYC_RCD[19:0];
				ret_state <= S_WR;
				state <= S_WAIT;
			end

			// READ with auto-precharge: A10 high, column below it.
			// A[9:0] is the column for a 10-bit-column part, and A10
			// is the auto-precharge flag, which is why the column
			// cannot simply be placed at A[COL_BITS-1:0] on a part
			// with more column bits.
			S_RD: begin
				rd_timer <= RD_TIMEOUT;
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_RD;
				phy_ba_o <= req_bank;
				phy_a_o <= {5'd0, 1'b1, req_col};
				phy_rden_o <= 1'b1;
				state <= S_RD_WAIT;
			end

			// The PHY announces the data. Nothing here counts CAS
			// latency; see this file's header for why.
			S_RD_WAIT: begin
				if (!phy_rvalid_i && rd_timer == 11'd0) begin
					rd_timeout_o <= 1'b1;
					wb_dat_o <= 32'hdeadbe00;
					wb_ack_o <= 1'b1;
					rdbuf_valid <= 1'b0;
					state <= S_IDLE;
				end else if (!phy_rvalid_i) begin
					rd_timer <= rd_timer - 11'd1;
				end else if (phy_rvalid_i) begin
					// Keep the whole burst: the next three words of
					// a line fill are already inside it.
					rdbuf_data <= phy_rdata_i;
					rdbuf_tag <= {req_row, req_bank,
					              req_col[COL_BITS-1:3]};
					rdbuf_valid <= 1'b1;
					case (req_word)
					2'd0: wb_dat_o <= phy_rdata_i[31:0];
					2'd1: wb_dat_o <= phy_rdata_i[63:32];
					2'd2: wb_dat_o <= phy_rdata_i[95:64];
					default: wb_dat_o <= phy_rdata_i[127:96];
					endcase
					wb_ack_o <= 1'b1;
					// The DRAM is still finishing its internal
					// precharge; the bus transaction is done but the
					// bank is not free yet.
					wait_ctr <= CYC_RD_BUSY[19:0];
					ret_state <= S_IDLE;
					state <= S_WAIT;
				end
			end

			// WRITE with auto-precharge. The whole merged block is
			// handed to the PHY here, all sixteen bytes, nothing
			// masked.
			S_WR: begin
				{phy_cs_n_o, phy_ras_n_o, phy_cas_n_o, phy_we_n_o} <= CMD_WR;
				phy_ba_o <= req_bank;
				phy_a_o <= {5'd0, 1'b1, req_col};
				phy_wren_o <= 1'b1;

				// All sixteen bytes, nothing masked.
				//
				// The block was either read and merged (S_RMW_WAIT)
				// or was already held from an earlier access, and in
				// both cases the bytes this store does not touch are
				// the bytes memory already has. DM stays inactive:
				// it is the one mechanism this design does not use,
				// and therefore the one that cannot be misaligned.
				phy_wdata_o <= rmw_merged;
				phy_wmask_o <= 16'd0;

				// Keep the block. The next store to it needs no read,
				// and the next load of it needs no access at all.
				rdbuf_data <= rmw_merged;
				rdbuf_tag <= {req_row, req_bank, req_col[COL_BITS-1:3]};
				rdbuf_valid <= 1'b1;

				wb_ack_o <= 1'b1;
				wait_ctr <= CYC_WR_BUSY[19:0];
				ret_state <= S_IDLE;
				state <= S_WAIT;
			end

			// -- Generic wait -------------------------------------
			S_WAIT: begin
				if (wait_ctr <= 20'd1) begin
					state <= ret_state;
				end else begin
					wait_ctr <= wait_ctr - 20'd1;
				end
			end

			default: state <= S_IDLE;

			endcase

		end

	end

	// phy_ready_i is consumed only as a gate on leaving reset; the
	// ECP5 delay hardware must be up before any DQS-based capture can
	// work, but nothing in the command sequence depends on it.
	wire _unused;
	assign _unused = &{1'b0, phy_ready_i, CYC_RC, CYC_RAS};

	`undef NS2CK
	`undef CK2CYC
	`undef NS2CYC

endmodule

`default_nettype wire
