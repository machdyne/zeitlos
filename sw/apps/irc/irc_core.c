/*
 * Zeitlos -- sw/apps/irc
 *
 * See irc_core.h and docs/irc_app.md.
 */

#include <string.h>
#include <stdio.h>

#include "irc_core.h"

// -- small helpers --

static char fold(char c) {
	if (c >= 'A' && c <= 'Z') return (char)(c + 32);
	switch (c) {			// RFC 2812 2.2: []\~ are the upper case of {}|^
	case '[': return '{';
	case ']': return '}';
	case '\\': return '|';
	case '~': return '^';
	default: return c;
	}
}

int irc_casecmp(const char *a, const char *b) {
	for (;; a++, b++) {
		char x = fold(*a), y = fold(*b);
		if (x != y) return (unsigned char)x - (unsigned char)y;
		if (!x) return 0;
	}
}

static bool is_channel(const char *s) {
	return s[0] == '#' || s[0] == '&' || s[0] == '+' || s[0] == '!';
}

static void copy(char *dst, uint32_t cap, const char *src) {
	uint32_t n = 0;
	if (!cap) return;
	while (src && src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
	dst[n] = 0;
}

static bool nick_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || (c && strchr("[]\\`_^{|}-", c) != NULL);
	// (c && ...): strchr() finds the terminator when asked for '\0',
	// which made the end of a line count as part of a nick -- and a
	// mention as the last word of a message go unnoticed.
}

bool irc_mentions(const char *text, const char *nick) {
	size_t n = strlen(nick);
	if (!n) return false;
	for (const char *p = text; *p; p++) {
		if (p != text && nick_char(p[-1])) continue;
		size_t i;
		for (i = 0; i < n && p[i] && fold(p[i]) == fold(nick[i]); i++) {}
		if (i == n && !nick_char(p[n])) return true;
	}
	return false;
}

void irc_strip(char *s) {
	char *w = s;
	for (char *r = s; *r; r++) {
		unsigned char c = (unsigned char)*r;
		if (c == 0x03) {			// colour: ^C[fg[,bg]], up to two digits each
			int d = 0;
			while (d < 2 && r[1] >= '0' && r[1] <= '9') { r++; d++; }
			if (d && r[1] == ',' && r[2] >= '0' && r[2] <= '9') {
				r += 2;
				if (r[1] >= '0' && r[1] <= '9') r++;
			}
			continue;
		}
		if (c == 0x02 || c == 0x0F || c == 0x16 || c == 0x1D ||
		    c == 0x1E || c == 0x1F || c == 0x11) continue;
		*w++ = (char)c;
	}
	*w = 0;
}

// -- parsing --

bool irc_parse(char *line, irc_msg_t *m) {

	char *p = line;

	memset(m, 0, sizeof(*m));
	m->prefix = "";

	while (*p == ' ') p++;

	// IRCv3 message tags (@a=b;c ...) come first; nothing here reads them.
	if (*p == '@') {
		while (*p && *p != ' ') p++;
		while (*p == ' ') p++;
	}

	if (*p == ':') {
		char *e;
		m->prefix = ++p;
		while (*p && *p != ' ') p++;
		if (*p) *p++ = 0;
		e = strchr(m->prefix, '!');
		{
			uint32_t n = e ? (uint32_t)(e - m->prefix) : (uint32_t)strlen(m->prefix);
			// a server name has a '.' and no '!'; it is not a nick
			if (!e && strchr(m->prefix, '.')) n = 0;
			if (n >= IRC_NICK_MAX) n = IRC_NICK_MAX - 1;
			memcpy(m->nick, m->prefix, n);
			m->nick[n] = 0;
		}
		while (*p == ' ') p++;
	}

	if (!*p) return false;
	m->command = p;
	while (*p && *p != ' ') p++;
	if (*p) *p++ = 0;

	while (*p && m->nparams < IRC_PARAMS_MAX) {
		while (*p == ' ') p++;
		if (!*p) break;
		if (*p == ':' || m->nparams == IRC_PARAMS_MAX - 1) {
			m->params[m->nparams++] = (*p == ':') ? p + 1 : p;
			break;
		}
		m->params[m->nparams++] = p;
		while (*p && *p != ' ') p++;
		if (*p) *p++ = 0;
	}

	return true;

}

