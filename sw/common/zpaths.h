#ifndef ZPATHS_H
#define ZPATHS_H

/*
 * zpaths.h -- every fixed location on the card, in one place.
 *
 * See docs/layout.md for what goes where and why.
 *
 * THE RULE: a path the system or an app opens by a fixed name is
 * written here and nowhere else. Code uses the name; the release tool
 * (release/lib/zpaths.py) reads this file to know where to put what.
 * So the card and the code cannot disagree about where a file is, and
 * moving one is a change to one line.
 *
 * `zrelease check` enforces it: a string literal in sw/os, sw/apps or
 * sw/common that names a location in this file, or starts with one of
 * the card's top-level folders, fails the check unless it is here.
 *
 * -- the format, which a script reads --
 *
 * One definition per line, exactly:
 *
 *     #define Z_NAME   "literal"
 *
 * No expressions, no concatenation, no continuation lines: the value is
 * the complete string, so the Python side is a regular expression and
 * cannot disagree with the compiler about what a name means.
 *
 * Z_DIR_* names a directory, Z_PATH_* a file. Directory values have no
 * trailing slash; code that needs one writes Z_DIR_X "/", which the
 * compiler joins into a single literal.
 *
 * -- the names say what, not where --
 *
 * Z_DIR_CRON_DATA is "where cron keeps its files", not "/data/cron". A
 * value can change without touching any code that uses the name.
 */

/* -- the system -- */

/* The system's own files: not any app's (docs/layout.md, rule 3). */
#define Z_DIR_SYS                 "/sys"
#define Z_PATH_SYS_CONFIG         "/sys/zeitlos.cfg"
#define Z_PATH_SYS_VERSION        "/sys/version"

/* Programs, one flat file each, and everything else each app has.
 * An app names its own files with z_data_file() (sw/common/zdata.h);
 * the per-app names below are for files the release ships or another
 * app reads. */
#define Z_DIR_APPS                "/apps"
#define Z_DIR_DATA                "/data"

#define Z_DIR_DOCS                "/docs"

/* The person's own files. Empty on a new card; the first directory a
 * save or open dialog shows, and where a shell starts. */
#define Z_DIR_HOME                "/home"

/* Scratch, gone at the next boot: the ramdisk when there is one, a
 * card directory the kernel empties at boot when there is not
 * (sw/os/fs/fs.c, docs/layout.md). */
#define Z_DIR_TMP                 "/tmp"

/* The kernel console's `ss` (sw/os/sh.c): something the person asked
 * for, so it goes where their things go. */
#define Z_PATH_SCREENSHOT         "/home/ss.bin"

/* Mount prefixes (sw/os/fs/fs.c, docs/ramdisk.md, docs/usb_host.md).
 * Not directories on the card: a directory of either name at the
 * card root cannot be reached. */
#define Z_DIR_RAM                 "/ram"
#define Z_DIR_USB                 "/usb"

/* -- media: examples that ship, and the person's own -- */

#define Z_DIR_MEDIA_AUDIO         "/media/audio"
#define Z_DIR_MEDIA_IMAGES        "/media/images"

/* -- the Ark library and ask's indexes of it (docs/ask_app.md) --
 * The documents are an optional package no single app owns, so they
 * are in /opt, where `read` and `files` find them without ask. The
 * index of each pack is ask's own data. A pack is the two halves,
 * matched by name: /opt/ark/<pack> and /data/ask/<pack>. tools/ask
 * reads these two values to lay out what it builds, but an index stores
 * document paths relative to its pack, so packs do not depend on them. */

#define Z_DIR_ARK                 "/opt/ark"
#define Z_DIR_ASK_PACKS           "/data/ask"

/* -- per app, by app -- */

/* automate (docs/automate.md, docs/demo.md) */
#define Z_DIR_AUTOMATE_DATA       "/data/automate"
#define Z_PATH_AUTOMATE_DEFAULT   "/data/automate/demo.zds"

/* basic: its disk, the one directory LOAD and SAVE see
 * (docs/basic_app.md) */
