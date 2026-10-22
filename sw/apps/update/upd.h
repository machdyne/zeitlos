#ifndef UPD_H
#define UPD_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * update's core (docs/update.md): compare a release's manifest with the
 * card's /sys/manifest, fetch what changed, put it in place, and write
 * the card's manifest last.
 *
 * Portable C over upd_io_t. update.c binds it to fs, net and the
 * console on the board; tests/test_update.c binds it to a directory on
 * the host and a store made by release/zrelease's own code, so the two
 * sides are tested against each other.
 *
 * Everything that can stop half way -- a kill, a pulled cable, a power
 * cut -- leaves a card that the next run finishes: a file is fetched to
 * <path>.upd and checked, the old one renamed to <path>.old, the new one
 * renamed into place, the old one removed; /sys/manifest and
 * /sys/version are written last and describe what is really there.
 */

#include <stdint.h>
#include <stdbool.h>

// upd_io_t results
#define UPD_OK        0
#define UPD_ERR      -1     // anything else
#define UPD_ENOENT   -2     // no such file
#define UPD_EBUSY    -3     // open for writing by another program
#define UPD_EEXIST   -4     // rename: the destination exists

// The longest card path handled, with ".upd" added.
#define UPD_PATH_MAX 256

// A manifest past this is refused rather than fetched into memory.
#define UPD_MANIFEST_MAX (1024 * 1024)

// fetch_read() is given a buffer at least this large: one chunk of
// whatever the source sends (a TFTP block is 1468 bytes).
#define UPD_CHUNK_MAX 2048

typedef struct {
	void *ctx;

	// -- the card. Paths are absolute: "/apps/sheet". --
	int  (*open_read)(void *ctx, const char *path);      // handle >= 0, or UPD_E*
	int  (*open_write)(void *ctx, const char *path);     // creates or truncates
	int  (*read)(void *ctx, int h, void *buf, int n);    // bytes; 0 at the end
	int  (*write)(void *ctx, int h, const void *buf, int n); // bytes written
	int  (*close)(void *ctx, int h);                     // UPD_OK
	int  (*size)(void *ctx, const char *path, uint32_t *out); // UPD_OK if a file
	int  (*unlink)(void *ctx, const char *path);
	int  (*rename)(void *ctx, const char *from, const char *to);
	int  (*mkdir)(void *ctx, const char *path);          // UPD_OK if it now exists
	int  (*free_kb)(void *ctx, uint32_t *kb);            // UPD_OK

	// -- the release. Names are the server's: "release/v0.0.7/manifest". --
	int  (*fetch_open)(void *ctx, const char *name, char *err, int errlen);
	int  (*fetch_read)(void *ctx, uint8_t *buf, int cap, char *err, int errlen);
	                                                      // > 0 bytes, 0 end, < 0 error
	void (*fetch_close)(void *ctx);                       // also aborts one in progress

	// -- the person --
	void (*out)(void *ctx, const char *text);             // \n for a new line, \r to redo one
	uint32_t (*ms)(void *ctx);                            // a clock in milliseconds
	bool (*stop)(void *ctx);                              // true: stop as soon as it is safe
} upd_io_t;

typedef struct {
	const char *release;    // "v0.0.7", or "latest"
	const char *dir;        // the store's name on the server: "release"
	const char *source;     // for the summary: "tftp 192.168.1.10"
	bool dry_run;           // -n: say what would change, change nothing
	bool verify;            // -v: hash every file, trust no manifest
	bool force;             // -f: update though the release's ABI is not the flash's,
	                        // or LATEST names an older release than the card's
	const char *kernel_abi; // the running kernel's ABI id (z_abi()); "" or
	                        // NULL when it is too old to say (docs/update.md, "ABI")
	const char *sets;       // -s: sets to add, "games,audio", or "all"; NULL: none
	// Signatures (docs/distribution.md). need_sig: the manifest must be
	// signed (Ed25519, manifest.sig) by one of the nkeys 32-byte public
	// keys at `keys`, and every object is checked by SHA-256 against it.
	// Off for TFTP, which is for development on a LAN.
	bool need_sig;
	const uint8_t *keys;
	int nkeys;
} upd_opts_t;

// The most sets a release may offer.
#define UPD_SETS_MAX 24

// Exit statuses.
#define UPD_DONE        0   // the card matches the release
#define UPD_PARTIAL     1   // some files were left (in use, or failed); run again
#define UPD_FAILED      2   // nothing changed, or stopped early: the card is consistent
#define UPD_USAGE       3

int upd_run(const upd_io_t *io, const upd_opts_t *o);

// What the card has: /sys/version and the manifest's counts, and the
// ABI the card's files were built for beside the one the flash has.
int upd_show(const upd_io_t *io, const char *kernel_abi);

// Whether a release's files may be written on this flash (docs/update.md,
// "ABI"): UPD_ABI_SAME, or why not, or why it cannot be told.
#define UPD_ABI_SAME        0
#define UPD_ABI_DIFFERENT   1   // refused unless -f
#define UPD_ABI_NO_KERNEL   2   // the kernel predates Z_SYS_ABI: a warning
#define UPD_ABI_NO_RELEASE  3   // the release does not say: a note
int upd_abi_check(const char *release_abi, int release_len, const char *kernel_abi);

// Public keys as hex (64 digits each, separated by spaces or commas) into
// `keys`, 32 bytes each: how many, or -1 if one is not a key.
int upd_keys_parse(const char *hex, uint8_t *keys, int max);

#endif
