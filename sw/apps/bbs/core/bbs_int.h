#ifndef BBS_INT_H
#define BBS_INT_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- what the core's files share. Not for the platforms.
 */
#include "bbs.h"

// ------------------------------------------------------------------
// configuration: <datadir>/bbs.cfg, "key: value" lines (cfg.c)
// ------------------------------------------------------------------

typedef struct {
	char name[48];                 // the BBS's name, shown everywhere
	char sysop[BBS_HANDLE_MAX * 4 + 1];   // shown on the welcome screen, informational
	int nodes;                     // callers at once, 1..BBS_NODES_MAX
	bool new_users;                // may callers make accounts
	int idle_minutes;              // logged in, nothing typed: goodbye
	int login_seconds;             // to finish logging in
	int pw_iterations;             // hashing new passwords (users.c)
	int new_level;                 // a new account's level
	char fed[128];                 // where this node's fed is: a socket path on
	                               // Linux, a port name ("fed0") on Zeitlos; "" none
} bbs_cfg_t;

extern bbs_cfg_t bbs_cfg;
extern char bbs_dir[BBS_PATH_MAX];     // the data directory, no trailing slash

void cfg_defaults(bbs_cfg_t *c);
void cfg_parse(bbs_cfg_t *c, const char *text, uint32_t len);
bool cfg_load(void);
// "<datadir>/<rel>" into out; false if it does not fit.
bool bbs_path(char *out, const char *rel);
void bbs_logf(const char *fmt, ...);

// ------------------------------------------------------------------
// users: <datadir>/users.dat, fixed-size records (users.c)
// ------------------------------------------------------------------

#define USER_REC_SIZE   256
#define USER_HANDLE_BYTES 64           // UTF-8, NUL-terminated, <= BBS_HANDLE_MAX characters
#define USER_LOC_BYTES  48

#define USER_F_SYSOP    0x0001
#define USER_F_DISABLED 0x0002
#define USER_F_LINE_EDITOR 0x0004     // writes a line at a time even with ANSI (profile: E)

#define LEVEL_SYSOP     255

// Terminal preferences. AUTO: what detection found.
enum { CS_AUTO = 0, CS_UTF8, CS_CP437, CS_ASCII };
enum { COLOR_AUTO = 0, COLOR_ON, COLOR_OFF };

typedef struct {
	uint32_t id;                   // 1-based; the record number + 1
	char handle[USER_HANDLE_BYTES];
	char location[USER_LOC_BYTES];
	uint16_t flags;
	uint8_t level;
	uint8_t charset;               // CS_*
	uint8_t color;                 // COLOR_*
	uint8_t rows;                  // 0: detected
	uint32_t pw_iter;
	uint8_t salt[16];
	uint8_t hash[32];
	uint32_t created, last_login, prev_login;   // Unix seconds
	uint32_t calls;
} user_t;

// The number of records, or -1 if users.dat cannot be read.
int users_count(void);
// Reads record `idx` (0-based). False if there is none.
bool users_read(int idx, user_t *u);
// Finds a handle, compared without case. Returns the index or -1.
int users_find(const char *handle);
// Writes a user back in place (u->id says where).
bool users_write(const user_t *u);
// Appends a new user; sets u->id. False if the file cannot grow.
bool users_add(user_t *u);

// Passwords: salted, iterated SHA-256 (docs/bbs.md, "Passwords").
void pw_set(user_t *u, const char *pw, int iterations);
bool pw_check(const user_t *u, const char *pw);

// A handle's key for comparison: ASCII letters folded to lower case,
// everything else as it is.
void handle_key(const char *in, char *out, uint32_t cap);
// Why a handle cannot be used, or NULL if it can.
const char *handle_problem(const char *h);

// Serialisation, exposed for the tests.
void user_pack(const user_t *u, uint8_t rec[USER_REC_SIZE]);
bool user_unpack(user_t *u, const uint8_t rec[USER_REC_SIZE]);

// ------------------------------------------------------------------
// messages (msgbase.c): areas, the log and its index, last read
// ------------------------------------------------------------------

#define AREA_MAX      32              // the mail and 31 forums
#define AREA_TAG_MAX  16
#define IDX_SIZE      32
#define MSG_HEAD_MAX  1024            // a message's headers, at most
// A message's body, at most: the editor's buffer, allocated only while
// someone writes. 6 KB on Zeitlos (the Makefile), 8 KB elsewhere.
// Reading has no such limit: bodies are paged from the log.
#ifndef MSG_BODY_MAX
#define MSG_BODY_MAX  8192
#endif
#define SUBJECT_MAX   60              // characters

#define IDX_DELETED   1
#define IDX_READ      2               // mail: the recipient has read it

typedef struct {
	char tag[AREA_TAG_MAX + 1];
	char name[48];
	char desc[96];
	uint8_t rlevel, wlevel;
	bool is_mail;                     // area 0
	uint32_t count;                   // messages in the index
	char topic[97];                   // its zfed topic ("" : local only) -- docs/bbs.md, "Federation"
} area_t;

