#ifndef K_CFG_H
#define K_CFG_H

/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The kernel's configuration store -- /zeitlos.cfg, loaded at boot.
 * See sw/common/zcfg.h for the format and the app API, and
 * docs/config.md for the whole design.
 */

#include <stdbool.h>
#include <stdint.h>

#include "../common/zobj.h"

// Reads /zeitlos.cfg into the store, replacing whatever was there, and
// applies the settings the kernel itself owns (system.video.mode).
// Returns the number of settings loaded; 0 means no file, which is not
// an error -- everything uses its default. *ignored gets the count of
// malformed lines. Prints a one-line summary when `verbose`.
//
// Safe to call with processes running: the store is replaced under
// k_fs_enter() (no preemption), so no app reads a half-loaded store.
int k_cfg_load(bool verbose, uint32_t *ignored);

// The card was not up when boot read the config (issue #7): the first
// access to a freshly powered card can fail, and with the core apps in
// flash boot does not wait for it. k_cfg_defer() -- boot, then -- marks
// the config as still to be read. k_cfg_retry(), called ONCE by init()
// after it has loaded the shells from the card, reads it if the card
// has come up by then, and otherwise gives up for this boot. Only init
// retries: nothing in the filesystem watches for a late card.
void k_cfg_defer(void);
void k_cfg_retry(void);

// The store's current value for `key`, or NULL if the file does not set
// it. The pointer is into the store and is invalidated by the next
// k_cfg_load(); copy it.
const char *k_cfg_find(const char *key);

// Settings loaded, and the index'th one (file order).
uint32_t k_cfg_count(void);
bool k_cfg_at(uint32_t index, const char **key, const char **val);
uint32_t k_cfg_generation(void);

// Syscall handlers -- Z_SYS_CFG_GET/_ENTRY/_RELOAD, sw/common/syscalls.def.
z_obj_t *k_cfg_get(z_obj_t *args);
z_obj_t *k_cfg_entry(z_obj_t *args);
z_obj_t *k_cfg_reload(z_obj_t *args);

// The `cfg` shell command: `cfg`, `cfg reload`, `cfg get <key>`.
void k_cfg_shell(const char *sub, const char *arg);

#endif
