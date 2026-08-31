#ifndef ZVERSION_H
#define ZVERSION_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * OS version.
 *
 * One string, here, because more than one thing wants to report it --
 * the info app displays it, and a boot banner or an `about` command
 * would want the same value rather than its own copy.
 *
 * Hand-maintained. There is deliberately no build date or git hash in
 * it: both would change the binary on every rebuild, which makes
 * "is the flashed image the one I just built?" harder to answer by
 * comparison, not easier.
 *
 * -- Why this is its own file --
 *
 * It was a #define partway down sw/common/zeitlos.h, which is included
 * by every app and by the kernel. That is still where it reaches all
 * of them from; zeitlos.h includes this. The reason for the split is
 * release/zrelease, which reads this value before a release build and
 * refuses to build v0.0.3 out of a tree that still says 0.0.2 -- the
 * failure that guards against is a release page and a running system
 * disagreeing about which version it is, discovered from a screenshot
 * months later.
 *
 * Reading one #define out of a 400-line header full of MMIO register
 * addresses is doable but fragile; reading it out of a file whose only
 * job is to hold it is not. `zrelease build --bump <version>` rewrites
 * the line below, and that edit is meant to be committed like any
 * other -- the tool changes it, it does not generate it, so the value
 * in a checked-out tree is always the value that was built.
 */

#define Z_OS_VERSION  "0.0.2"

#endif
