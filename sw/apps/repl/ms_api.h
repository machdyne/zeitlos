#ifndef MS_API_H
#define MS_API_H

#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>

/*
 * Zeitlos
 *
 * ms.c (sw/ext/ms, a git submodule -- see this app's own README/build
 * notes) is a single translation unit with no separate public header
 * of its own: under -DLIX it has no main() (its REPL/main section is
 * `#ifndef LIX`), so it's meant to be compiled as a standalone .o and
 * linked against an embedder's own main() -- but everything an
 * embedder would call is declared inline, mixed in with ms.c's own
 * `static` internals, not split out anywhere. This header is
 * literally just the small subset of those declarations that AREN'T
 * `static` in ms.c (i.e. the ones actually reachable from outside
 * it), copied here by hand so repl.c has something to #include
 * without pulling in the whole 2000+ line implementation file or
 * poking at its internals.
 *
 * Keep in sync BY HAND with ms.c's own "PUBLIC API" declarations
 * block (the un-`static` lines in the same declaration block ms.c's
 * own internal forward-declarations live in, near the top of the
 * file) if sw/ext/ms is ever updated to add/remove/change one of
 * these -- there's no build-time check that this header and ms.c
 * actually agree, a mismatch would just fail to link (a wrong
 * signature) or misbehave at runtime (a genuinely wrong prototype
 * that still happens to link). Small, deliberate, easy to audit --
 * the alternative (patching ms.c to split out a real ms.h) is a much
 * bigger diff against upstream for not much benefit at this size.
 */

typedef struct ms_val ms_val;	/* fully opaque here on purpose -- repl.c
	 * only ever holds ms_val* and passes it back into ms's own
	 * functions, never looks inside one, so it never needs the real
	 * struct definition (ms.c's own, sitting among its `static`
	 * internals) */

/* -- evaluation -- */
ms_val *ms_eval(ms_val *x, ms_val *env);
ms_val *ms_read(const char **s);
void ms_print(ms_val *v, bool readable);
char *ms_to_string(ms_val *v, bool readable);	/* caller must free() the
	 * result -- see this project's sw/ext/ms-zeitlos.patch, which is
	 * what makes this non-static in the first place; upstream ms.c
	 * keeps it internal */
void ms_load_string(const char *src, ms_val *env);

/* -- memory usage (not part of upstream ms.c -- see ms.c's own
 * comment right above these two definitions) -- */
long ms_heap_used(void);	/* cells currently live, out of MS_HEAP_SIZE
	 * total (a compile-time constant, already known to any caller
	 * that was itself built with the same -DMS_HEAP_SIZE, see
	 * sw/apps/repl/Makefile's MS_SIZE_CFLAGS) */
size_t ms_cell_size(void);	/* sizeof(ms_val) on THIS build/target --
	 * varies with pointer width, don't assume a fixed number */

/* -- session lifecycle -- */
extern ms_val *ms_global_env;
void ms_init(void);
void ms_deinit(void);
void ms_init_lix(bool stdlib);	/* only exists in ms.o if ms.c itself
	 * was built with -DLIX (sw/apps/repl/Makefile always does) --
	 * deliberately NOT wrapped in `#ifdef LIX` here: that guard would
	 * depend on whichever translation unit happens to include THIS
	 * header also defining LIX itself, which nothing requires (only
	 * ms.c's own compile step needs -DLIX; repl.c calling this
	 * function doesn't need to redefine it just to see the
	 * prototype) -- caught by testing exactly this build shape,
	 * where leaving the guard in produced a silent implicit-
	 * declaration warning instead of a real prototype. */

/* -- panic recovery (ms.c's own extension for embedders, not part of
 * upstream Lisp semantics -- see ms.c's own header comment on
 * ms_panic_before_try() for the full usage pattern and WHY the
 * setjmp() call itself can't be wrapped in a helper function; that
 * constraint is real and repl.c's own callers respect it, every
 * setjmp() below happens directly in the function that goes on to
 * call ms_eval(), never through a shared helper) -- */
extern jmp_buf ms_panic_recovery;
extern bool ms_panic_recovery_armed;
extern bool ms_exit_requested;
extern int ms_exit_code;
void ms_panic_before_try(void);
void ms_panic_after_recover(void);
void ms_panic_disarm(void);

extern int ms_out_of_memory_count;

#endif
