/*
 * Zeitlos BIOS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 */

// No <stdio.h>: this is a freestanding -nostdlib build with no C
// library linked at all, and it defines its own putchar/getchar/puts
// below rather than calling any. The include was vestigial, and it
// actively breaks toolchains whose libc headers are picolibc's --
// those declare stdin/stdout in ways that collide with this file's
// own. <stdint.h>/<stdbool.h> are freestanding headers supplied by
// GCC itself, so they need no library.
#include <stdint.h>

// normally from <stdio.h>; see the note above on why that isn't
// included here.
#ifndef EOF
#define EOF (-1)
#endif
#include <stdbool.h>

// instruction cache control (rtl/cache.v, 0x7000_01xx -- see
// sw/common/zsoc.h for the full register map and the flush rationale).
// Defined locally here rather than included: this BIOS is freestanding
// and keeps its own private copies of every register it touches, same
// as the uart/usb ones below.
#define reg_icache_ctrl (*(volatile uint32_t*)0x70000100)

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

#define reg_led (*(volatile uint8_t*)0xe0000000)
#define reg_leds (*(volatile uint8_t*)0xe0000004)

#define reg_usb_info (*(volatile uint32_t*)0xc0000000)
#define reg_usb_keys (*(volatile uint32_t*)0xc0000004)
#define reg_usb_mouse (*(volatile uint32_t*)0xc0000008)
#define reg_usb_cursor (*(volatile uint32_t*)0xc000000c)

#define reg_mtu (*(volatile uint32_t*)0x90000000)

// SOC capability CSRs (rtl/csrs.v, docs/csrs.md) -- local copies of
// the same registers sw/common/zsoc.h defines for the rest of the
// codebase, since sw/bios is a fully freestanding, self-contained
// build with no shared-header include path (same reasoning every
// other reg_* macro above is a private copy here, not a #include).
#define reg_csr_magic  (*(volatile uint32_t*)0x70000000)
#define reg_csr_mem_mb (*(volatile uint32_t*)0x70000004)
#define CSR_MAGIC 0x5A454954	// "ZEIT" -- see rtl/csrs.v

#define AUTOLOAD_CNT		500000

#define MEM_BIOS			0x00000000
#define MEM_BIOS_SIZE	2 * 1024
#define MEM_ROM			0x10000000
#define MEM_ROM_SIZE		1024 * 1024 * 2
#define MEM_VRAM			0x20000000
#define MEM_VRAM_SIZE	(640 * 480) / 32
#define MEM_MAIN			0x40000000

// DDR3 boards build a MINIMAL BIOS: copy the logo, train the DDR3 read
// path, load the OS, boot. Nothing else fits beside the training --
// the BIOS has 2048 words and the full monitor uses 1984 of them.
// Once training is settled it can move into gateware and the full
// BIOS comes back; until then each training change is a BIOS rebuild
// of seconds rather than a gateware build of many minutes.
#if defined(BOARD_MOZART_ML2)
#define BIOS_DDR3
#endif
// fallback only -- see get_mem_main_size() below, which is what
// main()/cmd_toggle_addr_ptr() actually call. Matches Obst (the only
// board this ever ran on before rtl/boards.vh's `MEM/rtl/csrs.v
// existed) -- kept as the fallback for a bitstream that predates CSRs
// entirely, same reasoning sw/os/mem.h's Z_MEM_SIZE_DEFAULT uses.
#define MEM_MAIN_SIZE_DEFAULT	1024 * 1024
#define MEM_APP			0x80000000
#define MEM_APP_SIZE		1024 * 1024

#define ROM_OS_ADDR		(MEM_ROM + (1024 * 1024 * 1))
#define ROM_OS_SIZE		1024 * 256

// Boot splash -- a full 640x480 1bpp framebuffer image, programmed at a
// fixed flash offset just below the kernel's by the top-level Makefile's
// `flash_logo` target. It is pre-centred and pre-padded at build time
// (sw/data/images/pad_logo.py), so showing it is one flat copy of the
// whole framebuffer rather than a row-by-row copy -- which matters here,
// where the BIOS budget is measured in bytes against BRAM_WORDS.
//
// KEEP IN SYNC with Z_BOOT_LOGO_FLASH_OFFSET (sw/os/logo.h) and
// LOGO_FLASH_OFFSET_HEX/_DEC (top-level Makefile).
#define ROM_LOGO_ADDR	(MEM_ROM + 0x000F0000)
#define ROM_LOGO_SIZE	((640 * 480) / 8)

