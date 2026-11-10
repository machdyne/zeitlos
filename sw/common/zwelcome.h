#ifndef Z_WELCOME_H
#define Z_WELCOME_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * When wm starts `welcome` by itself (docs/welcome_app.md, "When it opens
 * by itself"). Shared by wm, which decides, and welcome, which marks.
 *
 *   - a card with /sys/firstrun (Z_PATH_SYS_FIRSTRUN, zpaths.h): the
 *     release tool writes it on every card image, never in a manifest,
 *     so `update` never brings it back;
 *   - or no card at all, a store that can be written, and no
 *     Z_WELCOME_KV_SEEN in it: a machine that has never shown the
 *     welcome. A card that is in and has no mark is never interrupted.
 *
 * welcome removes the file and writes the key as soon as it starts,
 * before it asks anything.
 */

// The flash key/value store's key (docs/kvstore.md): "1" once this
// machine has shown the welcome. Absent on a new machine, and again after an
// image that erases the store.
#define Z_WELCOME_KV_SEEN   "apps.welcome.seen"

#endif
