// UNIVERSAL CONFIG
// ----------------

// ...

// BOARD CONFIG
// ------------

`ifdef BOARD_OBST

`define FPGA_ECP5
`define OSC48
`define SYSCLK48
`define MEM_SRAM
//`define MEM_QQSPI
`define LED_RGB
//`define LED_DEBUG

`elsif BOARD_LAKRITZ

`define FPGA_ECP5
`define OSC48
`define SYSCLK48
`define MEM_SDRAM

`endif