//#include "scancodes.h"
//#include "hidcodes.h"

uint16_t curs_x = 0;
uint16_t curs_y = 0;

uint32_t addr_ptr;
uint32_t mem_total;

// reads rtl/csrs.v's MEM_MB register instead of assuming
// MEM_MAIN_SIZE_DEFAULT -- see docs/csrs.md. Falls back to that
// default if reg_csr_magic doesn't read back CSR_MAGIC (an older
// bitstream that predates rtl/csrs.v has nothing mapped at that
// address at all -- reading it doesn't fault on this bus, it just
// returns whatever rtl/sysctl.v's data-mux default case resolves to,
// so the magic-number check is the only reliable way to tell) or if
// the MB value itself is implausibly zero.
uint32_t get_mem_main_size() {
	if (reg_csr_magic != CSR_MAGIC) return MEM_MAIN_SIZE_DEFAULT;
	uint32_t mem_mb = reg_csr_mem_mb;
	if (!mem_mb) return MEM_MAIN_SIZE_DEFAULT;
	return mem_mb * 1024 * 1024;
}

// --------------------------------------------------------

uint32_t xfer_recv(uint32_t addr);
uint32_t crc32b(char *data, uint32_t len);
char scantoascii(uint8_t scancode);
char hidtoascii(uint8_t code);

void print_hex(uint32_t v, int digits);
void memtest(uint32_t addr_ptr, uint32_t mem_total);
// NOT called memcpy: this takes integer addresses rather than
// pointers, returns void rather than dest, and copies whole words
// only, so it is not the standard function and must not share its
// name. It only ever compiled because -ffreestanding implies
// -fno-builtin and suppressed the conflict with the builtin.
void bios_wordcpy(uint32_t dest, uint32_t src, uint32_t n);
void load_zeitlos(void);

int vid_cols;
int vid_rows;
int vid_hres;
int vid_vres;

// --------------------------------------------------------


int putchar(int c)
{
	while ((reg_uart0_lsr & 0x20) == 0);
	if (c == '\n')
		putchar('\r');

	reg_uart0_data = (char)c;

	return c;
}

void print(const char *p)
{
	while (*p)
		putchar(*(p++));
}

// NOTE: putchar_vga() used to sit here -- a VGA text-output routine
// that was declared and defined but never called from anywhere, and
// which wrote to 0x10000000 (MEM_ROM, the flash window) rather than
// MEM_VRAM, so it could not have worked as written. Removed to make
// room for the splash copy in main(): this BIOS is capped at
// BRAM_WORDS (2048 words = 8KB, sw/bios/Makefile) and was within a
// handful of bytes of that ceiling, while putchar_vga() was ~290 bytes
// of it. Recoverable from git if it was a work in progress -- but its
// destination address needs fixing before it can do anything.
int getchar()
{
	int uart_dr = ((reg_uart0_lsr & 0x01) == 1);

	if (!uart_dr) {
		return EOF;
	} else {
		return reg_uart0_data;
	}
}

void getchars(char *buf, int len) {
	int c;
	for (int i = 0; i < len; i++) {
		while ((c = getchar()) == EOF);
		buf[i] = (char)c;
	};
}

uint32_t xfer_recv(uint32_t addr_ptr)
{

	uint32_t addr = addr_ptr;
	uint32_t bytes = 0;
	uint32_t crc_ours;
	uint32_t crc_theirs;

	char buf_data[252];
	char buf_crc[4];

	int cmd;
	int datasize;

	print("xfer addr 0x");
	print_hex(addr, 8);
	print("\n");

	while (1) {

		while ((cmd = getchar()) == EOF);
		buf_data[0] = (uint8_t)cmd;

		if ((char)cmd == 'L') {
			while ((datasize = getchar()) == EOF);
			buf_data[1] = (uint8_t)datasize;
			getchars(&buf_data[2], datasize);
			getchars(buf_crc, 4);
			crc_ours = crc32b(buf_data, datasize + 2);
			crc_theirs = buf_crc[0] | (buf_crc[1] << 8) |
				(buf_crc[2] << 16) | (buf_crc[3] << 24);
			if (crc_ours == crc_theirs) {
				for (int i = 0; i < datasize; i++) {
					(*(volatile uint8_t *)(addr + i)) = buf_data[2 + i];
				}
				addr += datasize;
				bytes += datasize;
				putchar('A');
			} else {
				putchar('N');
			}
		}

		if ((char)cmd == 'D') {
			break;
		}

	}

	return bytes;

}

