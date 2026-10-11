#ifndef ZFAST_H
#define ZFAST_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Fast zlink -- rtl/zfast.v, docs/zlink.md ("Fast"). The DS port of a
 * Mozart: two LVDS pairs, at 100, 50 or 25 Mbit/s, speaking the same
 * line as a GPIO stream engine in zlink mode (zgpio_stream.h) -- 8b/10b,
 * K symbols -- so the same transport code moves frames over it
 * (zlink_fast.h). EXPERIMENTAL, and only in a build with ZLINK_FAST:
 * z_zf_present() says.
 *
 * Each pair can transmit or receive; which one this end drives is the
 * question sw/common/zlink_fast.c answers with the far end. On a Sechzig
 * ML2, DS2 cannot transmit at all (z_zf_tx_ds2() is false).
 *
 * Non-blocking, like zgpio_stream.h: write and read move what fits.
 * FIFOs of 1024 entries each way.
 */

#include <stdint.h>
#include <stdbool.h>

#define Z_ZF_BASE        0xe0004000u

#define Z_ZF_REG_FCTL    0x00
#define Z_ZF_REG_FSTAT   0x0c
#define Z_ZF_REG_FTX     0x10
#define Z_ZF_REG_FTX4    0x14
#define Z_ZF_REG_FRX     0x18
#define Z_ZF_REG_FRX4    0x1c
#define Z_ZF_REG_FERR    0x20
#define Z_ZF_REG_FFLUSH  0x24
#define Z_ZF_REG_FLOCK   0x28

#define Z_ZF_FIFO_DEPTH  1024

// which pair
#define Z_ZF_NONE  0
#define Z_ZF_DS1   1
#define Z_ZF_DS2   2

// rates
#define Z_ZF_100M  0
#define Z_ZF_50M   1
#define Z_ZF_25M   2

// flags for z_zf_config()
#define Z_ZF_RXEN   (1u << 4)
#define Z_ZF_LOOP   (1u << 5)		// TX straight to RX inside the block
#define Z_ZF_TXINV  (1u << 8)
#define Z_ZF_RXINV  (1u << 9)

// FSTAT
#define Z_ZF_ST_TXLEVEL(s)  ((s) & 0x7ffu)
#define Z_ZF_ST_RXOVR       (1u << 12)
#define Z_ZF_ST_TXOVF       (1u << 13)
#define Z_ZF_ST_ALIGNED     (1u << 14)
#define Z_ZF_ST_RXLEVEL(s)  (((s) >> 16) & 0x7ffu)

// Built into this bitstream?
bool z_zf_present(void);

// Can DS2 transmit? (False on an ML2, and without the block.)
bool z_zf_tx_ds2(void);

// Take the block (a test-and-set, as an engine's SLOCK) and give it
// back -- releasing turns it off.
bool z_zf_claim(void);
void z_zf_release(void);

// Turn it on: transmit on `tx` (Z_ZF_NONE, _DS1, _DS2), receive on
// `rx` (_DS1 or _DS2), at a rate, with flags. A DS2 this board cannot
// drive is not driven. The FIFOs are kept; z_zf_flush() empties them.
void z_zf_config(int tx, int rx, int rate, uint32_t flags);
void z_zf_off(void);

// Bits per second for a rate.
uint32_t z_zf_bps(int rate);

uint32_t z_zf_write(const uint8_t *buf, uint32_t n);
bool z_zf_write_k(uint8_t k);
// Data bytes up to the next control symbol, which z_zf_read_sym()
// then returns (0x100 | k), as zgpio_stream.h does.
uint32_t z_zf_read(uint8_t *buf, uint32_t n);
int z_zf_read_sym(void);

// The fast way in: up to n words of four data bytes (byte 0 in bits
// 7:0), one bus access each, only while four entries are waiting. It
// does not stop at a control symbol -- that is popped as data, and
// z_zf_kword() is set -- so it is for a caller that knows how many data
// bytes come next (zlink_fast.c's frames carry their length).
uint32_t z_zf_read_words(uint32_t *w, uint32_t n);
bool z_zf_kword(void);

uint32_t z_zf_tx_free(void);
uint32_t z_zf_rx_level(void);
bool z_zf_aligned(void);
uint32_t z_zf_status(void);
uint32_t z_zf_errors(void);
void z_zf_flush(bool tx, bool rx);	// also clears the sticky flags and errors

#endif
