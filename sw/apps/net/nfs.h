#ifndef NFS_H
#define NFS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net's NFS client: NFSv3 over TCP, read-only, for fs's /nfs volume.
 * The protocol fs and the kernel shell speak to it is znfsproto.h; the
 * design is docs/nfs.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include "../../common/zeitlos.h"

// A Z_NET_NFS_REQ (net.c's message loop). Answered at once, or when the
// server has answered.
void nfs_handle_msg(const z_msg_t *msg);

// Mount "host:/export" at start (net.cfg's nfs=); nobody waits for it.
void nfs_mount_at_boot(const char *spec);

// Timeouts: every pass of net's loop.
void nfs_poll(void);

#endif
