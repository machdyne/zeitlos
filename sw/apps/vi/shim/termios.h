/*
 * <termios.h> for Zeitlos.
 *
 * Present in some embedded newlib builds and absent from others, so
 * shimmed rather than relied on -- the port should not depend on which
 * toolchain the tree happens to be built with.
 *
 * nextvi's term_init() uses it for one thing: turning off canonical
 * mode, echo and signal characters. A Zeitlos port connection has none
 * of those to begin with -- bytes arrive exactly as the terminal
 * produced them -- so the stubs in posix_stubs.c succeed and change
 * nothing.
 */
#ifndef _ZEITLOS_TERMIOS_H
#define _ZEITLOS_TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

#define NCCS 32

struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t     c_cc[NCCS];
};

/* Only these three are named by term_init(). */
#define ICANON 0x0002
#define ISIG   0x0001
#define ECHO   0x0008

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);

#endif
