#ifndef PRINT_HOST_NET_H
#define PRINT_HOST_NET_H
/*
 * print's transport on the build machine: a POSIX socket where the
 * board uses net's raw socket (print.c). For the host tests only.
 */
#include "../run.h"
void host_transport(transport_t *t);
#endif
