#ifndef ZARCOPY_H
#define ZARCOPY_H
/*
 * Copying out of the memory-mapped flash window, a 32-bit word per
 * read. zar.c loads flash apps with it.
 *
 * rtl/spiflash.v serves EVERY read as one SPI transaction of 32 bits --
 * a byte load costs as much as a word load -- so the byte-by-byte copy
 * this replaces made four transactions where one would do: 235 KB of
 * `net` was 235,000 of them. Here the aligned middle is read a word at
 * a time and only the unaligned first and last few bytes a byte at a
 * time. The controller assembles words little-endian (the byte at the
 * lowest address in bits 7:0), so byte k of a word is the byte at
 * address + k.
 *
 * The destination is written a word at a time when it is aligned too,
 * a byte at a time otherwise: a misaligned store may trap.
 *
 * A header, so sw/os/tests/test_zarcopy.c can run exactly this code
 * against memcpy for every alignment and length.
 */
#include <stdint.h>

static inline void zar_copy(uint8_t *out, volatile const uint8_t *src, uint32_t n) {
	uint32_t i = 0;

	// until the SOURCE is aligned: bytes
	while (i < n && ((uintptr_t)(src + i) & 3u)) {
		out[i] = src[i];
		i++;
	}
	// the middle: a word per flash read
	if (!((uintptr_t)(out + i) & 3u)) {
		for (; i + 4 <= n; i += 4)
			*(uint32_t *)(out + i) = *(volatile const uint32_t *)(src + i);
	} else {
		for (; i + 4 <= n; i += 4) {
			uint32_t w = *(volatile const uint32_t *)(src + i);
			out[i] = (uint8_t)w;
			out[i + 1] = (uint8_t)(w >> 8);
			out[i + 2] = (uint8_t)(w >> 16);
			out[i + 3] = (uint8_t)(w >> 24);
		}
	}
	// what is left: bytes
	for (; i < n; i++)
		out[i] = src[i];
}

#endif
