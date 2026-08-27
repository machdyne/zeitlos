#ifndef Z_XMODEM_H
#define Z_XMODEM_H

#include <stdint.h>

/*
 * XMODEM/CRC receiver -- `xmf` in sh.c.
 *
 * This exists alongside xfer.c rather than replacing it. The two solve
 * different problems:
 *
 *   xfer  -- our own protocol, needs the `xfer` utility on the host,
 *            carries an exact byte count.
 *   xmodem -- speaks to any ordinary terminal program (minicom, picocom,
 *            Tera Term, `sx`) with no host-side tooling at all, but has
 *            NO length field: the final block is padded to a block
 *            boundary and the padding has to be guessed at on the way
 *            in. See the trim note in xmodem.c.
 *
 * So xfer stays the right tool for uploading executables, where the
 * on-disk size is load-bearing (fs_exec_info() derives the data length
 * from f_size()); xmodem is the right tool when you're on someone
 * else's machine with nothing but a serial terminal.
 *
 * Blocking, deliberately, and for the same reason xfer_recv() is:
 * XMODEM's ACK/NAK timing doesn't compose with the shell's readline
 * loop, and an upload is a bounded action the user explicitly started.
 * Unlike xfer_recv() this one cannot hang forever -- every read has a
 * timeout, so a sender that never appears returns XMODEM_TIMEOUT
 * instead of wedging the shell.
 */

typedef enum {
	XMODEM_OK = 0,
	XMODEM_CANCELLED,	// sender sent CAN, or the user aborted
	XMODEM_TOO_LARGE,	// transfer exceeded the caller's buffer
	XMODEM_TIMEOUT,		// sender never appeared, or stopped mid-transfer
	XMODEM_SEQUENCE,	// block numbers went somewhere they can't go
} xmodem_result_t;

/*
 * Receives into `capacity` bytes at `addr_ptr` and returns the number
 * of bytes actually received. `result` (optional) gets the reason it
 * stopped.
 *
 * The buffer belongs to the caller -- allocation policy lives in sh.c,
 * next to the decision about how large an upload is reasonable, the
 * same way `xf` already does it. The return value is only meaningful
 * when *result is XMODEM_OK; on any other result the buffer holds a
 * partial transfer and should be discarded.
 */
uint32_t xmodem_recv(uint32_t addr_ptr, uint32_t capacity,
	xmodem_result_t *result);

// Human-readable form of a result code, for the shell to print.
const char *xmodem_strerror(xmodem_result_t result);

#endif
