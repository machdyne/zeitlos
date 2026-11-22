/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn on Linux -- the machine's side (lplat.h) over POSIX: files, the
 * clock, speech through espeak-ng if it is there. The boxes (prompt,
 * confirm) and running another app are main.c's: it fills them in.
 *
 * learn names its places as Zeitlos does (/opt/ark/edu..., /data/learn);
 * here they are folders of this computer (docs/learn_linux.md):
 *
 *   /opt/ark/NAME/...   a pack: a folder given on the command line
 *   /data/learn/...     $XDG_DATA_HOME/learn (or --data DIR)
 *   /usb/...            nothing: there is no stick
 */

#ifndef LPLAT_LINUX_H
#define LPLAT_LINUX_H

#include <stdbool.h>
#include "../lplat.h"

extern lplat_t linux_plat;

// where learners and progress are kept (made if missing); false if it cannot be
bool ll_set_data(const char *dir);
const char *ll_data(void);

// A folder from the command line: one course, a pack (a folder of
// courses: an Ark checkout's edu/), or a folder of packs. Returns how
// many packs it added; for one course, `open` gets the course's path as
// learn names it (to open at start), else "".
int ll_add_folder(const char *dir, char *open, int cap);
int ll_npacks(void);
const char *ll_pack_name(int i);
const char *ll_pack_dir(int i);

// speech: espeak-ng if it can be found (and not turned off)
void ll_speech(bool on);
bool ll_speech_available(void);

#endif
