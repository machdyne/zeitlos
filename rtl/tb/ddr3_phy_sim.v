/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 PHY -- SIMULATION ONLY, NOT SYNTHESISABLE
 *
 * Stands in for rtl/mem/ddr3_phy_ecp5.v so that rtl/mem/ddr3_ctrl.v
 * can be verified against a DRAM model without any vendor primitives,
 * DQS delay hardware or trained delay codes in the way. It implements
 * the same contract the real PHY will: whole-burst data in and out,
 * and a read valid signal the controller waits on rather than
 * predicts.
 *
 * Behavioural throughout, using delays rather than a gearbox. That is
 * the point -- if this were structured like the ECP5 datapath it
 * would share the ECP5 datapath's bugs, and a controller verified
 * against it would prove nothing about the controller.
 *
 * -- command timing --
 *
 * The controller holds a command for one controller cycle, which is
 * two DRAM clocks, so presenting it directly to the DRAM would have
 * the DRAM sample it twice and see two ACTIVATEs where one was meant.
 * A rising edge detect on CS# in the DRAM clock domain narrows it to
 * a single DRAM clock, with DESELECT either side. Nothing here needs
 * to know which clock phase the controller is on, which is one less
 * thing to get subtly wrong.
 */

`timescale 1ps/1ps
`default_nettype none