uint32_t crc32b(char *data, uint32_t len) {

	uint32_t byte, crc, mask;

	crc = 0xffffffff;
	for (int i = 0; i < len; i++) {
		byte = data[i];
		crc = crc ^ byte;
		for (int j = 7; j >= 0; j--) {
			mask = -(crc & 1);
			crc = (crc >> 1) ^ (0xedb88320 & mask);
		}
	}
	return ~crc;
}

void cmd_echo() {
	int c;

	while (1) {
		if ((c = getchar()) != EOF) {
			if ((char)c == '0') return;
			putchar(c);
		}
	}

}

void cmd_info() {

	uint8_t tmp;
	uint32_t tmp32;

	print("led: 0x");
	tmp = reg_led;
	print_hex(tmp, 2);
	print("\n");

	print("usb_info: 0x");
	tmp32 = reg_usb_info;
	print_hex(tmp32, 8);
	print("\n");

	print("usb_keys: 0x");
	tmp32 = reg_usb_keys;
	print_hex(tmp32, 8);
	print("\n");

	print("usb_mouse: 0x");
	tmp32 = reg_usb_mouse;
	print_hex(tmp32, 8);
	print("\n");

	print("usb_cursor: 0x");
	tmp32 = reg_usb_cursor;
	print_hex(tmp32, 8);
	print("\n");


}

void cmd_dump_bytes() {

	uint32_t addr = addr_ptr;
	uint8_t tmp;

	for (int i = 0; i < 16; i++) {
		print_hex(addr, 8);
		print(" ");
		for (int x = 0; x < 16; x++) {
			tmp = (*(volatile uint8_t *)addr);
			print_hex(tmp, 2);
			print(" ");
			addr += 1;
		}
		print("\n");
	}

}

void cmd_dump_words() {

	uint32_t addr = addr_ptr;
	uint32_t tmp;

	for (int i = 0; i < 16; i++) {
		print_hex(addr, 8);
		print(" ");
		for (int x = 0; x < 4; x++) {
			tmp = (*(volatile uint32_t *)addr);
			print_hex(tmp, 8);
			print(" ");
			addr += 4;
		}
		print("\n");
	}

}

void cmd_memzero()
{
	print("zeroing ... ");
   volatile uint32_t *addr = (uint32_t *)addr_ptr;
	for (int i = 0; i < (mem_total / sizeof(int)); i++) {
		(*(volatile uint32_t *)(addr + i)) = 0x00000000;
	}
	print("done.\n");
}

void cmd_memhigh()
{
	print("zeroing ... ");
   volatile uint32_t *addr = (uint32_t *)addr_ptr;
	for (int i = 0; i < (mem_total / sizeof(int)); i++) {
		(*(volatile uint32_t *)(addr + i)) = 0x12345678;
	}
	print("done.\n");
}

void cmd_memhigh_ff()
{
	print("zeroing ... ");
   volatile uint32_t *addr = (uint32_t *)addr_ptr;
	for (int i = 0; i < (mem_total / sizeof(int)); i++) {
		(*(volatile uint32_t *)(addr + i)) = 0xffffffff;
	}
	print("done.\n");
}

void bios_wordcpy(uint32_t dest, uint32_t src, uint32_t n) {
	volatile uint32_t *from = (uint32_t *)src;
	volatile uint32_t *to = (uint32_t *)dest;
	uint32_t words = n / sizeof(uint32_t);
	for (uint32_t i = 0; i < words; i++) {
		to[i] = from[i];
	}
}

