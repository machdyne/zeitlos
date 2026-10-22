#ifndef BKWIN_H
#define BKWIN_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup's window (docs/backup.md, "The window"): whether you are backed
 * up, in a headline, and one button to back up; Show backups opens the
 * drive in files; the titlebar's Aa makes it a strip for a corner. Its
 * work runs a slice at a time (bkjob.c), so the window answers at once
 * whatever it is doing.
 */

#include "bk.h"

#define BKWIN_NO_WM  -100   // no window could be made: the caller goes on without

// Runs the window until it is closed. Its exit status, or BKWIN_NO_WM.
int bkwin_run(void);

// From backup.c: the settings in zeitlos.cfg, and the files, clock and
// owner the jobs use.
void backup_settings(bk_opts_t *o);
void backup_base_io(bk_io_t *io);

#endif
