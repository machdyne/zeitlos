// UNIVERSAL CONFIG
// ----------------

`define DEBUG
`define ARBITER

// BOARD CONFIG
// ------------

`ifdef BOARD_OBST

`define FPGA_ECP5
`define OSC48
`define SYSCLK48
`define MEM_SRAM
`define MEM_VRAM
//`define MEM_QQSPI
`define MEM_ROM
`define MEM_GLYPH
`define LED_RGB
//`define LED_DEBUG
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
`define SPI_SDCARD
`define SPI_ETH

`elsif BOARD_LAKRITZ

`define FPGA_ECP5
`define OSC48
`define SYSCLK48
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HID
`define SPI_SDCARD

`elsif BOARD_MOZART_ML1

`define FPGA_ECP5
`define OSC48
`define SYSCLK48
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HID
`define SPI_SDCARD

`elsif BOARD_LEBKUCHEN

`define FPGA_GATEMATE
`define OSC48
`define SYSCLK48
//`define MEM_QQSPI
//`define MEM_QQSPI_SINGLE
`define MEM_VRAM
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
`define SPI_SDCARD
`define SPI_FLASH

`elsif BOARD_KOLSCH

`define FPGA_GATEMATE
`define OSC48
`define SYSCLK48
`define MEM_SDRAM
`define MEM_VRAM
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
//`define SPI_SDCARD
//`define SPI_FLASH

`endif