// -- interpreting --

static const char *param(const irc_msg_t *m, int i) {
	return (i < m->nparams && m->params[i]) ? m->params[i] : "";
}

// The last parameter: the message text of most commands.
static const char *trailing(const irc_msg_t *m) {
	return m->nparams ? m->params[m->nparams - 1] : "";
}

static void show(irc_event_t *ev, const char *target, const char *fmt,
	const char *a, const char *b, const char *c) {
	ev->act = IRC_ACT_SHOW;
	copy(ev->target, sizeof(ev->target), target);
	snprintf(ev->text, sizeof(ev->text), fmt, a, b, c);
	irc_strip(ev->text);
}

void irc_interpret(const irc_msg_t *m, const char *me, irc_event_t *ev) {

	const char *cmd = m->command;
	const char *who = m->nick[0] ? m->nick : m->prefix;
	bool from_me = me[0] && m->nick[0] && !irc_casecmp(m->nick, me);

	memset(ev, 0, sizeof(*ev));

	if (!strcmp(cmd, "PING")) {
		ev->act = IRC_ACT_PONG;
		snprintf(ev->wire, sizeof(ev->wire), "PONG :%s\r\n", trailing(m));
		return;
	}

	if (!strcmp(cmd, "PRIVMSG") || !strcmp(cmd, "NOTICE")) {

		const char *to = param(m, 0);
		const char *msg = trailing(m);
		bool notice = (cmd[0] == 'N');
		// A message to a channel belongs to the channel; one to us, to
		// its sender (a query) -- or, from a server, to the server
		// window.
		const char *where = is_channel(to) ? to
			: (m->nick[0] && !from_me) ? m->nick : (from_me ? to : "");
		size_t ml = strlen(msg);

		// CTCP: \001COMMAND args\001
		if (msg[0] == 1) {
			char ctcp[IRC_SHOW_MAX];
			copy(ctcp, sizeof(ctcp), msg + 1);
			if (ml > 1 && ctcp[strlen(ctcp) - 1] == 1) ctcp[strlen(ctcp) - 1] = 0;
			if (!strncmp(ctcp, "ACTION ", 7) || !strcmp(ctcp, "ACTION")) {
				const char *act = ctcp[6] ? ctcp + 7 : "";
				show(ev, where, "* %s %s", who, act, "");
				ev->mention = !from_me && irc_mentions(act, me);
				return;
			}
			if (notice) {
				show(ev, "", "-- CTCP reply from %s: %s", who, ctcp, "");
				return;
			}
			// Answer VERSION and PING, which clients use to see what
			// they are talking to and how far away it is. Nothing
			// else is answered: CTCP is a well-known way to make a
			// client flood itself off a network.
			if (!strcmp(ctcp, "VERSION")) {
				ev->act = IRC_ACT_REPLY;
				snprintf(ev->wire, sizeof(ev->wire),
					"NOTICE %.31s :\001VERSION Zeitlos irc\001\r\n", who);
			} else if (!strncmp(ctcp, "PING", 4)) {
				ev->act = IRC_ACT_REPLY;
				// Bounded so the reply fits one line whole, CRLF and all.
				snprintf(ev->wire, sizeof(ev->wire),
					"NOTICE %.31s :\001%.400s\001\r\n", who, ctcp);
			} else {
				ev->act = IRC_ACT_SHOW;
			}
			copy(ev->target, sizeof(ev->target), "");
			snprintf(ev->text, sizeof(ev->text), "-- CTCP %.500s from %.31s", ctcp, who);
			irc_strip(ev->text);
			return;
		}

		if (notice) show(ev, where, "-%s- %s", who, msg, "");
		else show(ev, where, "<%s> %s", who, msg, "");
		ev->mention = !from_me && !notice && irc_mentions(msg, me);
		return;

	}

	if (!strcmp(cmd, "JOIN")) {
		const char *ch = param(m, 0);
		if (from_me) ev->joined = true;
		show(ev, ch, "-- %s joined %s", who, ch, "");
		return;
	}

	if (!strcmp(cmd, "PART")) {
		const char *ch = param(m, 0);
		if (from_me) ev->parted = true;
		if (m->nparams > 1) show(ev, ch, "-- %s left %s (%s)", who, ch, trailing(m));
		else show(ev, ch, "-- %s left %s", who, ch, "");
		return;
	}

	if (!strcmp(cmd, "KICK")) {
		const char *ch = param(m, 0), *victim = param(m, 1);
		if (me[0] && !irc_casecmp(victim, me)) ev->parted = true;
		show(ev, ch, "-- %s was kicked from %s by %s", victim, ch, who);
		copy(ev->target, sizeof(ev->target), ch);
		return;
	}

	if (!strcmp(cmd, "QUIT")) {
		show(ev, "", "-- %s quit (%s)", who, trailing(m), "");
		return;
	}

	if (!strcmp(cmd, "NICK")) {
		if (from_me) copy(ev->new_nick, sizeof(ev->new_nick), trailing(m));
		show(ev, "", "-- %s is now %s", who, trailing(m), "");
		return;
	}

	if (!strcmp(cmd, "TOPIC")) {
		show(ev, param(m, 0), "-- %s set the topic: %s", who, trailing(m), "");
		return;
	}

	if (!strcmp(cmd, "MODE")) {
		char modes[IRC_SHOW_MAX];
		int n = 0;
		modes[0] = 0;
		for (int i = 1; i < m->nparams && n < (int)sizeof(modes) - 2; i++)
			n += snprintf(modes + n, sizeof(modes) - (size_t)n, "%s%s",
				i > 1 ? " " : "", m->params[i]);
		show(ev, is_channel(param(m, 0)) ? param(m, 0) : "",
			"-- %s sets mode %s on %s", who, modes, param(m, 0));
		return;
	}

	if (!strcmp(cmd, "ERROR")) {
		show(ev, "", "-- error: %s", trailing(m), "", "");
		return;
	}

	// -- numerics: three digits, and the first parameter is us --
	if (cmd[0] >= '0' && cmd[0] <= '9' && cmd[1] && cmd[2] && !cmd[3]) {

		int num = (cmd[0] - '0') * 100 + (cmd[1] - '0') * 10 + (cmd[2] - '0');

		switch (num) {
		case 1:
			show(ev, "", "-- %s", trailing(m), "", "");
			ev->act = IRC_ACT_WELCOME;
			// The server's name for us, which may differ from the one
			// asked for (truncated, or a collision resolved by it).
			copy(ev->new_nick, sizeof(ev->new_nick), param(m, 0));
			return;
		case 433:		// ERR_NICKNAMEINUSE
			show(ev, "", "-- the nick %s is taken", param(m, 1), "", "");
			ev->act = IRC_ACT_NICK_TAKEN;
			return;
		case 332:		// RPL_TOPIC
			show(ev, param(m, 1), "-- topic for %s: %s", param(m, 1), trailing(m), "");
			return;
		case 333:		// RPL_TOPICWHOTIME: noise
		case 366:		// RPL_ENDOFNAMES
			return;
		case 353:		// RPL_NAMREPLY: = #chan :names
			show(ev, param(m, 2), "-- in %s: %s", param(m, 2), trailing(m), "");
			return;
		default: {
			// Everything else in the server window: its parameters
			// after our own nick, which is what a person wants to read
			// (the MOTD, LUSERS, WHOIS, errors).
			char rest[IRC_SHOW_MAX];
			int n = 0;
			rest[0] = 0;
			for (int i = 1; i < m->nparams && n < (int)sizeof(rest) - 2; i++)
				n += snprintf(rest + n, sizeof(rest) - (size_t)n, "%s%s",
					i > 1 ? " " : "", m->params[i]);
			show(ev, "", "-- %s", rest, "", "");
			return;
		}
		}

	}

	// Anything else: shown as it came, in the server window.
	{
		char rest[IRC_SHOW_MAX];
		int n = 0;
		rest[0] = 0;
		for (int i = 0; i < m->nparams && n < (int)sizeof(rest) - 2; i++)
			n += snprintf(rest + n, sizeof(rest) - (size_t)n, " %s", m->params[i]);
		show(ev, "", "-- %s%s", cmd, rest, "");
	}

}