typedef struct {
	uint32_t off, len;                // the record in the log
	uint32_t date, from_id, to_id;
	uint32_t reply;                   // the number it replies to, in this area; 0 none
	uint32_t flags;                   // IDX_*
} idx_t;

typedef struct {
	int num;
	char id[72];                      // node:area:number, or a zfed object id (64 hex)
	char from[USER_HANDLE_BYTES], to[USER_HANDLE_BYTES];
	char subject[SUBJECT_MAX * 4 + 1];
	char reply_id[72];
	char post[33];                    // a federated post's token: the same post twice is one
	uint32_t from_id, to_id, date, reply;
	uint32_t body_off, body_len;      // in the log
} msg_t;

extern area_t bbs_area[AREA_MAX];
extern int bbs_nareas;
extern char bbs_node_id[17];

bool node_id_load(void);
void areas_parse(const char *text, uint32_t len);
bool areas_load(void);
int area_find(const char *tag);
void area_repair(int a);
int msg_count(int a);
bool msg_idx(int a, int num, idx_t *e);
int msg_idx_many(int a, int first, idx_t *e, int k);
bool msg_idx_write(int a, int num, const idx_t *e);
bool msg_head(int a, int num, msg_t *m, idx_t *e);
uint32_t msg_body(int a, const msg_t *m, char *buf, uint32_t cap);
int msg_post(int a, msg_t *m, const char *body, uint32_t blen);   // its number, or -1
// The number of a message among an area's last `back` with this id, or
// this post token (either may be NULL); 0 if none.
int msg_find(int a, const char *id, const char *post, int back);
extern char bbs_fed_network[33];      // the network the federated forums are in; "" none

// fedlink.c: the link to this node's fed (docs/bbs.md, "Federation")
void fed_init(void);
// Publishes a post to federated forum a: 1 on its way, 0 waiting for fed
// (kept in the outbox, sent when it can be), -1 could not be kept.
int fed_post(int a, const msg_t *m, const char *body, uint32_t blen);
// A letter to m->to = handle@node, sealed to that node by fed: as fed_post().
int fed_mail(const msg_t *m, const char *body, uint32_t blen);
bool fed_node_known(const char *name);		// a node in the network's list, by name
// A cancel for a message in federated forum a (its object id): as fed_post().
int fed_cancel(int a, const char *id);
const char *fed_node_name(void);				// this node's name; "" not yet known
bool msg_log_path(int a, char *out);

// ------------------------------------------------------------------
// text (text.c): UTF-8, CP437, dates
// ------------------------------------------------------------------

// The next character of s (advances *s). Malformed bytes read as U+FFFD.
uint32_t utf8_next(const char **s, const char *end);
int utf8_put(uint32_t cp, char *out);          // 1..4 bytes
int utf8_chars(const char *s);                 // characters, not bytes
void utf8_copy(char *dst, size_t cap, const char *src);   // cut at a character boundary
void utf8_pad(char *dst, size_t cap, const char *src, int width);   // exactly `width` characters
// CP437 byte for a character, or 0 if it has none.
uint8_t cp437_from(uint32_t cp);
uint32_t cp437_to(uint8_t b);
// What to show on a 7-bit terminal: ASCII for box drawing and the
// accented letters that have an obvious one, '?' for the rest.
char ascii_from(uint32_t cp);
// "2026-09-27 14:05" (UTC) into out[17]; "never" for 0.
void fmt_time(uint32_t t, char *out);

// ------------------------------------------------------------------
// a node (session.c, out.c)
// ------------------------------------------------------------------

typedef enum {
	N_FREE = 0,
	N_DETECT,          // asked the terminal what it is; waiting
	N_LOGIN_NAME, N_LOGIN_PASS,
	N_NEW_NAME, N_NEW_PASS, N_NEW_PASS2, N_NEW_LOC, N_NEW_CONFIRM,
	N_PAGER,           // showing text a screen at a time
	N_MENU,
	N_PROFILE, N_PROF_PASS_OLD, N_PROF_PASS_NEW, N_PROF_PASS_NEW2, N_PROF_LOC, N_PROF_ROWS,
	N_SYSOP, N_SYS_PICK, N_SYS_USER, N_SYS_LEVEL, N_SYS_PASS, N_SYS_KICK,
	N_AREAS,           // the forum list: which one
	N_READ,            // a message shown: next, reply ...
	N_AREA_MENU,       // one area: read, write ... (the mail too)
	N_POST_TO, N_POST_SUBJ, N_QUOTE_ASK,
	N_EDIT,            // writing a message, a line at a time
	N_ZETTA,           // writing a message full-screen (zetta, docs/zetta.md)
	N_ANYKEY,          // "press a key", then back to the menu
	N_BYE,             // closing once the output has gone
} nstate_t;

// What the pager is showing.
enum { PG_NONE = 0, PG_FILE, PG_USERS, PG_WHO, PG_BULLETINS, PG_MSG };

#define FIELD_MAX 320                  // an editor line: 76 characters of up to 4 bytes