module ddr3_phy_sim #(
	parameter TCK_PS = 10417,
	parameter CL  = 6,
	parameter CWL = 6
) (
	input wire clk_i,        // controller clock
	input wire ck_i,         // DRAM clock, twice clk_i
	input wire rst_i,

	// -- controller side ------------------------------------------
	input wire ctl_reset_n_i,
	input wire ctl_cke_i,
	input wire ctl_odt_i,
	input wire ctl_cs_n_i,
	input wire ctl_ras_n_i,
	input wire ctl_cas_n_i,
	input wire ctl_we_n_i,
	input wire [15:0] ctl_a_i,
	input wire [2:0] ctl_ba_i,

	input wire ctl_wren_i,
	input wire [127:0] ctl_wdata_i,
	input wire [15:0] ctl_wmask_i,
	input wire ctl_rden_i,
	output reg ctl_rvalid_o,
	output reg [127:0] ctl_rdata_o,

	// -- DRAM side ------------------------------------------------
	output reg ddr_reset_n,
	output reg ddr_cke,
	output reg ddr_odt,
	output reg ddr_cs_n,
	output reg ddr_ras_n,
	output reg ddr_cas_n,
	output reg ddr_we_n,
	output reg [15:0] ddr_a,
	output reg [2:0] ddr_ba,
	output reg [1:0] ddr_dm,
	inout wire [15:0] ddr_dq,
	inout wire [1:0] ddr_dqs
);

	reg cs_low_d;
	reg issue;

	reg rd_go;
	reg wr_go;
	reg [127:0] wr_data_lat;
	reg [15:0] wr_mask_lat;

	reg [15:0] dq_drv;
	reg [1:0] dqs_drv;
	reg drv_en;

	integer i;
	integer rb;
	integer wb;

	assign ddr_dq  = drv_en ? dq_drv : 16'hzzzz;
	assign ddr_dqs = drv_en ? dqs_drv : 2'bzz;

	initial begin
		drv_en = 1'b0;
		dq_drv = 16'd0;
		dqs_drv = 2'b00;
		ddr_dm = 2'b11;
		rd_go = 1'b0;
		wr_go = 1'b0;
		ctl_rvalid_o = 1'b0;
		ctl_rdata_o = 128'd0;
		cs_low_d = 1'b0;
		issue = 1'b0;
	end

	// -- level signals pass straight through ----------------------
	always @(*) begin
		ddr_reset_n = ctl_reset_n_i;
		ddr_cke = ctl_cke_i;
		ddr_odt = ctl_odt_i;
	end

	// -- command narrowing ----------------------------------------
	//
	// Present a real command for exactly one DRAM clock, on the edge
	// after CS# is first seen low. The DRAM samples what is on the
	// pins at the following rising edge, so the command lands one
	// DRAM clock after `issue` is set.
	always @(posedge ck_i) begin

		cs_low_d <= (ctl_cs_n_i === 1'b0);
		issue <= (ctl_cs_n_i === 1'b0) && !cs_low_d;

		if ((ctl_cs_n_i === 1'b0) && !cs_low_d) begin
			ddr_cs_n <= 1'b0;
			ddr_ras_n <= ctl_ras_n_i;
			ddr_cas_n <= ctl_cas_n_i;
			ddr_we_n <= ctl_we_n_i;
			ddr_a <= ctl_a_i;
			ddr_ba <= ctl_ba_i;
			if (ctl_rden_i) rd_go <= ~rd_go;
			if (ctl_wren_i) begin
				wr_data_lat <= ctl_wdata_i;
				wr_mask_lat <= ctl_wmask_i;
				wr_go <= ~wr_go;
			end
		end else begin
			ddr_cs_n <= 1'b1;
			ddr_ras_n <= 1'b1;
			ddr_cas_n <= 1'b1;
			ddr_we_n <= 1'b1;
		end

	end

	// -- write burst ----------------------------------------------
	//
	// One DRAM clock to reach the pins, then CWL to the first beat.
	// DQS is driven low for a clock ahead of the burst (the write
	// preamble) and toggles centred on each beat, which is where a
	// DDR3 part expects it on a write.
	always @(wr_go) begin
		#(TCK_PS);                          // command reaches the DRAM
		#((CWL - 1) * TCK_PS);              // preamble starts here
		drv_en = 1'b1;
		dqs_drv = 2'b00;
		dq_drv = 16'd0;
		ddr_dm = 2'b11;
		#(TCK_PS - (TCK_PS / 4));
		for (wb = 0; wb < 8; wb = wb + 1) begin
			dq_drv = wr_data_lat[wb*16 +: 16];
			ddr_dm = wr_mask_lat[wb*2 +: 2];
			#(TCK_PS / 4);
			dqs_drv = {2{wb[0] ^ 1'b1}};    // centre aligned to data
			#(TCK_PS / 4);
		end
		#(TCK_PS / 4);
		dqs_drv = 2'b00;                    // postamble
		#(TCK_PS / 4);
		drv_en = 1'b0;
		ddr_dm = 2'b11;
	end

	// -- read burst -----------------------------------------------
	//
	// The DRAM drives DQS edge aligned with the data, so sampling is
	// done a quarter of a beat later, in the middle of the eye. On
	// hardware that quarter-beat shift is what the ECP5's DQS delay
	// hardware produces and what has to be trained; here it is just a
	// delay.
	// -- read capture, on the STROBE ---------------------------
	//
	// The DRAM drives DQS edge-aligned with its data; the receiver
	// samples a quarter beat after each genuine 0/1 transition, in the
	// middle of the eye. That is what the ECP5's DQS delay does in
	// hardware, and it is what this does now.
	//
	// This used to count time from the controller's READ command. The
	// model counts from the DRAM bus, and the two origins differ, so
	// the capture landed a beat off. For a long time that was hidden:
	// the WRITE path had an equal and opposite error, and the two
	// cancelled for every pattern whose halves matched. Fixing the
	// write exposed the read. Two errors that cancel look like no
	// error, until one of them is fixed.
	//
	// Only transitions between REAL levels count. The preamble takes
	// the strobe from high impedance to 0, which Verilog calls a
	// negedge; counting it shifts every beat one place.
	reg rd_active;
	integer rd_beat;
	reg dqs_last;
	always @(ddr_dqs[0]) dqs_last <= ddr_dqs[0];

	always @(rd_go) begin
		rd_beat = 0;
		rd_active = 1'b1;
	end

	always @(posedge ddr_dqs[0] or negedge ddr_dqs[0]) begin
		if (rd_active && (ddr_dqs[0] === 1'b0 || ddr_dqs[0] === 1'b1)
		    && (dqs_last === 1'b0 || dqs_last === 1'b1)) begin
			#(TCK_PS / 4);
			ctl_rdata_o[rd_beat*16 +: 16] = ddr_dq;
			rd_beat = rd_beat + 1;
			if (rd_beat == 8) begin
				rd_active = 1'b0;
				@(posedge clk_i);
				ctl_rvalid_o = 1'b1;
				@(posedge clk_i);
				ctl_rvalid_o = 1'b0;
			end
		end
	end

endmodule

`default_nettype wire