void load_zeitlos() {
	print("loading zeitlos from rom to main memory ... ");

	print_hex(MEM_MAIN, 8);
	print(" ");
	print_hex(ROM_OS_ADDR, 8);
	print(" ");
	print_hex(ROM_OS_SIZE, 8);
	print(" ");

	bios_wordcpy(MEM_MAIN, ROM_OS_ADDR, ROM_OS_SIZE);

	// The kernel image was just written through the data path. The
	// instruction cache (rtl/cache.v) caches fetches only, so it never
	// observed those stores. On a COLD boot it is empty anyway and this
	// changes nothing -- the case that matters is a warm restart back
	// into the BIOS without the cache being reset, where lines from the
	// previous kernel are still resident for these exact physical
	// addresses and would be served to the new one.
	//
	// Safe on a bitstream built without `ICACHE, but only because
	// rtl/sysctl.v decodes this window unconditionally (csrs_wb keeps
	// the whole 0x7 nibble when there's no cache). An undecoded write
	// here would never be acked and would hang the BIOS outright.
	//reg_icache_ctrl = 0x2;		// flush only
	reg_icache_ctrl = 0x3;		// enable | flush

	print("done.\n");
}

//
// --------------------------------------------------------

void cmd_help() {

	print("\n [0] toggle address\n");
	print(" [D] dump memory as bytes\n");
	print(" [W] dump memory as words\n");
	print(" [9] reset memory page\n");
	print(" [ ] next memory page\n");
	print(" [I] system info\n");
	print(" [M] test memory\n");
	print(" [Z] zero memory\n");
	print(" [F] fill memory with pattern\n");
	print(" [X] receive to memory (xfer)\n");
	print(" [1] led on\n");
	print(" [2] led off\n");
	print(" [B] boot to 0x40000000\n");
	print(" [E] echo mode (exit with 0)\n");
	print(" [H] help\n\n");

}

void cmd_toggle_addr_ptr(void) {

	if (addr_ptr == MEM_BIOS) {
		addr_ptr = MEM_ROM;
		mem_total = MEM_ROM_SIZE;
	} else if (addr_ptr == MEM_ROM) {
		addr_ptr = MEM_VRAM;
		mem_total = MEM_VRAM_SIZE;
	} else if (addr_ptr == MEM_VRAM) {
		addr_ptr = MEM_MAIN;
		mem_total = get_mem_main_size();
	} else if (addr_ptr == MEM_MAIN) {
		addr_ptr = MEM_APP;
		mem_total = MEM_APP_SIZE;
	} else if (addr_ptr == MEM_APP) {
		addr_ptr = MEM_BIOS;
		mem_total = MEM_BIOS_SIZE;
	}

}

void cmd_xfer() {
	uint32_t b = xfer_recv(addr_ptr);
	print("xfer received ");
	print_hex(b, 8);
	print(" bytes at ");
	print_hex(addr_ptr, 8);
	print("\n");
}

void uart_init() {

	uint16_t baud_rate_divisor = 3;	// clock / divisor = 16 x baud rate
												// 48_000_000 / 3 = 16000000 = 16 x 1000000

	// set LCR
	reg_uart0_lcr = (uint8_t)0b10000011;	// access divisor latch; 8n1

	// set divisor latch
	reg_uart0_dlbh = (uint8_t)((baud_rate_divisor >> 8) & 0xff);
	reg_uart0_dlbl = (uint8_t)(baud_rate_divisor & 0xff);

	reg_uart0_lcr = (uint8_t)0b00000011;	// disable divisor latch

	reg_uart0_fcr = (uint8_t)0b00000111;	// enable fifos; trigger 1 byte
	reg_uart0_ier = (uint8_t)0b00000000;	// disable all interrupts

}

void delay() {
	volatile static int x, y;
	for (int i = 0; i < 500000; i++) {
		x += y;
	}
}

#ifdef BIOS_DDR3

// -- DDR3 read-path training --------------------------------------
//
// Registers: rtl/mem/ddr3.v.
#define reg_ddr3_status (*(volatile uint32_t*)0x70000700)
#define reg_ddr3_tune   (*(volatile uint32_t*)0x70000704)
#define reg_ddr3_ctrl   (*(volatile uint32_t*)0x70000708)
#define reg_dcache_ctrl (*(volatile uint32_t*)0x70000110)

#define CTRL_HOLD_REFRESH 1
#define CTRL_TRAIN_WRITES 4
#define CTRL_RDBUF_OFF    8

// Every training access goes to the DRAM. The first hardware scan
// forgot the controller's own block register: the training write left
// the block in it, every training read was answered from it, and every
// setting passed -- the map was all 'f', sixteen offsets working
// everywhere, while the real read path had never delivered a burst.
#define CTRL_TRAINING (CTRL_TRAIN_WRITES | CTRL_RDBUF_OFF)

