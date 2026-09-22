#ifndef Z_JUMP_H
#define Z_JUMP_H
/*
 * Jumploaders: reading and changing their boot address in place.
 * docs/zboot.md sec. 5.
 *
 * A jumploader is a bitstream built by `zfpga jump` (zfpga pack -J):
 * its header carries a ZJUMP1 line saying where the eight bits of its
 * boot address sit in the stream and which CRCs cover them. Changing
 * the address is flipping those bits and recomputing those CRCs -- no
 * chip database, no re-packing, a few hundred bytes of flash.
 *
 * Freestanding: shared by the kernel (on flash, through sector
 * buffers) and zfpga's host test (on a file in memory). Bytes are read
 * and written through callbacks, at offsets from the jumploader's
 * start; zjump_set() reads back what it has written.
 */
#include <stdint.h>

#define ZJUMP_BITS 8
#define ZJUMP_MAX_CRC 8

typedef uint8_t (*zjump_rd_t)(void *ctx, uint32_t off);
typedef void (*zjump_wr_t)(void *ctx, uint32_t off, uint8_t v);

typedef struct {
	uint32_t pre;                   // the preamble's offset: descriptor offsets count from it
	uint32_t b_off[ZJUMP_BITS];     // bit j of the address (addr[16 + j]) ...
	uint8_t b_mask[ZJUMP_BITS];     // ... is this bit of this byte
	uint32_t c_from[ZJUMP_MAX_CRC]; // each CRC covers [from, at) ...
	uint32_t c_at[ZJUMP_MAX_CRC];   // ... and sits at at, at + 1 (MSB first)
	int n_crc;
} zjump_t;

// 0 if the bytes at offset 0 are a jumploader (header, ZJUMP1 line,
// preamble), within `max` bytes of header; -1 otherwise.
int zjump_parse(zjump_rd_t rd, void *ctx, uint32_t max, zjump_t *j);

// Its boot address: 0x000000 - 0xFF0000, 64 KB aligned.
uint32_t zjump_target(const zjump_t *j, zjump_rd_t rd, void *ctx);

// Point it at `target` (64 KB aligned, below 16 MB): writes the address
// bits, then the CRCs over what it wrote. Everything it touches lies
// between zjump_span()'s first and last byte.
void zjump_set(const zjump_t *j, uint32_t target, zjump_rd_t rd, zjump_wr_t wr, void *ctx);
void zjump_span(const zjump_t *j, uint32_t *first, uint32_t *last);

#endif
