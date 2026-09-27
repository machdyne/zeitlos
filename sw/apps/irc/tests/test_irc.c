/*
 * Host tests for irc_core.c -- docs/irc_app.md.
 *
 *   make -C sw/apps/irc test
 */

#include <stdio.h>
#include <string.h>

#include "../irc_core.h"

static int checks, fails;
#define CK(c, w) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, w); } } while (0)

static irc_event_t ev;

// Parses and interprets one server line as nick "zed".
static irc_event_t *in(const char *line) {
	static char buf[IRC_LINE_MAX];
	irc_msg_t m;
	snprintf(buf, sizeof(buf), "%s", line);
	memset(&ev, 0, sizeof(ev));
	if (!irc_parse(buf, &m)) { ev.act = (irc_act_t)-1; return &ev; }
	irc_interpret(&m, "zed", &ev);
	return &ev;
}

static irc_input_t inp;
static irc_input_t *typed(const char *line, const char *target) {
	irc_input(line, target, "zed", &inp);
	return &inp;
}

#define SHOWS(line, tgt, txt) do { irc_event_t *e = in(line); \
	checks++; if (e->act != IRC_ACT_SHOW || strcmp(e->target, tgt) || strcmp(e->text, txt)) { \
	fails++; printf("FAIL %d: %s\n  got act=%d target=[%s] text=[%s]\n", __LINE__, line, \
	(int)e->act, e->target, e->text); } } while (0)

