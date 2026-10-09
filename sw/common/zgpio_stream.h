#ifndef ZGPIO_STREAM_H
#define ZGPIO_STREAM_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * GPIO stream engines -- rtl/gpio_stream.v, docs/gpio.md ("Stream
 * engines"). A serial peripheral that can be put on ANY GPIO pin, in
 * one of four modes:
 *
 *   Z_GS_ZLINK  8b/10b, self-clocked, up to 12 Mbit/s (docs/zlink.md)
 *   Z_GS_SPI    SPI master, full duplex, modes 0-3, SCK up to 12 MHz
 *   Z_GS_UART   8N1 / 8N2, 300 baud to 3 Mbaud
 *   Z_GS_RAW    bits out and/or in at a fixed rate, up to 48 Mbit/s
 *
 * A board builds 0, 1 or more engines (rtl/boards.vh,
 * GPIO_STREAM_ENGINES; one by default wherever there is a GPIO port).
 * Ask z_gs_count() -- 0 on a board without them, and on every bitstream
 * that predates them, so code written against this header degrades to
 * whatever software fallback it has.
 *
 * -- Use --
 *
 *   int e = z_gs_claim(Z_GS_ANY);          // -1: none free / none built
 *   z_gs_cfg_t c = Z_GS_CFG_INIT;
 *   c.mode = Z_GS_UART;
 *   c.tx = Z_GS_PIN(0, 0);                 // port 0, pin 0
 *   c.rx = Z_GS_PIN(0, 1);
 *   c.flags = Z_GS_RXEN;
 *   c.div = z_gs_div_for(Z_GS_UART, 115200, NULL);
 *   if (z_gs_config(e, &c) == 0) {
 *       z_gs_write(e, buf, n);
 *       ...
 *   }
 *   z_gs_release(e);
 *
 * While an engine is configured, the pins it uses are its own: the
 * port's DIR and OUT no longer reach them (they still read and write;
 * zgpio.h's calls on those pins just have no effect). z_gs_release()
 * gives them back.
 *
 * -- Ownership between processes --
 *
 * z_gs_claim() takes an engine with SLOCK, a test-and-set register --
 * one bus cycle, so two processes cannot both get the same engine.
 * Nothing in the kernel knows about it: a process that exits without
 * z_gs_release() leaves the engine claimed (and possibly driving its
 * pins). z_gs_force_release() exists for a shell command to recover
 * from that; ordinary code never calls it.
 *
 * -- Non-blocking --
 *
 * z_gs_write() and z_gs_read() move what fits and return how much that
 * was; they never wait. The FIFOs are 1024 entries each way.
 * z_gs_spi_xfer() and z_gs_drain() are the two calls that do wait, and
 * both have bounded loops.
 */

#include <stdbool.h>
#include <stdint.h>

#define Z_GS_MAX_ENGINES 4
#define Z_GS_FIFO_DEPTH  1024
#define Z_GS_ANY         (-1)

// Engine registers: 0xe000_2000 + engine * 0x40. Byte offsets.
#define Z_GS_BASE        0xe0002000u
#define Z_GS_SIZE        0x40u

#define Z_GS_REG_SCTL    0x00
#define Z_GS_REG_SPINS   0x04
#define Z_GS_REG_SRATE   0x08
#define Z_GS_REG_SSTAT   0x0c
#define Z_GS_REG_STX     0x10
#define Z_GS_REG_STX4    0x14
#define Z_GS_REG_SRX     0x18
#define Z_GS_REG_SRX4    0x1c
#define Z_GS_REG_SERR    0x20
#define Z_GS_REG_SFLUSH  0x24
#define Z_GS_REG_SLOCK   0x28

typedef enum {
	Z_GS_OFF   = 0,
	Z_GS_ZLINK = 1,
	Z_GS_SPI   = 2,
	Z_GS_UART  = 3,
	Z_GS_RAW   = 4,
} z_gs_mode_t;

// SCTL flag bits, exactly as rtl/gpio_stream.v lays them out (bits 3-15)
#define Z_GS_LSB      (1u << 3)		// SPI, raw: LSB first (zlink, UART always are)
#define Z_GS_CPHA     (1u << 4)		// SPI
#define Z_GS_CPOL     (1u << 5)		// SPI
#define Z_GS_TXINV    (1u << 6)		// invert the TX/MOSI pin
#define Z_GS_RXINV    (1u << 7)		// invert the RX/MISO pin
#define Z_GS_TXOD     (1u << 8)		// TX open drain: drive low, release high
#define Z_GS_RXEN     (1u << 9)		// keep what is received
#define Z_GS_LOOP     (1u << 10)	// TX straight to RX inside the engine
#define Z_GS_REPEAT   (1u << 11)	// raw: loop the TX FIFO
#define Z_GS_TRIG     (1u << 12)	// raw: start capturing at the first edge
#define Z_GS_STOP2    (1u << 13)	// UART: two stop bits
#define Z_GS_CS       (1u << 14)	// level on the CS pin (use z_gs_cs())
#define Z_GS_IDLEHIGH (1u << 15)	// raw: TX idles high
#define Z_GS_FLAGS    0xfff8u

// SSTAT
#define Z_GS_ST_TXLEVEL(s)  ((s) & 0x7ffu)
#define Z_GS_ST_BUSY        (1u << 11)
#define Z_GS_ST_RXOVR       (1u << 12)	// sticky: RX FIFO was full
#define Z_GS_ST_TXOVF       (1u << 13)	// sticky: pushed into a full TX FIFO
#define Z_GS_ST_ALIGNED     (1u << 14)	// zlink: receiver has a comma
#define Z_GS_ST_KWORD       (1u << 15)	// sticky: SRX4 popped a K symbol
#define Z_GS_ST_RXLEVEL(s)  (((s) >> 16) & 0x7ffu)

