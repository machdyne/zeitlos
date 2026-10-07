#ifndef BENCH_NETLIST_H
#define BENCH_NETLIST_H

/* bench -- netlists: the whole text of a .net file into the bench
 * (core.h). 0, or -1 with "line N: ..." in err. */
int bn_load(const char *text, char *err, int errlen);

#endif