// Every byte of the block different, so a beat, a byte or a lane in
// the wrong place cannot pass. (A pattern whose halves match hides a
// one-beat shift -- which is how two cancelling errors survived in
// the controller's testbench.)
static const uint32_t ddr3_pat[4] = {
	0x04030201, 0x08070605, 0x0c0b0a09, 0x100f0e0d
};

// Per lane, per read phase, per gate: the offset that assembled the
// lane correctly, 0xff for none, or 0xfe for MORE THAN ONE.
//
// A real read path assembles a lane at exactly one offset -- the
// assembly testbench proves it. Two offsets passing in one cell means
// the reads are not coming from the DRAM at all, so such a cell is
// shown as '*' and never chosen. The trainer checks that its own
// measurement reaches the hardware, instead of trusting it.
static uint8_t ddr3_tbl[2][8][8];

static uint32_t ddr3_tune_of(uint32_t wd, uint32_t rs, uint32_t g,
	uint32_t o)
{
	return wd | (g << 4) | (g << 8) | (rs << 12) | (rs << 16)
		| (o << 20) | (o << 24);
}

// Written through the training mode, which skips the read that
// read-modify-write would otherwise need: four words in sequence
// leave the block exactly right whatever the read path is doing.
// Refresh runs while it happens, which also bounds how long refresh
// is ever held -- DDR3 keeps data for 64ms without it.
static void ddr3_write_pat(void)
{
	volatile uint32_t *m = (volatile uint32_t *)MEM_MAIN;
	int i;
	reg_ddr3_ctrl = CTRL_TRAINING;
	for (i = 0; i < 4; i++) m[i] = ddr3_pat[i];
	reg_ddr3_ctrl = CTRL_TRAINING | CTRL_HOLD_REFRESH;
}

// Both lanes scored separately, on their own bytes, so one lane
// working is visible while the other is not. And twice: a value that
// comes back identical is repeatable, which a stale capture that
// happens to be right once is not.
static uint32_t ddr3_score(void)
{
	volatile uint32_t *m = (volatile uint32_t *)MEM_MAIN;
	uint32_t ok = 3, a, b;
	int i;
	for (i = 0; i < 4; i++) {
		a = m[i];
		b = m[i];
		if (a != b) return 0;
		if ((a & 0x00ff00ff) != (ddr3_pat[i] & 0x00ff00ff)) ok &= ~1;
		if ((a & 0xff00ff00) != (ddr3_pat[i] & 0xff00ff00)) ok &= ~2;
	}
	return ok;
}

// Does the same offset work one read phase away? READCLKSEL is CIRCULAR:
// stepping from phase 7 to phase 0 moves a whole cycle, and the gate
// compensates by one. The first hardware map showed exactly that --
// lane 1 working at r6 and r7 with gate 2, and at r0 with gate 3, same
// offset -- so r7 was the centre of its window, while a test that only
// looked at r6 and a nonexistent r8 called it an edge.
static int ddr3_nb(int lane, int rs, int g, uint8_t v)
{
	if (rs < 0) { rs = 7; g--; }
	if (rs > 7) { rs = 0; g++; }
	if (g < 0 || g > 7) return 0;
	return ddr3_tbl[lane][rs][g] == v;
}

// The cell with the most working neighbours in phase: 2 = centred.
static int ddr3_pick(int lane, uint32_t *rs_o, uint32_t *g_o,
	uint32_t *o_o)
{
	int rs, g, sc, best = -1;
	uint8_t v;
	for (rs = 0; rs < 8; rs++)
		for (g = 0; g < 8; g++) {
			v = ddr3_tbl[lane][rs][g];
			if (v >= 0xfe) continue;
			sc = ddr3_nb(lane, rs - 1, g, v) + ddr3_nb(lane, rs + 1, g, v);
			if (sc > best) {
				best = sc;
				*rs_o = rs; *g_o = g; *o_o = v;
			}
		}
	return best + 1;       // 0 none, 1 edge-only, 2 one side, 3 centred
}

