/*
 * ddr3_ecp5_stubs.v -- empty stand-ins for the ECP5 DDR primitives.
 *
 * Port-for-port copies of the declarations in yosys's
 * share/ecp5/cells_bb.v, with no behaviour. They let the DDR3
 * subsystem ELABORATE in simulation so logic around the primitives --
 * the register block in tb_ddr3_reg.v -- can be tested. They say
 * nothing about the primitives themselves, which have no simulation
 * models.
 */
`timescale 1ns/1ps
module DQSBUFM #(parameter DQS_LI_DEL_ADJ = "FACTORYONLY", parameter DQS_LI_DEL_VAL = 0, parameter DQS_LO_DEL_ADJ = "FACTORYONLY", parameter DQS_LO_DEL_VAL = 0, parameter GSR = "ENABLED") (DQSI, READ1, READ0, READCLKSEL2, READCLKSEL1, READCLKSEL0, DDRDEL, ECLK, SCLK, DYNDELAY7, DYNDELAY6, DYNDELAY5, DYNDELAY4, DYNDELAY3, DYNDELAY2, DYNDELAY1, DYNDELAY0, RST, RDLOADN, RDMOVE, RDDIRECTION, WRLOADN, WRMOVE, WRDIRECTION, PAUSE, DQSR90, DQSW, DQSW270, RDPNTR2, RDPNTR1, RDPNTR0, WRPNTR2, WRPNTR1, WRPNTR0, DATAVALID, BURSTDET, RDCFLAG, WRCFLAG);
	input DQSI;
	input READ1;
	input READ0;
	input READCLKSEL2;
	input READCLKSEL1;
	input READCLKSEL0;
	input DDRDEL;
	input ECLK;
	input SCLK;
	input DYNDELAY7;
	input DYNDELAY6;
	input DYNDELAY5;
	input DYNDELAY4;
	input DYNDELAY3;
	input DYNDELAY2;
	input DYNDELAY1;
	input DYNDELAY0;
	input RST;
	input RDLOADN;
	input RDMOVE;
	input RDDIRECTION;
	input WRLOADN;
	input WRMOVE;
	input WRDIRECTION;
	input PAUSE;
	output DQSR90;
	output DQSW;
	output DQSW270;
	output RDPNTR2;
	output RDPNTR1;
	output RDPNTR0;
	output WRPNTR2;
	output WRPNTR1;
	output WRPNTR0;
	output DATAVALID;
	output BURSTDET;
	output RDCFLAG;
	output WRCFLAG;
endmodule
module IDDRX2DQA #(parameter GSR = "ENABLED") (D, DQSR90, ECLK, SCLK, RST, RDPNTR2, RDPNTR1, RDPNTR0, WRPNTR2, WRPNTR1, WRPNTR0, Q0, Q1, Q2, Q3, QWL);
	input D;
	input DQSR90;
	input ECLK;
	input SCLK;
	input RST;
	input RDPNTR2;
	input RDPNTR1;
	input RDPNTR0;
	input WRPNTR2;
	input WRPNTR1;
	input WRPNTR0;
	output Q0;
	output Q1;
	output Q2;
	output Q3;
	output QWL;
endmodule
module ODDRX2DQA #(parameter GSR = "ENABLED") (D0, D1, D2, D3, RST, ECLK, SCLK, DQSW270, Q);
	input D0;
	input D1;
	input D2;
	input D3;
	input RST;
	input ECLK;
	input SCLK;
	input DQSW270;
	output Q;
endmodule
module ODDRX2DQSB #(parameter GSR = "ENABLED") (D0, D1, D2, D3, RST, ECLK, SCLK, DQSW, Q);
	input D0;
	input D1;
	input D2;
	input D3;
	input RST;
	input ECLK;
	input SCLK;
	input DQSW;
	output Q;
endmodule
module TSHX2DQA #(parameter GSR = "ENABLED", parameter REGSET = "SET") (T0, T1, SCLK, ECLK, DQSW270, RST, Q);
	input T0;
	input T1;
	input SCLK;
	input ECLK;
	input DQSW270;
	input RST;
	output Q;
endmodule
module TSHX2DQSA #(parameter GSR = "ENABLED", parameter REGSET = "SET") (T0, T1, SCLK, ECLK, DQSW, RST, Q);
	input T0;
	input T1;
	input SCLK;
	input ECLK;
	input DQSW;
	input RST;
	output Q;
endmodule
module ODDRX2F #(parameter GSR = "ENABLED") (SCLK, ECLK, RST, D0, D1, D2, D3, Q);
	input SCLK;
	input ECLK;
	input RST;
	input D0;
	input D1;
	input D2;
	input D3;
	output Q;
endmodule
module DELAYG #(parameter DEL_MODE = "USER_DEFINED", parameter DEL_VALUE = 0) (A, Z);
	input A;
	output Z;
endmodule
module TRELLIS_IO #(parameter DIR="INPUT") (inout B, input I, input T, output O); endmodule