// Pin numbers are port * 8 + pin, the same flat numbering everywhere in
// this API. -1 means the role is not used.
#define Z_GS_PIN(port, pin) ((int8_t)((port) * 8 + (pin)))
#define Z_GS_NOPIN          ((int8_t)-1)

// zlink control symbols (K flag set). K28.5 is the idle comma; the
// receiver uses it to align and never puts it in the RX FIFO.
#define Z_GS_K28_5  0xBC
#define Z_GS_K27_7  0xFB
#define Z_GS_K29_7  0xFD
#define Z_GS_K30_7  0xFE

typedef struct {
	z_gs_mode_t mode;
	int8_t tx;			// TX / MOSI
	int8_t rx;			// RX / MISO
	int8_t clk;			// SPI SCK
	int8_t cs;			// SPI CS (any mode: drives Z_GS_CS's level)
	uint32_t flags;		// Z_GS_* above
	uint16_t div;		// tick = 48 MHz / (div + 1); see z_gs_div_for()
	uint8_t spi_delay;	// SPI: MISO sample delay in clocks, 0-7
} z_gs_cfg_t;

#define Z_GS_CFG_INIT { Z_GS_OFF, Z_GS_NOPIN, Z_GS_NOPIN, Z_GS_NOPIN, \
                        Z_GS_NOPIN, 0, 0, 0 }

// z_gs_config() results
#define Z_GS_OK          0
#define Z_GS_EBADENGINE  (-1)	// not built, or not claimed by this process
#define Z_GS_EMODE       (-2)	// mode not built into this bitstream
#define Z_GS_EPIN        (-3)	// pin out of range for this board
#define Z_GS_EPINDUP     (-4)	// two roles on one pin
#define Z_GS_EPINBUSY    (-5)	// another engine is using that pin
#define Z_GS_ENOPIN      (-6)	// the mode needs a role that is not set

// -- presence ----------------------------------------------------

// Engines this bitstream has. 0 if none, or no GPIO at all.
uint32_t z_gs_count(void);

// Which modes they have: bit (1 << mode) for each of Z_GS_ZLINK ..
// Z_GS_RAW. zlink is always there when an engine is.
uint32_t z_gs_modes(void);

// -- ownership ---------------------------------------------------

// Claim an engine: a specific one, or Z_GS_ANY for the first free one.
// Returns its number, or -1.
int  z_gs_claim(int engine);

// Turn it off (pins back to ordinary GPIO), empty its FIFOs and give
// it back.
void z_gs_release(int engine);

// Release an engine some other process claimed and never gave back.
// For a shell command, not for ordinary code.
void z_gs_force_release(int engine);

// -- configuration -----------------------------------------------

// Check the pins against this board and the other engines, then
// program the engine. The engine's FIFOs are emptied; it starts
// sending (zlink idles, UART idle-high, ...) as soon as this returns.
int  z_gs_config(int engine, const z_gs_cfg_t *cfg);

// The divider that comes closest to `bps` in a mode (bits per second;
// for SPI, the SCK frequency), clamped to what the hardware can do --
// so a rate above the mode's maximum gets divider 0. If `actual` is not
// NULL it gets what the returned divider really gives; check it when
// the rate matters (a UART at 115200 is really 115384, +0.16%).
uint16_t z_gs_div_for(z_gs_mode_t mode, uint32_t bps, uint32_t *actual);

// What a divider gives, in bits per second (SPI: SCK in Hz).
uint32_t z_gs_rate(z_gs_mode_t mode, uint16_t div);

// SPI chip select. Waits for nothing: call z_gs_drain() first if the
// last byte has to be out before CS goes high.
void z_gs_cs(int engine, bool level);

// -- data --------------------------------------------------------

// Queue up to n bytes; returns how many fit.
uint32_t z_gs_write(int engine, const uint8_t *buf, uint32_t n);

// Queue one zlink control symbol (K28.y, K23/27/29/30.7). false if the
// FIFO is full.
bool z_gs_write_k(int engine, uint8_t k);

// Take up to n received data bytes; returns how many. In zlink mode it
// stops BEFORE a control symbol, which z_gs_read_sym() then returns.
uint32_t z_gs_read(int engine, uint8_t *buf, uint32_t n);

// One received entry: the byte, with 0x100 set for a zlink control
// symbol; -1 if there is none.
int  z_gs_read_sym(int engine);

uint32_t z_gs_tx_free(int engine);
uint32_t z_gs_rx_level(int engine);

// Something still to send, or being sent.
bool z_gs_busy(int engine);

// Wait (up to about `max_polls` status reads) until nothing is left to
// send. true if it emptied.
bool z_gs_drain(int engine, uint32_t max_polls);

void z_gs_flush(int engine, bool tx, bool rx);

// SSTAT, and the error counter (zlink code errors, UART framing).
uint32_t z_gs_status(int engine);
uint32_t z_gs_errors(int engine);
void z_gs_clear_errors(int engine);

// -- SPI ---------------------------------------------------------

// Full-duplex transfer of n bytes, in chunks that fit the FIFOs. tx may
// be NULL (sends 0xFF); rx may be NULL (received bytes are dropped).
// Does not touch CS. Returns the number of bytes exchanged (n when it
// worked), or -1 if the engine is not in SPI mode. An engine configured
// without Z_GS_RXEN is write-only: everything is sent, the call waits
// for the last byte to leave, and rx is not touched.
int  z_gs_spi_xfer(int engine, const uint8_t *tx, uint8_t *rx, uint32_t n);

#endif
