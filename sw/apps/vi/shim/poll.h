/*
 * <poll.h> for Zeitlos.
 *
 * No embedded newlib or picolibc provides one -- poll() is a
 * multiplexing call and these libraries have nothing to multiplex.
 * nextvi uses it in exactly two places: before each single-byte read
 * in term_read(), and around the pipes of a `:!` shell-out.
 *
 * The first has nothing left to wait for here, because the read
 * underneath it is z_stdin_hook, which does its own blocking and
 * message pumping (sw/apps/vi/vi_zeitlos.c). The second cannot happen
 * at all: there is no fork.
 *
 * So the implementation in posix_stubs.c always reports ready. This
 * header exists so that vi.c's `#include <poll.h>` resolves; the
 * struct layout has to match what term.c initialises, which is
 * `{STDIN_FILENO, POLLIN}`.
 */
#ifndef _ZEITLOS_POLL_H
#define _ZEITLOS_POLL_H

struct pollfd {
	int   fd;
	short events;
	short revents;
};

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

typedef unsigned long nfds_t;

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif
