/*
 * Draws irc's window on the build machine, from the REAL repaint() --
 * sw/common/tests/zrender.h explains why a layout is looked at, not
 * just asserted.
 *
 *   make render        writes /tmp/irc.pbm
 *
 * irc.c is included whole, with its main() renamed out of the way.
 * The clock is reported absent, so no MMIO is read: lines have no
 * timestamps here.
 */

#include "zrender.h"
#include "../../../common/zrtc.h"
#define z_rtc_available() false
#define main irc_main
#include "../irc.c"
#undef main

int z_fb_scroll_debug, z_fb_scroll_dbg_armed, z_fb_scroll_align;

int main(int argc, char **argv) {

	const char *out = argc > 1 ? argv[1] : "/tmp/irc";
	char path[256];

	if (!z_render_open(&win, WIN_W, WIN_H)) return 77;
	z_edit_init(&input, input_buf, sizeof(input_buf), "");
	input.focus = true;

	snprintf(server, sizeof(server), "irc.libera.chat");
	snprintf(me, sizeof(me), "zed");
	cstate = C_ONLINE;

	add_line("", "-- Zeitlos irc -- /help for commands", false);
	add_line("", "-- Welcome to the Libera.Chat Internet Relay Chat Network zed", false);
	view_add("#zeitlos");
	view_add("alice");
	view_add("#fpga");
	cur = view_find("#zeitlos");
	add_line("#zeitlos", "-- zed joined #zeitlos", false);
	add_line("#zeitlos", "-- topic for #zeitlos: Zeitlos -- a timeless FPGA computer | https://github.com/machdyne/zeitlos", false);
	add_line("#zeitlos", "<bob> has anyone tried the new gopher support in web yet?", false);
	add_line("#zeitlos", "<carol> yes, and gemini too. The certificate pinning works the way the spec says it should, which is a nice change from clients that just ignore the certificate entirely.", false);
	add_line("#zeitlos", "<bob> zed: does irc run on the Obst?", true);
	add_line("#zeitlos", "* carol waves", false);
	add_line("#zeitlos", "<zed> it needs net and wm; should fit, it is about 180KB", false);
	add_line("#fpga", "<dave> anyone around?", false);
	add_line("alice", "<alice> psst, zed -- got a minute?", true);
	z_edit_set(&input, "sounds good, I will try it tonight");

	repaint();
	snprintf(path, sizeof(path), "%s.pbm", out);
	z_render_write(path, &win, 2);

	return 0;

}