// One lane's map: a row per read phase, a column per gate, the offset
// that worked there or '.'.
static void ddr3_map(int lane)
{
	int rs, g;
	uint8_t v;
	for (rs = 0; rs < 8; rs++) {
		print(lane ? " L1 r" : " L0 r");
		print_hex(rs, 1);
		print(" ");
		for (g = 0; g < 8; g++) {
			v = ddr3_tbl[lane][rs][g];
			if (v == 0xff) print(".");
			else if (v == 0xfe) print("*");
			else print_hex(v, 1);
		}
		print("\n");
	}
}

// Where does the read gate catch the strobe?
//
// BURSTDET is set by each lane's DQSBUFM when the read gate catches a
// DQS burst. It depends only on the READ command and the gate --
// not on the data, and not on whether any write ever landed -- so it
// answers the first question independently of every other one: is the
// DRAM answering reads at all, and when?
//
// One read per cell; the PHY clears BURSTDET as each read is issued.
// Each cell prints the lanes that saw a burst: 1 = lane 0, 2 = lane 1,
// 3 = both. (Every read completes now -- data is captured a fixed time
// after the command, as in LiteDRAM, not when DATAVALID says so -- so
// completion no longer tells anything and is not shown.)
static void ddr3_gatescan(void)
{
	volatile uint32_t *m = (volatile uint32_t *)MEM_MAIN;
	uint32_t rs, g, st, d;

	print("gate scan: burstdet, 1=L0 2=L1 3=both, row=rdclksel col=gate\n");
	reg_ddr3_ctrl = CTRL_TRAINING | CTRL_HOLD_REFRESH;
	for (rs = 0; rs < 8; rs++) {
		print(" r");
		print_hex(rs, 1);
		print(" ");
		for (g = 0; g < 8; g++) {
			reg_ddr3_tune = ddr3_tune_of(3, rs, g, 4);
			d = m[0];
			(void)d;
			st = reg_ddr3_status;
			print_hex((st >> 4) & 3, 1);
		}
		print("\n");
	}
	reg_ddr3_ctrl = CTRL_TRAINING;
}

// When training finds nothing: what DOES come back? The first gate
// where both lanes saw a burst, every offset, all four words -- the
// shape of the wrong data says more than a pass/fail map can.
static void ddr3_dump(void)
{
	volatile uint32_t *m = (volatile uint32_t *)MEM_MAIN;
	uint32_t rs, g, o, i;

	reg_ddr3_ctrl = CTRL_TRAINING | CTRL_HOLD_REFRESH;
	for (rs = 0; rs < 8; rs++)
		for (g = 0; g < 8; g++) {
			reg_ddr3_tune = ddr3_tune_of(4, rs, g, 4);
			(void)m[0];
			if (((reg_ddr3_status >> 4) & 3) == 3) goto found;
		}
	print("no cell with a burst on both lanes\n");
	reg_ddr3_ctrl = 0;
	return;
found:
	// Write delay 4: LiteDRAM's position, and the only one that has
	// worked here. The dump used 3, i.e. showed data written wrongly.
	reg_ddr3_tune = ddr3_tune_of(4, rs, g, 4);
	print("dump (wd4) at r");
	print_hex(rs, 1);
	print(" g");
	print_hex(g, 1);
	print(", want 04030201 08070605 0c0b0a09 100f0e0d\n");
	ddr3_write_pat();
	for (o = 0; o < 16; o++) {
		reg_ddr3_tune = ddr3_tune_of(4, rs, g, o);
		print(" o");
		print_hex(o, 1);
		for (i = 0; i < 4; i++) {
			print(" ");
			print_hex(m[i], 8);
		}
		print("\n");
	}
	reg_ddr3_ctrl = 0;
}

