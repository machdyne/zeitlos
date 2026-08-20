/*
 * Zeitlos
 *
 * Stub for Machdyne Scheme's -DLIX build (sw/ext/ms/ms.c -- see that
 * submodule's README.md, "Embedding"). Upstream's original LIX target
 * (the Zucker SOC, ms's predecessor OS) expects a header with exactly
 * this name/path to exist on the include path -- but building with
 * -DLIX compiles out every actual file-I/O call ms.c would otherwise
 * route through it (fopen/fread/fseek/fclose -- `load`/`file->str`
 * are compiled out entirely, see ms.c's own top-of-file build
 * comment). Nothing in ms.c under -DLIX actually declares or calls
 * anything through this header -- it only needs to exist and be
 * findable. Deliberately empty.
 *
 * Shared here (sw/common/ms_platform/), not under sw/apps/repl/,
 * since any future app embedding ms (see this project's own planning
 * notes on eventually wanting Scheme reachable from more than one
 * place) needs the exact same stub -- no reason for each to carry
 * its own copy.
 */
