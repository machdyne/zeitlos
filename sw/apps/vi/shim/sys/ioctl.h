/*
 * <sys/ioctl.h> for Zeitlos.
 *
 * Provided for one call: TIOCGWINSZ in term_init(), asking the
 * terminal how big it is.
 *
 * The stub fails deliberately rather than answering, so that
 * term_init() falls through to its own 80x25 defaults -- see
 * posix_stubs.c for why a confident wrong answer is worse than none.
 */
#ifndef _ZEITLOS_SYS_IOCTL_H
#define _ZEITLOS_SYS_IOCTL_H

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413

int ioctl(int fd, unsigned long request, ...);

#endif
