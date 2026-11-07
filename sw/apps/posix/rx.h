#ifndef RX_H
#define RX_H
/*
 * posix -- a small regular expression engine, for sed (text.c).
 *
 * POSIX basic regular expressions as sed takes them, or extended ones
 * (sed -E):
 *
 *   .  [abc]  [^a-z]  [[:digit:]]  ^  $  *         both
 *   \( \)  \{ \}-less  \+  \?  \|                   basic (GNU's extensions)
 *   ( )  +  ?  |                                    extended
 *   \1 .. \9 in a replacement, & the whole match    (text.c)
 *
 * No back-references inside a pattern, no intervals ({m,n}), no
 * collating elements. Compiled to a small program and run by a
 * backtracking machine with its own stack -- not C recursion, because a
 * command runs on an 8 KB coroutine stack (job.c) and a recursive
 * matcher's depth is the length of the line. Leftmost, then greedy, as
 * sed and Perl choose; not POSIX's leftmost-longest, which differs only
 * for alternations like `a|ab`.
 *
 * One expression compiled at a time per rx_t; a sed script holds one
 * per command (text.c), so programs are small and fixed in size.
 */
#include <stdint.h>
#include <stdbool.h>

#define RX_PROG     256         // instructions
#define RX_SETS     8           // bracket expressions
#define RX_GROUPS   10          // \0 (the whole match) .. \9

typedef struct { uint8_t op; uint8_t a; uint16_t x, y; } rx_ins_t;

typedef struct {
	rx_ins_t prog[RX_PROG];
	int n;
	uint8_t sets[RX_SETS][32];
	int nsets;
	int ngroups;                // groups in the expression, not counting \0
	int nloops;
	bool anchored;              // begins with ^: tried at the start only
} rx_t;

// Compiles `pat` (n bytes). False with a reason in `err` if it is not
// one this engine takes.
bool rx_compile(rx_t *r, const char *pat, int n, bool extended, char *err, int errcap);

// The first match in s[from..len): true with cap[2k], cap[2k+1] the
// start and end of group k (cap[0], cap[1] the whole match; -1 for a
// group that took no part). `bol`: s[0] is the start of a line, for ^.
// False when there is none -- or, rarely, when an expression would
// take more steps than allowed (*toolong set).
bool rx_exec(const rx_t *r, const char *s, int len, int from, int cap[2 * RX_GROUPS], bool *toolong);

#endif
