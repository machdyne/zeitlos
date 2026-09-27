#ifndef IRC_CORE_H
#define IRC_CORE_H

/*
 * Zeitlos -- sw/apps/irc
 *
 * The IRC protocol (RFC 1459/2812), as the client needs it: parsing a
 * line from the server, turning it into a line for the screen, and
 * turning what the user types into what goes on the wire. No I/O and
 * no Zeitlos dependencies: tests/test_irc.c runs all of it on the
 * build machine, and irc.c is the window and the socket around it.
 * docs/irc_app.md.
 */

#include <stdint.h>
#include <stdbool.h>

#define IRC_LINE_MAX     512		// RFC 2812: including CRLF
#define IRC_NICK_MAX     32
#define IRC_TARGET_MAX   64
#define IRC_PARAMS_MAX   15
#define IRC_SHOW_MAX     600		// one line for the screen

// A parsed server line. Pointers into the caller's buffer, which
// irc_parse() modifies in place.
typedef struct {
	const char	*prefix;		// "nick!user@host" or a server, or ""
	char		nick[IRC_NICK_MAX];	// the prefix up to '!', or ""
	const char	*command;		// "PRIVMSG", "001", ...
	const char	*params[IRC_PARAMS_MAX];
	int			nparams;		// the trailing parameter is the last
} irc_msg_t;

// Splits `line` (no CRLF) in place. False for an empty line or one
// with no command.
bool irc_parse(char *line, irc_msg_t *m);

// What a parsed line means to the client.
typedef enum {
	IRC_ACT_NONE = 0,
	IRC_ACT_SHOW,			// show `text` (in `target`, "" for the server window)
	IRC_ACT_PONG,			// send `wire`: the answer to a PING
	IRC_ACT_WELCOME,		// 001: registered -- join the configured channels
	IRC_ACT_NICK_TAKEN,		// 433 during registration: try another nick
	IRC_ACT_REPLY,			// send `wire` (a CTCP answer) and show `text`
} irc_act_t;

typedef struct {
	irc_act_t	act;
	char		text[IRC_SHOW_MAX];
	char		target[IRC_TARGET_MAX];	// channel or nick it belongs to
	char		wire[IRC_LINE_MAX];		// to send, with CRLF
	bool		mention;		// someone said our nick
	bool		joined;			// WE joined `target`
	bool		parted;			// WE left `target`
	char		new_nick[IRC_NICK_MAX];	// WE are now called this
} irc_event_t;

// Interprets one parsed line for a client whose nick is `me`.
void irc_interpret(const irc_msg_t *m, const char *me, irc_event_t *ev);

// What the user typed, turned into a wire line.
typedef enum {
	IRC_IN_NONE = 0,		// nothing to do (an empty line)
	IRC_IN_SEND,			// send `wire`; echo `text` locally if set
	IRC_IN_CONNECT,			// /connect [host [port]] -- `arg1`, `port`
	IRC_IN_QUIT,			// send `wire` and close
	IRC_IN_TARGET,			// switch the current target to `arg1`
	IRC_IN_HELP,			// show the command list
	IRC_IN_ERROR,			// show `text` (a usage message)
} irc_in_t;

typedef struct {
	irc_in_t	kind;
	char		wire[IRC_LINE_MAX];
	char		text[IRC_SHOW_MAX];
	char		arg1[IRC_TARGET_MAX + 192];
	uint16_t	port;
} irc_input_t;

// `line` as typed; `target` the current channel or query ("" for none);
// `me` our nick. Plain text goes to the target as a PRIVMSG.
void irc_input(const char *line, const char *target, const char *me,
	irc_input_t *out);

// Removes mIRC formatting from `s` in place: bold (^B), colour (^C
// with its digits), reverse, italics, underline, strikethrough,
// monospace and reset. A 1bpp screen shows none of them, and the
// codes are otherwise drawn as garbage.
void irc_strip(char *s);

// Is `nick` a word in `text` (case-insensitive, IRC's letters)?
bool irc_mentions(const char *text, const char *nick);

// Case-insensitive compare the way IRC folds names ({}|^ are the
// lower case of []\~ -- RFC 2812 section 2.2).
int irc_casecmp(const char *a, const char *b);

#endif
