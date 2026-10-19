/*
 * kidgames -- z_dialog_help() for the host tests only.
 *
 * kgui.c's F1 opens the shared help box (docs/app_help.md), which runs
 * a message loop of its own; there is no wm on a build machine, so the
 * panel targets link this instead of sw/common/zdialog.c.
 */

#include "../../../common/zdialog.h"

void z_dialog_help(const z_dialog_ctx_t *c, const char *t, const char *x,
	const char *d)
{
	(void)c; (void)t; (void)x; (void)d;
}
