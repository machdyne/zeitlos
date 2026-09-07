/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * MOVED. The SHA-256 implementation that was here is now
 * sw/common/zsha256.c -- see that file and ssh_sha256.h for why.
 *
 * THIS FILE IS DEAD AND CAN BE DELETED. It is still present only
 * because these changes were delivered as an archive unpacked over
 * the tree, and an archive cannot remove a file. Nothing compiles it:
 * sw/apps/net/Makefile builds zsha256.o from sw/common instead.
 *
 * Left as an explicit stub rather than an empty file so that anyone
 * who opens it looking for the implementation is told where it went,
 * rather than editing a copy that does nothing.
 */

// A translation unit with no declarations is undefined behaviour in
// C99; this keeps it legal if something does still compile it.
typedef int ssh_sha256_moved_to_sw_common;
