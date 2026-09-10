/*
 * zeitlos-sim: simos.h -- the operating system the simulator does not
 * have.
 *
 * machine.c models the SOC: the bus, VRAM, the rasterizer, the
 * blitter, the UART. It also answers the four UART syscalls and EXIT,
 * because those are one line each and genuinely are the machine. This
 * file answers everything else an app can ask the kernel for -- the
 * filesystem above all, plus uptime, pids and messaging -- against the
 * host instead of against a real Zeitlos.
 *
 * The split is on purpose. sw/os is 700KB of scheduler, memory pool,
 * FatFs and process table, and reimplementing any of it here would
 * produce a second, subtly different Zeitlos to keep in step with the
 * first. What this file does instead is answer the SYSCALLS, at the
 * ABI boundary, with the simplest host-backed thing that satisfies the
 * contract in sw/os/fsapi.h and sw/common/zfs.h. An app cannot tell
 * the difference; a kernel developer should not try to use it as one.
 *
 * -- What is deliberately NOT modelled --
 *
 * There is one process. So there is no scheduler, no preemption, no
 * second process to send a message to, and no pid registry with
 * anything in it. Z_SYS_MSG_READ always reports an empty mailbox and
 * Z_SYS_PID_LOOKUP always fails, which is not a stub so much as the
 * truthful answer: there is no `wm` here. An app that handles those
 * correctly falls back to standalone behaviour (see `term`'s local-echo
 * path when no port provider answers), and an app that does not will
 * fail here in exactly the way it would on a machine where wm had not
 * been started -- which is a bug worth finding.
 */

#ifndef ZSIM_SIMOS_H
#define ZSIM_SIMOS_H

#include <stdint.h>

struct machine;

/*
 * The host directory the guest filesystem is backed by. Everything the
 * app opens resolves under here and nothing escapes it (see
 * simos_hostpath()). NULL or unset means the filesystem syscalls all
 * fail cleanly, which is the right default: silently reading the
 * developer's working directory because an app asked for "wm" is not a
 * behaviour anybody wants to discover later.
 */
void simos_set_root(const char *dir);
const char *simos_get_root(void);

/*
 * Handles a syscall machine.c did not. Returns 1 for &z_ok, 0 for
 * &z_fail -- the caller turns that into the pointer, since only it
 * knows where the two shared return objects live in guest memory.
 *
 * An id this file does not know is reported once, by name where
 * possible, and answered with failure. Failing is better than
 * succeeding vacuously: an app that gets &z_ok back from a syscall
 * that did nothing goes on to read an OUT field that was never
 * written.
 */
int simos_syscall(struct machine *m, uint32_t id, uint32_t obj);

#endif