static int ddr3_train(void)
{
	volatile uint32_t *m = (volatile uint32_t *)MEM_MAIN;
	uint32_t wd, rs, g, o, sc, n;
	uint32_t rs0, g0, o0, rs1, g1, o1;
	int p0, p1;

	// The trainer must read DRAM, not a cache. D_CTRL resets to 0
	// already; saying so here stops the trainer depending on a reset
	// value that someone might one day change.
	reg_dcache_ctrl = 0;

	print("ddr3 ");
	for (n = 0; n < 2000000 && !(reg_ddr3_status & 1); n++) ;
	print_hex(reg_ddr3_status, 8);
	if (!(reg_ddr3_status & 1)) {
		print(" init timeout\n");
		return 0;
	}
	print("\n");

	// Always, before training: it is informative whether or not
	// training then succeeds.
	ddr3_gatescan();

	for (wd = 0; wd < 8; wd++) {
		for (rs = 0; rs < 8; rs++) {
			reg_ddr3_tune = ddr3_tune_of(wd, 0, 0, 0);
			ddr3_write_pat();
			for (g = 0; g < 8; g++) {
				ddr3_tbl[0][rs][g] = 0xff;
				ddr3_tbl[1][rs][g] = 0xff;
				for (o = 0; o < 16; o++) {
					reg_ddr3_tune = ddr3_tune_of(wd, rs, g, o);
					sc = ddr3_score();
					if (sc & 1) ddr3_tbl[0][rs][g] =
						ddr3_tbl[0][rs][g] == 0xff ? o : 0xfe;
					if (sc & 2) ddr3_tbl[1][rs][g] =
						ddr3_tbl[1][rs][g] == 0xff ? o : 0xfe;
				}
			}
		}
		p0 = ddr3_pick(0, &rs0, &g0, &o0);
		p1 = ddr3_pick(1, &rs1, &g1, &o1);
		if (p0 || p1) {
			print("wd");
			print_hex(wd, 1);
			print("\n");
			ddr3_map(0);
			ddr3_map(1);
		}
		if (p0 && p1) break;
	}

	reg_ddr3_ctrl = 0;
	if (wd == 8) {
		print("ddr3 training FAILED\n");
		ddr3_dump();
		return 0;
	}

	reg_ddr3_tune = wd | (g0 << 4) | (g1 << 8) | (rs0 << 12)
		| (rs1 << 16) | (o0 << 20) | (o1 << 24);
	print("ddr3 tune ");
	print_hex(reg_ddr3_tune, 8);
	// Per lane: C = centred (works one phase either side), e = edge.
	print(p0 == 3 ? " L0:C" : " L0:e");
	print(p1 == 3 ? " L1:C\n" : " L1:e\n");

	// Now through the normal path: read-modify-write, refresh on,
	// a block the training never touched, a word and a single byte.
	m[64] = 0xa5c3a5c3;
	m[65] = 0x5a3c5a3c;
	((volatile uint8_t *)MEM_MAIN)[64 * 4 + 1] = 0x77;
	print("ddr3 check ");
	print_hex(m[64], 8);
	print(" ");
	print_hex(m[65], 8);
	if (m[64] != 0xa5c377c3 || m[65] != 0x5a3c5a3c) {
		print(" FAILED\n");
		return 0;
	}
	print(" ok\n");
	return 1;
}

// Compare the loaded kernel with the ROM it came from, twice.
//
// The copy itself is robust -- each 16-byte block is written four times
// and the last write carries all four words from the merge register --
// so a mismatch here comes from READING BACK, which is what the OS does
// with every instruction it fetches. Two passes: if they disagree, the
// errors are read noise at a marginal setting, not wrong data stored.
// "bits" is every bit that was ever wrong, so it names the lane: the
// low byte of each half is lane 0 (00ff00ff), the high byte lane 1.
static void ddr3_verify(void)
{
	volatile uint32_t *d = (volatile uint32_t *)MEM_MAIN;
	volatile uint32_t *r = (volatile uint32_t *)ROM_OS_ADDR;
	uint32_t i, a, b, e, x, first, pass;

	for (pass = 0; pass < 2; pass++) {
		e = 0; x = 0; first = 0;
		for (i = 0; i < ROM_OS_SIZE / 4; i++) {
			a = d[i];
			b = r[i];
			if (a != b) {
				if (!e) first = i;
				e++;
				x |= a ^ b;
			}
		}
		print("verify ");
		print_hex(e, 8);
		print(" bad words, bits ");
		print_hex(x, 8);
		if (e) {
			print(", first @");
			print_hex(first * 4, 8);
			print(" got ");
			print_hex(d[first], 8);
			print(" want ");
			print_hex(r[first], 8);
		}
		print("\n");
	}
}

// Logo, training, OS. Returning from main boots: the startup code
// jumps to MEM_MAIN (boot_picorv32.S).
void main() {
	reg_led = 0xff;
	reg_mtu = 0x40000000;	// 0x8000_0000 will mirror 0x4000_0000

	uart_init();
	print("ZB\n");
	bios_wordcpy(MEM_VRAM, ROM_LOGO_ADDR, ROM_LOGO_SIZE);

	if (!ddr3_train()) {
		print("main memory unusable -- not booting\n");
		while (1) ;
	}
	load_zeitlos();
	ddr3_verify();
}

