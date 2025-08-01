#ifndef ZEITLOS_H
#define ZEITLOS_H

#include <stdint.h>
#include "zobj.h"

typedef uint32_t *(*z_kernel_ptr_t)(uint32_t, uint32_t *, uint32_t);

#define reg_kernel (*(volatile uint32_t*)0x0000000c)

#define reg_uart0_data (*(volatile uint8_t*)0xf0000000)
#define reg_uart0_dlbl (*(volatile uint8_t*)0xf0000000)
#define reg_uart0_dlbh (*(volatile uint8_t*)0xf0000004)
#define reg_uart0_ier (*(volatile uint8_t*)0xf0000004)
#define reg_uart0_fcr (*(volatile uint8_t*)0xf0000008)
#define reg_uart0_iir (*(volatile uint8_t*)0xf0000008)
#define reg_uart0_lcr (*(volatile uint8_t*)0xf000000c)
#define reg_uart0_mcr (*(volatile uint8_t*)0xf0000010)
#define reg_uart0_lsr (*(volatile uint8_t*)0xf0000014)
#define reg_uart0_msr (*(volatile uint8_t*)0xf0000018)

#define reg_led (*(volatile uint32_t*)0xe0000000)
#define reg_leds (*(volatile uint32_t*)0xe0000004)

#define reg_usb_info (*(volatile uint32_t*)0xc0000000)
#define reg_usb_keys (*(volatile uint32_t*)0xc0000004)
#define reg_usb_mouse (*(volatile uint32_t*)0xc0000008)
#define reg_usb_cursor (*(volatile uint32_t*)0xc000000c)

#define reg_sdcard (*(volatile uint32_t*)0xb0000000)

#define gpu_x0 (*(volatile uint32_t*)0xa0000000)
#define gpu_y0 (*(volatile uint32_t*)0xa0000004)
#define gpu_x1 (*(volatile uint32_t*)0xa0000008)
#define gpu_y1 (*(volatile uint32_t*)0xa000000c)
#define gpu_color (*(volatile uint32_t*)0xa0000010)
#define gpu_start (*(volatile uint32_t*)0xa0000014)
#define gpu_busy (*(volatile uint32_t*)0xa0000018)
#define gpu_pixel_count (*(volatile uint32_t*)0xa000001c)
#define gpu_debug_cur_x (*(volatile uint32_t*)0xa0000020)
#define gpu_debug_cur_y (*(volatile uint32_t*)0xa0000024)  
#define gpu_debug_fifo_count (*(volatile uint32_t*)0xa0000028)
#define gpu_clip_x0     (*(volatile uint32_t*)0xa000002c)  // Left bound
#define gpu_clip_y0     (*(volatile uint32_t*)0xa0000030)  // Top bound  
#define gpu_clip_x1     (*(volatile uint32_t*)0xa0000034)  // Right bound
#define gpu_clip_y1     (*(volatile uint32_t*)0xa0000038)  // Bottom bound
#define gpu_clip_enable (*(volatile uint32_t*)0xa000003c)  // Enable clipping

#define reg_mtu (*(volatile uint32_t*)0x90000000)

// --

int getch(void);
void readline(char *buf, int maxlen);
void echo(void);
void noecho(void);

#define VT100_CURSOR_UP       "\e[A"
#define VT100_CURSOR_DOWN     "\e[B"
#define VT100_CURSOR_RIGHT    "\e[C"
#define VT100_CURSOR_LEFT     "\e[D"
#define VT100_CURSOR_HOME     "\e[;H"
#define VT100_CURSOR_MOVE_TO  "\e[%i;%iH"
#define VT100_CURSOR_CRLF     "\e[E"
#define VT100_CLEAR_HOME      "\e[;H"
#define VT100_ERASE_SCREEN    "\e[J"
#define VT100_ERASE_LINE      "\e[K"

#define CH_ESC	0x1b
#define CH_LF	0x0a
#define CH_CR	0x0d
#define CH_FF	0x0c
#define CH_BS	0x08
#define CH_DEL	0x7f

// --

#define Z_MKSYSCALL(name, fn) Z_SYS_##name,
typedef enum {
	Z_SYSCALL_NONE = 0,
	#include "syscalls.def"
	Z_SYSCALL_COUNT
} z_syscall_id_t;
#undef Z_MKSYSCALL

#endif