// -- input --

// The first word of `s` into `w` (up to cap), returning the rest.
static const char *word(const char *s, char *w, uint32_t cap) {
	uint32_t n = 0;
	while (*s == ' ') s++;
	while (*s && *s != ' ') { if (n + 1 < cap) w[n++] = *s; s++; }
	w[n] = 0;
	while (*s == ' ') s++;
	return s;
}

void irc_input(const char *line, const char *target, const char *me,
	irc_input_t *o) {

	char cmd[16], a[IRC_TARGET_MAX + 192];
	const char *rest;

	memset(o, 0, sizeof(*o));

	while (*line == ' ') line++;
	if (!*line) return;

	// Anything with a CR or LF in it would end the line early and send
	// the rest as a second command of the user's choosing -- refuse
	// rather than trust it came from the keyboard.
	if (strpbrk(line, "\r\n")) {
		o->kind = IRC_IN_ERROR;
		copy(o->text, sizeof(o->text), "-- a line cannot contain a line break");
		return;
	}

	// "//text" sends "/text" as a message.
	if (line[0] != '/' || line[1] == '/') {
		if (line[0] == '/') line++;
		if (!target[0]) {
			o->kind = IRC_IN_ERROR;
			copy(o->text, sizeof(o->text),
				"-- no channel: /join #channel first, or /msg nick text");
			return;
		}
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "PRIVMSG %s :%s\r\n", target, line);
		snprintf(o->text, sizeof(o->text), "<%s> %s", me, line);
		return;
	}

	rest = word(line + 1, cmd, sizeof(cmd));
	for (char *p = cmd; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;

	if (!strcmp(cmd, "join") || !strcmp(cmd, "j")) {
		rest = word(rest, a, sizeof(a));
		if (!a[0]) goto usage;
		o->kind = IRC_IN_SEND;
		// "/join zeitlos" means "#zeitlos"
		snprintf(o->wire, sizeof(o->wire), "JOIN %s%s\r\n",
			is_channel(a) ? "" : "#", a);
		return;
	}

	if (!strcmp(cmd, "part") || !strcmp(cmd, "leave")) {
		const char *ch = target;
		if (*rest && is_channel(rest)) { rest = word(rest, a, sizeof(a)); ch = a; }
		if (!ch[0] || !is_channel(ch)) goto usage;
		o->kind = IRC_IN_SEND;
		if (*rest) snprintf(o->wire, sizeof(o->wire), "PART %s :%s\r\n", ch, rest);
		else snprintf(o->wire, sizeof(o->wire), "PART %s\r\n", ch);
		return;
	}

	if (!strcmp(cmd, "msg") || !strcmp(cmd, "query")) {
		rest = word(rest, a, sizeof(a));
		if (!a[0]) goto usage;
		if (!*rest) {				// "/query nick": just switch to it
			o->kind = IRC_IN_TARGET;
			copy(o->arg1, sizeof(o->arg1), a);
			return;
		}
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "PRIVMSG %s :%s\r\n", a, rest);
		snprintf(o->text, sizeof(o->text), "-> <%s> %s", a, rest);
		copy(o->arg1, sizeof(o->arg1), a);
		return;
	}

	if (!strcmp(cmd, "me")) {
		if (!target[0] || !*rest) goto usage;
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "PRIVMSG %s :\001ACTION %s\001\r\n",
			target, rest);
		snprintf(o->text, sizeof(o->text), "* %s %s", me, rest);
		return;
	}

	if (!strcmp(cmd, "notice")) {
		rest = word(rest, a, sizeof(a));
		if (!a[0] || !*rest) goto usage;
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "NOTICE %s :%s\r\n", a, rest);
		snprintf(o->text, sizeof(o->text), "-> -%s- %s", a, rest);
		return;
	}

	if (!strcmp(cmd, "nick")) {
		rest = word(rest, a, sizeof(a));
		if (!a[0]) goto usage;
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "NICK %s\r\n", a);
		return;
	}

	if (!strcmp(cmd, "topic")) {
		if (!target[0] || !is_channel(target)) goto usage;
		o->kind = IRC_IN_SEND;
		if (*rest) snprintf(o->wire, sizeof(o->wire), "TOPIC %s :%s\r\n", target, rest);
		else snprintf(o->wire, sizeof(o->wire), "TOPIC %s\r\n", target);
		return;
	}

	if (!strcmp(cmd, "names")) {
		const char *ch = *rest ? rest : target;
		if (!ch[0]) goto usage;
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "NAMES %s\r\n", ch);
		return;
	}

	if (!strcmp(cmd, "whois")) {
		rest = word(rest, a, sizeof(a));
		if (!a[0]) goto usage;
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "WHOIS %s\r\n", a);
		return;
	}

	if (!strcmp(cmd, "raw") || !strcmp(cmd, "quote")) {
		if (!*rest) goto usage;
		o->kind = IRC_IN_SEND;
		snprintf(o->wire, sizeof(o->wire), "%s\r\n", rest);
		snprintf(o->text, sizeof(o->text), "-> %s", rest);
		return;
	}

	if (!strcmp(cmd, "quit") || !strcmp(cmd, "disconnect")) {
		o->kind = IRC_IN_QUIT;
		snprintf(o->wire, sizeof(o->wire), "QUIT :%s\r\n",
			*rest ? rest : "Zeitlos");
		return;
	}

	if (!strcmp(cmd, "connect") || !strcmp(cmd, "server")) {
		int port = 6667;
		rest = word(rest, o->arg1, sizeof(o->arg1));
		if (*rest) {
			port = 0;
			for (const char *p = rest; *p >= '0' && *p <= '9'; p++)
				port = port * 10 + (*p - '0');
			if (port <= 0 || port > 65535) goto usage;
		}
		o->kind = IRC_IN_CONNECT;
		o->port = (uint16_t)port;
		return;
	}

	if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
		o->kind = IRC_IN_HELP;
		return;
	}

usage:
	o->kind = IRC_IN_ERROR;
	if (!strcmp(cmd, "join") || !strcmp(cmd, "j"))
		copy(o->text, sizeof(o->text), "-- usage: /join #channel");
	else if (!strcmp(cmd, "part") || !strcmp(cmd, "leave"))
		copy(o->text, sizeof(o->text), "-- usage: /part [#channel] [reason]");
	else if (!strcmp(cmd, "msg") || !strcmp(cmd, "query"))
		copy(o->text, sizeof(o->text), "-- usage: /msg nick [text]");
	else if (!strcmp(cmd, "me"))
		copy(o->text, sizeof(o->text), "-- usage: /me action (in a channel or query)");
	else if (!strcmp(cmd, "connect") || !strcmp(cmd, "server"))
		copy(o->text, sizeof(o->text), "-- usage: /connect [host [port]]");
	else
		snprintf(o->text, sizeof(o->text),
			"-- unknown command /%s (or missing argument) -- /help lists them", cmd);

}