#define Z_DIR_BASIC_PROGRAMS      "/data/basic"

/* bbs (docs/bbs.md). The default of apps.bbs.dir. */
#define Z_DIR_BBS                 "/data/bbs"

/* bench: the netlists it ships, where its open dialog starts
 * (docs/bench.md) */
#define Z_DIR_BENCH_EXAMPLES      "/data/bench/examples"

/* casino and its games (docs/casino_bank.md). The bank is casino's;
 * poker, roulette, blackjack, slots and craps use it from here. */
#define Z_DIR_CASINO_DATA         "/data/casino"
#define Z_PATH_CASINO_BANK        "/data/casino/bank.dat"

/* chip8: the ROMs it ships, where its open dialog starts
 * (docs/chip8_app.md) */
#define Z_DIR_CHIP8_ROMS          "/data/chip8/roms"

/* cron (docs/cron.md). Also probed by the kernel at boot. */
#define Z_DIR_CRON_DATA           "/data/cron"
#define Z_PATH_CRON_CONFIG        "/data/cron/cron.cfg"
#define Z_PATH_CRON_STATE         "/data/cron/state"
#define Z_PATH_CRON_LOG           "/data/cron/log"

/* fed (docs/fed.md). The default of apps.fed.dir. */
#define Z_DIR_FED                 "/data/fed"

/* jfont (docs/text_encoding.md, "Japanese") */
#define Z_PATH_JFONT_FONT         "/data/jfont/jp12.zfn"

/* kidgames (docs/kidgames_app.md) */
#define Z_DIR_KIDGAMES_DATA       "/data/kidgames"
#define Z_PATH_KIDGAMES_SAVE      "/data/kidgames/kidgames.sav"

/* ls99: one folder per LS-99 placed on a bench (docs/ls99.md) */
#define Z_DIR_LS99_MODULES        "/data/ls99"

/* mesh (docs/mesh_app.md) */
#define Z_DIR_MESH_DATA           "/data/mesh"
#define Z_PATH_MESH_LOG           "/data/mesh/mesh.log"

/* net (docs/networking.md, docs/esp32link.md). net is a core app, in
 * flash; its data is on the card like any other app's. */
#define Z_DIR_NET_DATA            "/data/net"
#define Z_PATH_NET_CONFIG         "/data/net/net.cfg"
#define Z_PATH_NET_IP             "/data/net/net.ip"

/* netserve (docs/netserve.md). The HTTP root is the default of the
 * directory in apps.netserve.http. */
#define Z_PATH_NETSERVE_AUTHKEYS  "/data/netserve/authkeys"
#define Z_DIR_NETSERVE_WWW        "/data/netserve/www"

/* tts (docs/tts.md, docs/tts_data.md) */
#define Z_PATH_TTS_PACK           "/data/tts/speech/en.spk"

/* vi, nextvi in a term window (docs/posix.md) */
#define Z_PATH_VI_ARGS            "/tmp/vi.args"

/* web (docs/web_app.md, docs/tls.md) */
#define Z_DIR_WEB_DATA            "/data/web"
#define Z_PATH_WEB_ROOTS          "/data/web/roots.der"
#define Z_PATH_WEB_PINS           "/data/web/pins.txt"
#define Z_PATH_WEB_SPOOL          "/tmp/web.spool"

/* zcc (docs/zcc.md) */
#define Z_DIR_ZCC_LIBZ            "/data/zcc/libz"
#define Z_DIR_ZCC_INCLUDE         "/data/zcc/libz/include"
#define Z_DIR_ZCC_EXAMPLES        "/data/zcc/examples"
#define Z_PATH_ZCC_ARGS           "/tmp/zcc.args"

/* zerdesk (docs/remote_desktop.md) */
#define Z_PATH_ZERDESK_PAGE       "/data/zerdesk/index.html"

/* zfpga (docs/zfpga.md) */
#define Z_DIR_ZFPGA_DB            "/data/zfpga"
#define Z_PATH_ZFPGA_ARGS         "/tmp/zfpga.args"

#endif
