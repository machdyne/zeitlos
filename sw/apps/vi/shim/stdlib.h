/*
 * <stdlib.h> for Zeitlos -- the real one, with `itoa` renamed.
 *
 * newlib declares `itoa(int, char *, int)` as a non-standard
 * extension. nextvi defines its own `itoa(int, char[])`. Two arguments
 * against three is a conflicting declaration, and the build stops.
 *
 * -- Why not -Ditoa=nextvi_itoa --
 *
 * That was the first attempt and it fails for a reason worth
 * recording: a command-line -D applies to the WHOLE translation unit,
 * including newlib's own header. So newlib's three-argument
 * declaration became `nextvi_itoa` too, and the conflict survived the
 * rename intact.
 *
 * The rename has to happen AFTER newlib has been read and BEFORE
 * nextvi's vi.h -- and vi.c includes them in that order with nothing
 * in between, so there is nowhere to put it except here.
 *
 * #include_next reaches the real header past this one; -Ishim comes
 * first on the include path, which is what makes that work.
 *
 * The alternative was renaming it in sw/ext/nextvi, which works and
 * costs a local change to vendored code on every re-vendor. This costs
 * one file that never changes.
 */
#ifndef _ZEITLOS_STDLIB_H
#define _ZEITLOS_STDLIB_H

#include_next <stdlib.h>

#undef itoa
#define itoa nextvi_itoa

#endif