int main(void) {

	char buf[IRC_LINE_MAX];
	irc_msg_t m;

	// -- parsing --
	strcpy(buf, ":alice!a@host.example PRIVMSG #zeitlos :hello there: you");
	CK(irc_parse(buf, &m), "parses");
	CK(!strcmp(m.prefix, "alice!a@host.example") && !strcmp(m.nick, "alice"), "prefix and nick");
	CK(!strcmp(m.command, "PRIVMSG") && m.nparams == 2, "command, two params");
	CK(!strcmp(m.params[0], "#zeitlos") && !strcmp(m.params[1], "hello there: you"),
		"trailing keeps spaces and colons");
	strcpy(buf, "@time=2026-09-26T12:00:00Z :irc.example.net 001 zed :Welcome");
	CK(irc_parse(buf, &m) && !strcmp(m.command, "001") && m.nick[0] == 0,
		"message tags skipped; a server is not a nick");
	strcpy(buf, "PING :abc");
	CK(irc_parse(buf, &m) && !strcmp(m.prefix, "") && !strcmp(m.params[0], "abc"), "no prefix");
	strcpy(buf, "   ");
	CK(!irc_parse(buf, &m), "blank line refused");
	strcpy(buf, ":x MODE #c +o bob");
	CK(irc_parse(buf, &m) && m.nparams == 3 && !strcmp(m.params[2], "bob"), "params without trailing");

	// -- events --
	CK(in("PING :irc.example.net")->act == IRC_ACT_PONG &&
		!strcmp(ev.wire, "PONG :irc.example.net\r\n"), "PING answered");
	SHOWS(":alice!a@h PRIVMSG #zeitlos :hi all", "#zeitlos", "<alice> hi all");
	CK(!ev.mention, "no mention");
	SHOWS(":alice!a@h PRIVMSG #zeitlos :zed: look", "#zeitlos", "<alice> zed: look");
	CK(ev.mention, "mention at the start");
	in(":alice!a@h PRIVMSG #zeitlos :ask Zed about it");
	CK(ev.mention, "mention folded");
	in(":alice!a@h PRIVMSG #zeitlos :zeditor is great");
	CK(!ev.mention, "a longer word is not a mention");
	SHOWS(":alice!a@h PRIVMSG zed :psst", "alice", "<alice> psst");
	CK(ev.mention == false, "a query is not flagged by nick");
	SHOWS(":alice!a@h NOTICE zed :note", "alice", "-alice- note");
	SHOWS(":irc.example.net NOTICE * :*** Looking up your hostname", "", "-irc.example.net- *** Looking up your hostname");
	SHOWS(":alice!a@h PRIVMSG #zeitlos :\001ACTION waves at zed\001", "#zeitlos", "* alice waves at zed");
	CK(ev.mention, "mention in an action");
	CK(in(":alice!a@h PRIVMSG zed :\001VERSION\001")->act == IRC_ACT_REPLY &&
		!strcmp(ev.wire, "NOTICE alice :\001VERSION Zeitlos irc\001\r\n"), "CTCP VERSION answered");
	CK(in(":alice!a@h PRIVMSG zed :\001PING 123\001")->act == IRC_ACT_REPLY &&
		!strcmp(ev.wire, "NOTICE alice :\001PING 123\001\r\n"), "CTCP PING echoed");
	CK(in(":alice!a@h PRIVMSG zed :\001FINGER\001")->act == IRC_ACT_SHOW && !ev.wire[0],
		"other CTCP shown, not answered");
	SHOWS(":alice!a@h PRIVMSG #z :\00304,01red\003 \002bold\002 \037u\037", "#z", "<alice> red bold u");

	SHOWS(":zed!z@h JOIN #zeitlos", "#zeitlos", "-- zed joined #zeitlos");
	CK(ev.joined, "our own join noted");
	SHOWS(":bob!b@h JOIN :#zeitlos", "#zeitlos", "-- bob joined #zeitlos");
	CK(!ev.joined, "someone else's join is not ours");
	SHOWS(":zed!z@h PART #zeitlos :bye", "#zeitlos", "-- zed left #zeitlos (bye)");
	CK(ev.parted, "our own part noted");
	SHOWS(":op!o@h KICK #zeitlos zed :spam", "#zeitlos", "-- zed was kicked from #zeitlos by op");
	CK(ev.parted, "a kick of us is a part");
	SHOWS(":bob!b@h QUIT :Ping timeout", "", "-- bob quit (Ping timeout)");
	SHOWS(":zed!z@h NICK :zed2", "", "-- zed is now zed2");
	CK(!strcmp(ev.new_nick, "zed2"), "our own nick change noted");
	in(":bob!b@h NICK bobby");
	CK(!ev.new_nick[0], "someone else's nick change is not ours");
	SHOWS(":bob!b@h TOPIC #z :new topic", "#z", "-- bob set the topic: new topic");
	SHOWS(":op!o@h MODE #z +o bob", "#z", "-- op sets mode +o bob on #z");

	CK(in(":irc.example.net 001 zed_ :Welcome to the network")->act == IRC_ACT_WELCOME &&
		!strcmp(ev.new_nick, "zed_"), "001: registered, with the server's name for us");
	CK(in(":irc.example.net 433 * zed :Nickname is already in use")->act == IRC_ACT_NICK_TAKEN,
		"433 nick taken");
	SHOWS(":irc.example.net 332 zed #z :the topic", "#z", "-- topic for #z: the topic");
	SHOWS(":irc.example.net 353 zed = #z :@op bob zed", "#z", "-- in #z: @op bob zed");
	CK(in(":irc.example.net 366 zed #z :End of /NAMES list.")->act == IRC_ACT_NONE, "366 hidden");
	SHOWS(":irc.example.net 372 zed :- message of the day", "", "-- - message of the day");
	SHOWS(":irc.example.net 401 zed nobody :No such nick", "", "-- nobody No such nick");
	SHOWS("ERROR :Closing Link", "", "-- error: Closing Link");

	// -- input --
	CK(typed("hello", "#z")->kind == IRC_IN_SEND && !strcmp(inp.wire, "PRIVMSG #z :hello\r\n") &&
		!strcmp(inp.text, "<zed> hello"), "plain text to the target, echoed");
	CK(typed("hello", "")->kind == IRC_IN_ERROR, "plain text with no target refused");
	CK(typed("//slash", "#z")->kind == IRC_IN_SEND && !strcmp(inp.wire, "PRIVMSG #z :/slash\r\n"),
		"// sends a literal slash");
	CK(typed("/join zeitlos", "")->kind == IRC_IN_SEND && !strcmp(inp.wire, "JOIN #zeitlos\r\n"),
		"/join adds the #");
	CK(typed("/JOIN #a", "")->kind == IRC_IN_SEND && !strcmp(inp.wire, "JOIN #a\r\n"), "commands any case");
	CK(typed("/part", "#z")->kind == IRC_IN_SEND && !strcmp(inp.wire, "PART #z\r\n"), "/part the current channel");
	CK(typed("/part #y later", "#z")->kind == IRC_IN_SEND && !strcmp(inp.wire, "PART #y :later\r\n"),
		"/part another, with a reason");
	CK(typed("/part", "alice")->kind == IRC_IN_ERROR, "cannot /part a query");
	CK(typed("/msg alice hi there", "#z")->kind == IRC_IN_SEND &&
		!strcmp(inp.wire, "PRIVMSG alice :hi there\r\n") && !strcmp(inp.arg1, "alice"), "/msg");
	CK(typed("/query alice", "#z")->kind == IRC_IN_TARGET && !strcmp(inp.arg1, "alice"),
		"/query switches");
	CK(typed("/me waves", "#z")->kind == IRC_IN_SEND &&
		!strcmp(inp.wire, "PRIVMSG #z :\001ACTION waves\001\r\n") && !strcmp(inp.text, "* zed waves"), "/me");
	CK(typed("/nick zed2", "")->kind == IRC_IN_SEND && !strcmp(inp.wire, "NICK zed2\r\n"), "/nick");
	CK(typed("/topic", "#z")->kind == IRC_IN_SEND && !strcmp(inp.wire, "TOPIC #z\r\n"), "/topic asks");
	CK(typed("/quit bye", "")->kind == IRC_IN_QUIT && !strcmp(inp.wire, "QUIT :bye\r\n"), "/quit");
	CK(typed("/connect irc.example.net 7000", "")->kind == IRC_IN_CONNECT &&
		!strcmp(inp.arg1, "irc.example.net") && inp.port == 7000, "/connect host port");
	CK(typed("/connect", "")->kind == IRC_IN_CONNECT && !inp.arg1[0] && inp.port == 6667,
		"/connect alone: the configured server");
	CK(typed("/connect h 99999", "")->kind == IRC_IN_ERROR, "bad port refused");
	CK(typed("/raw PRIVMSG x :y", "")->kind == IRC_IN_SEND && !strcmp(inp.wire, "PRIVMSG x :y\r\n"), "/raw");
	CK(typed("/frobnicate", "")->kind == IRC_IN_ERROR && strstr(inp.text, "/frobnicate"), "unknown command named");
	CK(typed("hi\r\nQUIT", "#z")->kind == IRC_IN_ERROR, "CR/LF injection refused");
	CK(typed("   ", "#z")->kind == IRC_IN_NONE, "blank does nothing");

	// -- folding --
	CK(!irc_casecmp("Zed[away]", "zed{AWAY}"), "RFC 2812 case folding");
	CK(irc_casecmp("zed", "zee") != 0, "different nicks differ");

	printf("test_irc: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