#else

void main() {

	uint32_t ctr = 0;
	bool interacted = false;
	int cmd;

	reg_led = 0xff;
	reg_mtu = 0x40000000;	// 0x8000_0000 will mirror 0x4000_0000

	addr_ptr = MEM_MAIN;
	mem_total = get_mem_main_size();

	uart_init();

	print("ZB\n");

	// Splash before load_zeitlos() on purpose: the 256KB kernel copy is
	// the longest pause in the boot, so this puts something on screen
	// first. Flash is memory-mapped (load_zeitlos() copies the
	// kernel straight out of it), so this uses no RAM at all -- and
	// because the image is a whole pre-padded framebuffer, it also
	// clears whatever was in VRAM at reset.
	bios_wordcpy(MEM_VRAM, ROM_LOGO_ADDR, ROM_LOGO_SIZE);

#ifndef FPGA_GATEMATE
	load_zeitlos();
#endif

	cmd_info();
	cmd_help();

	/* Discard whatever the host sent while the console was still
	 * blocked waiting to be opened.
	 *
	 * On a `USB_CDC board (rtl/usb_cdc_uart.v, docs/usb_cdc.md) this
	 * is not defensive tidying, it is load-bearing. That console
	 * blocks in putchar() until a terminal opens the port -- which is
	 * how the banner above survives with no buffer to hold it -- and
	 * a terminal opening a port is also the moment it sends its
	 * greeting: minicom's default modem init string, or
	 * ModemManager's AT probes on a Linux box without the udev rule.
	 * So by the time we get here there are already bytes waiting, and
	 * the loop below treats ANY byte as interaction and cancels
	 * autoboot for good (ctr is never reset, and its test is an exact
	 * equality that never matches again). The board sat at this
	 * prompt instead of booting, every single time.
	 *
	 * Here rather than in uart_init(): that runs before the banner,
	 * so it flushes a port nothing has opened yet. THIS is the point
	 * at which the console genuinely becomes usable.
	 *
	 * Harmless on a 16550 board, where it discards at most a few
	 * bytes typed during the kernel copy.
	 *
	 * Both halves are needed. FCR bit 1 empties the hardware receive
	 * FIFO in one write; the getchar() loop catches anything that
	 * lands in the gap afterwards. Neither closes the window
	 * completely -- a byte still on the wire will arrive -- but a
	 * terminal sends its greeting at open(), which is squarely inside
	 * the window this covers. */
	reg_uart0_fcr = (uint8_t)0b00000111;
	while (getchar() != EOF);

	while (1) {

		print("@");
		print_hex(addr_ptr, 8);
		print("> ");

		while ((cmd = getchar()) == EOF) {
			ctr++;
			if (ctr == AUTOLOAD_CNT && !interacted) return;
		}

		interacted = true;

		print("\n");

		switch (cmd) {
			case 'h':
			case 'H':
				cmd_help();
				break;
			case '0':
				cmd_toggle_addr_ptr();
				break;
			case '1':
				reg_led = 0x01;
				break;
			case '2':
				reg_led = 0x00;
				break;
			case '9':
				addr_ptr = 0x40000000;
				break;
			case ' ':
				addr_ptr += 256;
				break;
			case 'x':
			case 'X':
				cmd_xfer();
				break;
			case 'i':
			case 'I':
				cmd_info();
				break;
			case 'd':
			case 'D':
				cmd_dump_bytes();
				break;
			case 'w':
			case 'W':
				cmd_dump_words();
				break;
			case 'm':
			case 'M':
				memtest(addr_ptr, mem_total);
				break;
			case 'b':
			case 'B':
				print("booting ... ");
				return;
				break;
			case 'e':
			case 'E':
				cmd_echo();
				break;
			case 'z':
			case 'Z':
				cmd_memzero();
				break;
			case 'f':
				cmd_memhigh();
				break;
			case 'F':
				cmd_memhigh_ff();
				break;
			case 'l':
			case 'L':
				load_zeitlos();
				break;
			default:
				continue;
		}

	}

}
#endif  /* BIOS_DDR3 */
