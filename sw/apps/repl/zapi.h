#ifndef ZAPI_H
#define ZAPI_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The Zeitlos Scheme API -- see docs/scheme_api.md for the full
 * design writeup. Every C-backed procedure Scheme code running inside
 * `repl` can call (beyond ms's own stdlib) is registered here, via
 * ms_def_builtin() (sw/ext/ms/ms.c). This file is deliberately the
 * ONLY place new procedures get added -- ms.c itself never needs
 * touching again once its own small registration/construction patch
 * (docs/scheme_api.md \S3) is in place.
 */

// registers every zapi_* procedure below into ms_global_env. Call
// once, from main(), right after ms_init_lix() succeeds -- same
// ordering ms_stdlib.l's own load already needs (a zapi_* builtin
// could in principle be called from Scheme code loaded as part of a
// future stdlib addition, so this needs to be done before anything
// else gets a chance to eval).
void zapi_register(void);

// destroys the Scheme-owned window with this wm window id (if this
// process still has one open under it) -- see zapi.c's own comment
// for the full writeup. Called from repl.c's main loop on Z_WM_CLOSE
// (sw/common/zwm.h) -- the one piece of the window-close protocol
// that has to live in repl.c itself rather than zapi.c, since
// zapi.c's own procedures only ever run from an ms_eval() call, never
// from the message loop directly.
void zapi_win_close(int id);

#endif