typedef struct {
	nstate_t state;
	uint32_t since_ms;                 // when the state began (timeouts)
	uint32_t last_input_ms;
	uint32_t connected_ms;             // plat_ms() at the call
	uint32_t esc_ms;                   // when a lone ESC arrived
	nstate_t after_key;                // N_ANYKEY: where a key goes
	uint32_t connected_at;             // Unix seconds
	char transport[8];
	char peer[48];
	char offered[33];                  // the user name the client offered

	// the terminal
	bool ansi;                         // cursor movement and SGR understood
	uint8_t charset;                   // CS_UTF8, CS_CP437, CS_ASCII: what is sent
	bool color;
	int rows, cols;
	int detect_step;                   // replies to the probes, in order
	bool detect_sent_col;
	uint8_t in_esc[16];                // an escape sequence arriving
	uint8_t in_esc_n;
	uint32_t in_cp;                    // UTF-8 arriving
	uint8_t in_need;

	// who
	bool logged_in;
	user_t user;
	int tries;
	int empty_handles;                 // Enter alone at the handle prompt: the third ends the call
	char pending_pw[FIELD_MAX];        // the new password, until confirmed

	// the line being typed
	bool field_on;
	bool field_mask;
	int field_max;                     // characters
	char field[FIELD_MAX];

	// the pager
	int pg_kind;
	char pg_path[BBS_PATH_MAX];
	uint32_t pg_off;                   // file: next byte; lists: next index
	int pg_count;                      // lists: how many; bulletins: which file
	uint32_t pg_newer;                 // bulletins: only those newer than this
	nstate_t pg_then;                  // where to go when it ends
	int pg_line;                       // lines shown on this screen

	// messages
	uint32_t lastread[AREA_MAX];       // per area (not mail): the last number read
	bool lastread_dirty;
	int area;                          // the area being read or written
	int msgnum;                        // the message shown
	bool scan;                         // a new-scan: on to the next area at the end
	uint32_t pg_end;                   // PG_MSG: where the body ends in the log
	msg_t post;                        // the message being written
	char *ed;                          // its body (malloc'd while writing)
	void *zed;                         // zetta and what it needs (reader.c; malloc'd while writing full-screen)
	uint32_t ed_len;
	int ed_lines;

	// sysop: the user being edited
	user_t edit;

	// out
	uint8_t out[BBS_OUT_RING];
	uint32_t out_head, out_len;
	bool out_lost;
	bool close_after;
} node_t;

extern node_t *bbs_node[BBS_NODES_MAX];

// msgbase.c, for a node
void lastread_load(node_t *n);
void lastread_save(node_t *n);
int area_new(node_t *n, int a);

// session.c, for the other screens
void set_state(node_t *n, nstate_t s);
void field(node_t *n, nstate_t st, const char *prompt, int max, bool mask, const char *prefill);
void anykey(node_t *n, nstate_t then);
void show_menu(node_t *n);
void item(node_t *n, char key, const char *what);
void pager_start(node_t *n, int kind, nstate_t then);
void pager_run(node_t *n);
int node_index(const node_t *n);
uint32_t upper(uint32_t k);
#define K_ENTER  0x110001
#define K_BS     0x110002
#define K_ESC    0x110003
#define K_UP     0x110004
#define K_DOWN   0x110005
#define K_LEFT   0x110006
#define K_RIGHT  0x110007

// reader.c: forums, mail, the new-scan, writing
void forums_show(node_t *n);
void area_menu(node_t *n, int a);
void newscan_start(node_t *n);
void reader_key(node_t *n, uint32_t k);
void reader_after_body(node_t *n);
bool reader_field(node_t *n);      // a finished field of reader.c's: handled?
bool reader_keyed(node_t *n, uint32_t k);   // a key in one of reader.c's states: handled?
bool editor_wrap(node_t *n, uint32_t k);    // N_EDIT: a character past the line's end
void editor_free(node_t *n);
// N_ZETTA: bytes typed go straight to the editor; the time, when idle.
void zed_input(node_t *n, const uint8_t *d, uint32_t len);
void zed_tick(node_t *n);
int mail_new(node_t *n);

// out.c -- everything written to a caller goes through here.
void out_raw(node_t *n, const void *d, uint32_t len);
// A string literal, its length counted by the compiler rather than by
// hand: a miscounted one sends its terminating NUL to the caller.
#define OUT_LIT(n, lit) out_raw((n), "" lit, (uint32_t)(sizeof(lit) - 1))
void out_text(node_t *n, const char *utf8);    // translated for the caller's charset
void out_textn(node_t *n, const char *utf8, uint32_t len);
void out_mci(node_t *n, const char *s);        // with |codes (docs/bbs.md, "Screens")
void out_mcin(node_t *n, const char *s, uint32_t len);
void out_fmt(node_t *n, const char *fmt, ...); // printf, then out_mci()
void out_nl(node_t *n);
void out_cls(node_t *n);
void out_rev(node_t *n, bool on);
void out_attr_reset(node_t *n);
void out_fg(node_t *n, int color);             // 0-15; nothing when colour is off
void out_rule(node_t *n);                      // a line across the screen
void out_title(node_t *n, const char *title);  // the bar at the top of a screen
uint32_t out_room(const node_t *n);

#endif
