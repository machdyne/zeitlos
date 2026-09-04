/*
 * mmod -- read, write and verify an SPI memory module in an MMOD
 * socket (github.com/machdyne/mmod).
 *
 *   > run wm
 *   > run mmod
 *
 * Layout and drawing live in panel.c, which this includes. The split
 * exists so tests/render.c can compile the panel WITHOUT the event
 * loop and draw it on the build machine -- see that file. Anything
 * that positions or paints something belongs there; anything that
 * talks to a device or a message queue belongs here.
 *
 * -- What works today --
 *
 * DETECT, and the controls that describe the device. The transfer
 * operations are not built: pressing READ/WRITE/VERIFY/ERASE says so
 * rather than doing something approximate. They need the z_mmod_*
 * device layer (NOR/FRAM/EEPROM profiles, 2/3/4-byte addressing, page
 * program, sector erase, WIP polling) and the chunked cancellable
 * state machine that goes with it, neither of which exists yet.
 *
 * That is a deliberate order. Identify proves the bus, and writing to
 * flash across a bus that has not been proven is how a module is
 * lost -- see check_ss() below on what a stuck chip select does.
 */

#include "panel.c"

#include "../../common/zmmod.h"
#include "../../common/zfsapp.h"
#include "../../common/zdialog.h"

#define MMOD_KHZ 400

static z_mmod_t dev;
static uint32_t port;

// -- device ------------------------------------------------------
//
// Everything below the panel is sw/common/zmmod.c, which is tested
// against a simulated flash in sw/common/tests/test_mmod.c -- opcodes,
// address widths, page splitting, WREN, WIP and the refusals. This
// file only moves values between that library and the panel.

static void profile_to_panel(void) {

	dev_detected = dev.detected;
	ss_ok = dev.ss_ok;
	dev_id[0] = dev.id[0]; dev_id[1] = dev.id[1]; dev_id[2] = dev.id[2];
	dev_size = dev.size;
	dev_page = dev.page;
	dev_erase = dev.sector;

	switch (dev.cls) {
	case Z_MMOD_NOR:    dev_class = C_NOR; break;
	case Z_MMOD_FRAM:   dev_class = C_FRAM; break;
	case Z_MMOD_EEPROM: dev_class = C_EEPROM; break;
	default:            dev_class = C_UNKNOWN; break;
	}

	addr_idx = dev.addr_bytes == 2 ? 0 : (dev.addr_bytes == 4 ? 2 : 1);

	if (range_len == 0 || (dev_size && range_len > dev_size))
		range_len = dev_size;

}

// The panel is authoritative once a human has touched it: the profile
// often cannot be detected (see zmmod.h) and correcting it by hand is
// the intended workflow, not a fallback.
static void panel_to_profile(void) {
	dev.addr_bytes = addr_widths[addr_idx];
	dev.size = dev_size;
	switch (dev_class) {
	case C_NOR:
		dev.cls = Z_MMOD_NOR;
		dev.needs_erase = true; dev.has_wip = true;
		if (!dev.page) dev.page = 256;
		if (!dev.sector) dev.sector = 4096;
		break;
	case C_FRAM:
		dev.cls = Z_MMOD_FRAM;
		dev.needs_erase = false; dev.has_wip = false;
		dev.page = 0; dev.sector = 0;
		break;
	case C_EEPROM:
		dev.cls = Z_MMOD_EEPROM;
		dev.needs_erase = false; dev.has_wip = true;
		if (!dev.page) dev.page = 256;
		dev.sector = 0;
		break;
	default:
		dev.cls = Z_MMOD_UNKNOWN;
		break;
	}
	dev_page = dev.page;
	dev_erase = dev.sector;
}

static void do_detect(void) {

	z_mmod_rv rv;

	if (port >= z_gpio_port_count()) {
		snprintf(status, sizeof(status), "No GPIO port %lu in this "
			"bitstream.", (unsigned long)port);
		snprintf(detail, sizeof(detail), "See docs/gpio.md.");
		return;
	}

	z_mmod_init(&dev, port, MMOD_KHZ);

	rv = z_mmod_detect(&dev);

	profile_to_panel();

	if (rv == Z_MMOD_NODEV) {

		snprintf(status, sizeof(status), "No device responded (%02x %02x "
			"%02x). Port reads %02x with the bus idle.",
			dev.id[0], dev.id[1], dev.id[2], z_gpio_in_get(port));

		// 00 and ff point at different halves of the board.
		if (dev.id[0] == 0xff)
			snprintf(detail, sizeof(detail), "MISO stayed high -- empty "
				"socket, no power, or SS/SCK/MOSI not reaching it.");
		else
			snprintf(detail, sizeof(detail), "MISO stayed low -- something "
				"is holding it down. Check pin 3 is on bit 2.");

		return;

	}

	z_mmod_ss_ok(&dev);
	ss_ok = dev.ss_ok;

	if (!ss_ok) {
		snprintf(status, sizeof(status), "SS IS NOT WORKING -- the device "
			"answered with SS deasserted.");
		snprintf(detail, sizeof(detail), "It is permanently selected. "
			"WRITE and ERASE are locked out.");
		return;
	}

	snprintf(status, sizeof(status), "Found %02x %02x %02x (%s).",
		dev.id[0], dev.id[1], dev.id[2], z_mmod_vendor(dev.id[0]));

	if (dev.cls == Z_MMOD_NOR)
		snprintf(detail, sizeof(detail), "NOR flash. Set CLASS by hand for "
			"FRAM or EEPROM -- many do not answer 9F at all.");
	else if (dev.cls == Z_MMOD_FRAM)
		snprintf(detail, sizeof(detail), "FRAM (JEDEC continuation). Set "
			"SIZE by hand -- the density is not in the first three bytes.");
	else
		snprintf(detail, sizeof(detail), "Answered, but not in a shape this "
			"understands. Set CLASS, SIZE and ADDR by hand.");

}

// Read the same 16 bytes at each address width so a human can see
// which produces sensible data. Safe because it only reads -- see
// zmmod.h on why guessing is fine for reads and not for writes.
static void do_probe(void) {

	uint8_t a[16], b[16], c[16];
	int i;
	size_t k = 0;

	if (!dev.detected) {
		snprintf(status, sizeof(status), "Run DETECT first.");
		detail[0] = 0;
		return;
	}

	panel_to_profile();
	z_mmod_probe_widths(&dev, range_start, a, b, c);

	k += (size_t)snprintf(status + k, sizeof(status) - k, "2:");
	for (i = 0; i < 6; i++)
		k += (size_t)snprintf(status + k, sizeof(status) - k, " %02x", a[i]);
	k += (size_t)snprintf(status + k, sizeof(status) - k, "   3:");
	for (i = 0; i < 6; i++)
		k += (size_t)snprintf(status + k, sizeof(status) - k, " %02x", b[i]);
	k += (size_t)snprintf(status + k, sizeof(status) - k, "   4:");
	for (i = 0; i < 6; i++)
		k += (size_t)snprintf(status + k, sizeof(status) - k, " %02x", c[i]);

	snprintf(detail, sizeof(detail), "Same 16 bytes at each width. Pick the "
		"one that looks like data, then set ADDR.");

}

// Release every pin. SS in particular: leaving it driven low holds the
// module selected after this app exits, and leaving it driven high on
// a board where that is a marginal level is worse than letting the
// module's own 10k pull-up do the job it is specified for.
static void release_pins(void) {
	if (port >= z_gpio_port_count()) return;
	z_gpio_mode(port, 0, Z_GPIO_IN);
	z_gpio_mode(port, 1, Z_GPIO_IN);
	z_gpio_mode(port, 2, Z_GPIO_IN);
	z_gpio_mode(port, 3, Z_GPIO_IN);
}

// -- dialogs -----------------------------------------------------
//
// sw/common/zdialog.h runs its own message loop while a dialog is up,
// and hands anything that is not the dialog's back through on_msg.
// That callback MUST service the parent window's redraws: wm expects
// an ack, and a window that stops acking while a modal dialog is open
// is one that wm reports as timed out.

static void dialog_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

	case Z_WM_SET_CLIP:
		z_win_apply_clip(&win, &msg->obj);
		break;

	case Z_WM_REDRAW:
		if (msg->obj.type != Z_UINT32) break;
		if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;
		z_win_apply_redraw(&win, msg->obj.val.uint32);
		repaint();
		z_win_redraw_done(&win);
		break;

	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(&win, &msg->obj);
		break;

	default:
		break;

	}

}

static z_dialog_ctx_t dlg;

// Keep only the last path component for display -- the panel's FILE
// well is not wide enough for a path, and the directory is not what
// anyone is checking when they glance at it.
static void set_file(const char *path) {

	const char *base = path;
	const char *p;

	for (p = path; *p; p++)
		if (*p == '/') base = p + 1;

	snprintf(file_path, sizeof(file_path), "%s", path);
	snprintf(file_name, sizeof(file_name), "%s", base);

	file_size = (uint32_t)fs_size(file_path);

}

static void do_open(void) {

	char path[160];

	if (!z_dialog_open(&dlg, NULL, path, sizeof(path))) return;

	set_file(path);

	snprintf(status, sizeof(status), "Source: %s (%lu bytes).",
		file_name, (unsigned long)file_size);
	snprintf(detail, sizeof(detail),
		"VERIFY compares this against the device. WRITE is not wired yet.");

}

// Returns false if the user cancelled, so a READ that opened this
// because no file was set can abandon quietly rather than reporting
// an error nobody caused.
static bool do_save(void) {

	char path[160];

	if (!z_dialog_save(&dlg, NULL, file_name[0] ? file_name : "read.bin",
		path, sizeof(path))) return false;

	set_file(path);

	snprintf(status, sizeof(status), "Destination: %s.", file_name);
	detail[0] = 0;

	return true;

}

// -- range editing ----------------------------------------------
//
// Typed hex rather than a cycle of presets. On a 32MB part the useful
// ranges are not a short list -- testing means reading a window,
// moving it, reading again -- and a preset cycle would either be long
// enough to be tedious or short enough to be useless.

static bool parse_hex(const char *s, uint32_t *out) {

	uint32_t v = 0;
	int n = 0;

	if (!s) return false;

	while (*s == ' ') s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

	for (; *s; s++) {
		uint32_t d;
		if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
		else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
		else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
		else if (*s == ' ') break;
		else return false;
		if (n >= 8) return false;			// would overflow 32 bits
		v = (v << 4) | d;
		n++;
	}

	if (!n) return false;

	*out = v;

	return true;

}

static void edit_range(bool is_start) {

	char cur[16], got[32];
	uint32_t v;

	snprintf(cur, sizeof(cur), "%08lx", (unsigned long)
		(is_start ? range_start : range_len));

	if (!z_dialog_prompt(&dlg, is_start ? "Start address" : "Length",
		is_start ? "Device address, in hex."
			: "Number of bytes, in hex.", cur, got, sizeof(got)))
		return;

	if (!parse_hex(got, &v)) {
		snprintf(status, sizeof(status), "'%s' is not a hex number.", got);
		detail[0] = 0;
		return;
	}

	if (is_start) range_start = v;
	else range_len = v;

	// Clamped against the device rather than refused, because the
	// common mistake is a length that runs past the end and the
	// obvious intent is "to the end". Said out loud so it is not a
	// silent adjustment.
	if (dev_size) {
		if (range_start >= dev_size) {
			range_start = dev_size - 1;
			snprintf(status, sizeof(status), "Start clamped to the last "
				"byte of a %lu byte device.", (unsigned long)dev_size);
		} else if (range_start + range_len > dev_size) {
			range_len = dev_size - range_start;
			snprintf(status, sizeof(status), "Length clamped to %08lx -- "
				"the range would have run past the end.",
				(unsigned long)range_len);
		} else {
			snprintf(status, sizeof(status), "Range %08lx + %08lx.",
				(unsigned long)range_start, (unsigned long)range_len);
		}
	}

	detail[0] = 0;

}

// -- the operation state machine ---------------------------------
//
// A 32MB read over bit-banged SPI is minutes, so an operation cannot
// be a function call: it is a state advanced one chunk at a time from
// the event loop, between message polls. That keeps the window
// repainting, the CANCEL button live, and wm's redraw acks flowing --
// none of which survives a loop that runs to completion.
//
// It is emphatically NOT the maskirq() burst sw/apps/logic uses for
// its capture. That one is bounded at 20ms; this one is not bounded
// at all.

typedef enum { OP_NONE = 0, OP_READ, OP_VERIFY, OP_WRITE,
	OP_ERASE } op_t;

// One chunk. At the ~60 KB/s bit-banging manages this is about 8ms of
// work, which is a comfortable slice: long enough that the per-call
// overhead is amortised, short enough that the window stays
// responsive and CANCEL lands within a frame.
#define OP_CHUNK 512

static op_t op;
static uint32_t op_addr;		// next device address
static uint32_t op_end;
static uint32_t op_done;		// bytes moved
static uint32_t op_t0;			// ticks at start, for the rate
static uint32_t op_base;		// where this operation actually began
static int op_fh = -1;
static uint8_t op_buf[OP_CHUNK];
static uint8_t op_cmp[OP_CHUNK];
static uint32_t src_off;		// where the source is up to

// Read the next `n` bytes from whichever source is selected.
//
// ROM IS A FIXED WINDOW, not an address anyone types. Restricting it
// to Z_ROM_SIZE at Z_ROM_BASE is the whole safety story: an arbitrary
// address field on a machine with no MMU is a way to read a
// peripheral register or an unmapped window and hang the bus, and no
// amount of validation on a typed address is as reliable as not
// having one.
//
// Returns bytes read, 0 at the end of the source, negative on error.
static int src_read(uint8_t *buf, uint32_t n) {

	if (src == SRC_ROM) {

		const volatile uint8_t *rom = (const volatile uint8_t *)Z_ROM_BASE;
		uint32_t i;

		if (src_off >= Z_ROM_SIZE) return 0;
		if (n > Z_ROM_SIZE - src_off) n = Z_ROM_SIZE - src_off;

		// Byte at a time rather than memcpy: this is memory-mapped
		// SPI flash, and going through a volatile pointer keeps the
		// compiler from turning it into something wider than the
		// window's read path expects.
		for (i = 0; i < n; i++) buf[i] = rom[src_off + i];

		src_off += n;

		return (int)n;

	}

	{
		int got = fs_read_chunk(op_fh, buf, (int)n);
		if (got > 0) src_off += (uint32_t)got;
		return got;
	}

}

// How long the source is, or 0 if that is not knowable.
static uint32_t src_len(void) {
	if (src == SRC_ROM) return Z_ROM_SIZE;
	return file_size;
}

static const char *src_name(void) {
	return src == SRC_ROM ? "ROM" : file_name;
}

static void op_finish(const char *how) {

	if (op_fh >= 0) { fs_close_handle(op_fh); op_fh = -1; }

	op = OP_NONE;
	progress = -1;

	snprintf(detail, sizeof(detail), "%s", how);

	relabel();
	repaint();

}

static const char *op_name(void) {
	switch (op) {
	case OP_READ:   return "READ";
	case OP_VERIFY: return "VERIFY";
	case OP_WRITE:  return "WRITE";
	case OP_ERASE:  return "ERASE";
	default:        return "";
	}
}

// Is this operation modifying the device?
//
// Drives what CANCEL says. "Safe" and "the device is half written"
// are very different things to tell somebody hovering over that
// button, and getting it wrong in the reassuring direction is worse
// than not saying anything.
static bool op_destructive(void) {
	return op == OP_WRITE || op == OP_ERASE;
}

static void op_rate_line(void) {

	uint32_t el = z_uptime_ticks() - op_t0;
	uint32_t kbs = 0;
	uint32_t pct = op_end > op_base
		? (op_done * 100u) / (op_end - op_base) : 0;

	// Measured, never assumed -- the same rule i2c-khz follows. The
	// two backends differ by more than an order of magnitude and a
	// figure taken from the request rather than the clock would be
	// wrong by that much.
	if (el) kbs = (op_done * Z_TICK_HZ) / (el * 1024u);

	snprintf(lbl_rate, sizeof(lbl_rate), "%lu KB/s", (unsigned long)kbs);

	progress = (int)pct;

	snprintf(status, sizeof(status), "%s %lu / %lu bytes   %lu KB/s",
		op_name(),
		(unsigned long)op_done,
		(unsigned long)(op_end - op_base),
		(unsigned long)kbs);

	{
		char eta[32];

		eta[0] = 0;
		if (kbs && op_end > op_base + op_done)
			snprintf(eta, sizeof(eta), "about %lus remaining   ",
				(unsigned long)(((op_end - op_base) - op_done)
					/ (kbs * 1024u)));

		if (op_destructive())
			// Cancelling a write or an erase stops between units, so
			// nothing is left half-programmed -- but what has already
			// gone is gone, and saying "safe" here would be a lie.
			snprintf(detail, sizeof(detail), "%sCANCEL stops cleanly, but "
				"what is already %s stays that way", eta,
				op == OP_ERASE ? "erased" : "written");
		else
			snprintf(detail, sizeof(detail), "%sCANCEL is safe -- nothing "
				"is being written", eta);
	}

}

// One slice of work. Returns true while there is more to do.
static bool op_step(void) {

	uint32_t n;
	z_mmod_rv rv;

	// -- erase: one sector per slice --
	//
	// A sector erase is specified in the hundreds of milliseconds, so
	// one per pass is already a long slice; several would make CANCEL
	// and the repaint noticeably late for no gain.
	if (op == OP_ERASE) {

		if (op_addr >= op_end) {
			op_rate_line();
			{
				char done[64];
				snprintf(done, sizeof(done), "Erased %lu bytes.",
					(unsigned long)op_done);
				op_finish(done);
			}
			return false;
		}

		rv = z_mmod_erase_sector(&dev, op_addr);

		if (rv != Z_MMOD_OK) {
			snprintf(status, sizeof(status), "ERASE failed at %08lx: %s",
				(unsigned long)op_addr, z_mmod_strerror(rv));
			op_finish("Stopped. The device is partly erased.");
			return false;
		}

		op_addr += dev.sector;
		op_done += dev.sector;

		op_rate_line();

		return true;

	}

	n = op_end - op_addr;
	if (n > OP_CHUNK) n = OP_CHUNK;
	if (!n) { op_finish("Done."); return false; }

	// -- write: file in, device out --
	if (op == OP_WRITE) {

		int got = src_read(op_buf, n);

		if (got <= 0) {
			// Running out of source is the normal end of a write whose
			// range was longer than it, not an error.
			op_rate_line();
			{
				char done[80];
				snprintf(done, sizeof(done), "Wrote %lu bytes. "
					"VERIFY to confirm.", (unsigned long)op_done);
				op_finish(done);
			}
			return false;
		}

		n = (uint32_t)got;

		rv = z_mmod_write(&dev, op_addr, op_buf, n);

		if (rv != Z_MMOD_OK) {
			snprintf(status, sizeof(status), "WRITE failed at %08lx: %s",
				(unsigned long)op_addr, z_mmod_strerror(rv));
			// A blank-check failure here is the common case and has a
			// specific fix, so name it rather than leaving the user
			// with "data did not match" on a write.
			if (rv == Z_MMOD_VERIFY)
				op_finish("That range is not erased. ERASE it first.");
			else
				op_finish("Stopped. The device is partly written.");
			return false;
		}

		op_addr += n;
		op_done += n;

		if (op_addr >= op_end) {
			op_rate_line();
			{
				char done[80];
				snprintf(done, sizeof(done), "Wrote %lu bytes. "
					"VERIFY to confirm.", (unsigned long)op_done);
				op_finish(done);
			}
			return false;
		}

		op_rate_line();

		return true;

	}

	// -- read and verify: device out --

	rv = z_mmod_read(&dev, op_addr, op_buf, n);

	if (rv != Z_MMOD_OK) {
		snprintf(status, sizeof(status), "READ failed at %08lx: %s",
			(unsigned long)op_addr, z_mmod_strerror(rv));
		op_finish("Stopped.");
		return false;
	}

	if (op == OP_READ) {

		if (fs_write_chunk(op_fh, op_buf, (int)n) != (int)n) {
			snprintf(status, sizeof(status), "Write to '%s' failed at "
				"%lu bytes -- disk full?", file_name,
				(unsigned long)op_done);
			op_finish("Stopped.");
			return false;
		}

	} else {

		int got = src_read(op_cmp, n);

		// End of file ENDS THE VERIFY SUCCESSFULLY.
		//
		// The question VERIFY answers is "does the device match this
		// file", so the file's length is the answer's length. Running
		// out of it means every byte the file has, matched -- which
		// is a pass, not the failure an earlier version reported.
		//
		// op_start() also clamps the range to the file size, so this
		// normally does not fire. It is not redundant: fs_size()
		// returns 0 both for an empty file and for one it cannot
		// stat, so the clamp cannot always be applied and this is
		// what makes the behaviour correct either way.
		//
		// The one case that is genuinely wrong is an empty or
		// unreadable file, where nothing was compared at all.
		if (got <= 0) {

			if (!op_done) {
				snprintf(status, sizeof(status),
					"'%s' is empty or unreadable -- nothing to compare.",
					src_name());
				op_finish("Stopped.");
				return false;
			}

			op_rate_line();
			{
				char done[80];
				snprintf(done, sizeof(done), "Verified -- %lu bytes match, "
					"no differences.", (unsigned long)op_done);
				op_finish(done);
			}
			return false;

		}

		if (got < (int)n) n = (uint32_t)got;

		{
			uint32_t i;
			for (i = 0; i < n; i++)
				if (op_buf[i] != op_cmp[i]) {
					snprintf(status, sizeof(status), "MISMATCH at %08lx: "
						"device %02x, file %02x",
						(unsigned long)(op_addr + i), op_buf[i], op_cmp[i]);
					op_finish("Verify failed.");
					return false;
				}
		}

	}

	op_addr += n;
	op_done += n;

	if (op_addr >= op_end) {
		op_rate_line();
		{
			char done[64];
			if (op == OP_READ)
				snprintf(done, sizeof(done), "Read complete -- %lu bytes.",
					(unsigned long)op_done);
			else
				// The byte count matters here: it is the file's
				// length, which may be less than the range, and
				// "verified" without a number would not say how much.
				snprintf(done, sizeof(done), "Verified -- %lu bytes match, "
					"no differences.", (unsigned long)op_done);
			op_finish(done);
		}
		return false;
	}

	op_rate_line();

	return true;

}

static void op_start(op_t which) {

	uint32_t len = range_len;

	if (!dev.detected) {
		snprintf(status, sizeof(status), "Run DETECT first.");
		detail[0] = 0;
		return;
	}

	// No file yet? Ask, rather than reporting an error the user did
	// nothing to cause. Cancelling the dialog abandons quietly.
	// READ has no ROM setting: ROM is read-only, so there is nowhere
	// for the device's contents to go. Say so rather than silently
	// switching the selector under the user.
	if (which == OP_READ && src == SRC_ROM) {
		snprintf(status, sizeof(status),
			"READ writes to a file. Set SRC to FILE first.");
		snprintf(detail, sizeof(detail),
			"ROM is read-only -- it can be a source, not a destination.");
		return;
	}

	if (!file_path[0] && src == SRC_FILE) {
		if (which == OP_READ) { if (!do_save()) return; }
		else { do_open(); if (!file_path[0]) return; }
	}

	// READ REPLACES THE FILE, and the file is very likely one that
	// was just used for something else.
	//
	// The panel keeps one filename across every operation, so the
	// path sitting in it after a WRITE or a VERIFY is the SOURCE for
	// that operation -- and pressing READ next would destroy the
	// image you just wrote from, with the device as the only
	// remaining copy. That is a bad way to lose a firmware image.
	//
	// sw/common/zdialog.h's save dialog deliberately does not ask
	// about overwriting ("that is the caller's decision to make"),
	// so this is where it gets asked. It covers both routes in: a
	// path carried over from an earlier operation, and one just
	// chosen in the save dialog that happens to exist.
	if (which == OP_READ) {

		uint32_t existing = (uint32_t)fs_size(file_path);

		if (existing) {

			char msg[220];

			snprintf(msg, sizeof(msg),
				"Replace %s?\n"
				"It already exists -- %lu bytes.\n"
				"%s\n"
				"Use the save icon to read into a different file.",
				file_name, (unsigned long)existing,
				(existing == file_size && file_size)
					? "This is the file the last operation used."
					: "Its contents will be lost.");

			if (z_dialog_confirm(&dlg, "Read", msg, Z_DIALOG_YES_NO)
				!= Z_DIALOG_YES) {
				snprintf(status, sizeof(status), "Read cancelled -- %s "
					"was not touched.", file_name);
				detail[0] = 0;
				return;
			}

		}

	}

	panel_to_profile();

	// VERIFY compares the device against the FILE, so the file's
	// length is the authority -- reading past its end and reporting
	// "the file ended" would be inventing a failure out of the
	// ordinary case of a range wider than its reference.
	//
	// WRITE already clamped the same way; not doing it here too was
	// an inconsistency, and it showed up as a verify that could never
	// pass unless the file happened to be exactly the range length.
	//
	// A file LONGER than the range still verifies only the range: the
	// user set that deliberately.
	if (which == OP_VERIFY && src_len() && len > src_len())
		len = src_len();

	if (!len || (dev.size && range_start + len > dev.size)) {
		snprintf(status, sizeof(status), "Range %08lx + %08lx is outside a "
			"%lu byte device.", (unsigned long)range_start,
			(unsigned long)len, (unsigned long)dev.size);
		detail[0] = 0;
		return;
	}

	// ROM needs no handle. Left at -1 so op_finish()'s close is a
	// no-op and there is one exit path rather than two.
	op_fh = -1;

	if (src == SRC_FILE || which == OP_READ)
		op_fh = (which == OP_READ)
			? fs_open_write(file_path) : fs_open_read(file_path);

	if (op_fh < 0 && (src == SRC_FILE || which == OP_READ)) {
		snprintf(status, sizeof(status), "Could not open '%s' for %s.",
			file_path, which == OP_READ ? "writing" : "reading");
		snprintf(detail, sizeof(detail), "Is the SD card mounted?");
		return;
	}

	op = which;
	op_base = range_start;
	op_addr = range_start;
	op_end = range_start + len;
	op_done = 0;
	op_t0 = z_uptime_ticks();
	progress = 0;

	// ROM offsets track device addresses, so a range means the same
	// place in both. Backing up the whole thing is START 0, and
	// backing up just the kernel is its own offset in both -- which
	// is the behaviour that makes a partial backup restorable to
	// where it came from.
	src_off = (src == SRC_ROM) ? range_start : 0;

	op_rate_line();

	// Say when the length came from the file rather than the range,
	// so a run that compares less than the range was asked for does
	// not look like it stopped early.
	if (which == OP_VERIFY && len < range_len)
		snprintf(detail, sizeof(detail), "Comparing %lu bytes -- the %s is "
			"shorter than the range.", (unsigned long)len,
			src == SRC_ROM ? "ROM window" : "file");

}

// Start an erase, after showing exactly what will be destroyed.
//
// THE RANGE IS SNAPPED TO SECTOR BOUNDARIES AND THE SNAP IS SHOWN.
// NOR flash can only erase whole sectors, so a byte range is a lie
// there -- and widening one silently is how a neighbouring sector
// gets lost to a range that was not aligned. The dialog names the
// snapped range, and says so explicitly when it differs from what was
// asked for.
static void erase_start(void) {

	uint32_t first, last, bytes, sectors;
	char msg[220];

	if (!dev.detected) {
		snprintf(status, sizeof(status), "Run DETECT first.");
		detail[0] = 0;
		return;
	}

	panel_to_profile();

	if (!dev.sector) {
		snprintf(status, sizeof(status), "%s has no erase operation.",
			z_mmod_class_name(dev.cls));
		snprintf(detail, sizeof(detail),
			"FRAM and EEPROM are overwritten in place -- just WRITE.");
		return;
	}

	if (!range_len) {
		snprintf(status, sizeof(status), "Length is zero.");
		detail[0] = 0;
		return;
	}

	first = range_start - (range_start % dev.sector);
	last = range_start + range_len - 1;
	last = last - (last % dev.sector) + dev.sector - 1;

	if (dev.size && last >= dev.size) last = dev.size - 1;

	bytes = last - first + 1;
	sectors = bytes / dev.sector;

	snprintf(msg, sizeof(msg),
		"Erase %08lx - %08lx?\n"
		"%lu bytes, %lu sector%s of %lu.\n"
		"%s\n"
		"This cannot be undone.",
		(unsigned long)first, (unsigned long)last,
		(unsigned long)bytes, (unsigned long)sectors,
		sectors == 1 ? "" : "s", (unsigned long)dev.sector,
		(first != range_start || last != range_start + range_len - 1)
			? "Widened from the range you set, to whole sectors."
			: "Exactly the range you set.");

	if (z_dialog_confirm(&dlg, "Erase", msg, Z_DIALOG_YES_NO)
		!= Z_DIALOG_YES) {
		snprintf(status, sizeof(status), "Erase cancelled.");
		detail[0] = 0;
		return;
	}

	op = OP_ERASE;
	op_base = first;
	op_addr = first;
	op_end = last + 1;
	op_done = 0;
	op_t0 = z_uptime_ticks();
	progress = 0;

	op_rate_line();

}

// Start a write. Confirms, because this is the other operation that
// destroys what was there.
static void write_start(void) {

	char msg[220];
	uint32_t len;

	if (!dev.detected) {
		snprintf(status, sizeof(status), "Run DETECT first.");
		detail[0] = 0;
		return;
	}

	if (!file_path[0] && src == SRC_FILE) {
		do_open();
		if (!file_path[0]) return;
	}

	panel_to_profile();

	len = range_len;
	if (src_len() && len > src_len()) len = src_len();

	if (!len) {
		snprintf(status, sizeof(status), "Nothing to write -- "
			"length is zero or the file is empty.");
		detail[0] = 0;
		return;
	}

	if (dev.size && range_start + len > dev.size) {
		snprintf(status, sizeof(status), "Range runs past the end of a %lu "
			"byte device.", (unsigned long)dev.size);
		detail[0] = 0;
		return;
	}

	if (src == SRC_ROM)
		snprintf(msg, sizeof(msg),
			"Write %lu bytes from\nROM %08lx (boot flash)\n"
			"to %08lx - %08lx?\n%s",
			(unsigned long)len,
			(unsigned long)(Z_ROM_BASE + range_start),
			(unsigned long)range_start,
			(unsigned long)(range_start + len - 1),
			dev.needs_erase
				? "The range must already be erased."
				: "This overwrites whatever is there.");
	else
		snprintf(msg, sizeof(msg),
			"Write %lu bytes from\n%s\nto %08lx - %08lx?\n%s",
			(unsigned long)len, file_name,
			(unsigned long)range_start,
			(unsigned long)(range_start + len - 1),
			dev.needs_erase
				? "The range must already be erased."
				: "This overwrites whatever is there.");

	if (z_dialog_confirm(&dlg, "Write", msg, Z_DIALOG_YES_NO)
		!= Z_DIALOG_YES) {
		snprintf(status, sizeof(status), "Write cancelled.");
		detail[0] = 0;
		return;
	}

	op_fh = -1;

	if (src == SRC_FILE) {
		op_fh = fs_open_read(file_path);
		if (op_fh < 0) {
			snprintf(status, sizeof(status), "Could not open '%s'.",
				file_path);
			detail[0] = 0;
			return;
		}
	}

	op = OP_WRITE;
	op_base = range_start;
	op_addr = range_start;
	op_end = range_start + len;
	op_done = 0;
	op_t0 = z_uptime_ticks();
	progress = 0;
	src_off = (src == SRC_ROM) ? range_start : 0;

	op_rate_line();

}

static void op_cancel(void) {

	bool wrote = op_destructive();

	if (op == OP_NONE) return;

	snprintf(status, sizeof(status), "Cancelled after %lu bytes.",
		(unsigned long)op_done);

	// Say which it was. Cancelling a read leaves a short file and an
	// untouched device; cancelling a write or erase leaves the device
	// partly changed, and telling somebody it was "not modified"
	// there would be a comfortable lie.
	op_finish(wrote
		? "The device is partly modified -- VERIFY before trusting it."
		: "The device was not modified.");

}

static void not_yet(const char *what) {
	snprintf(status, sizeof(status), "%s is not built yet.", what);
	snprintf(detail, sizeof(detail), "The device layer supports it; the app "
		"does not wire it yet. See docs/mmod.md.");
}

// -- actions -----------------------------------------------------

static void act(int idx) {

	bool full = false;

	if (idx < 0) return;

	switch (idx) {

	case W_PORT:
		port = (port + 1) % (z_gpio_port_count() ? z_gpio_port_count() : 1);
		snprintf(lbl_port, sizeof(lbl_port), "%lu", (unsigned long)port);
		dev_detected = false;
		ss_ok = false;
		full = true;
		break;

	case W_DETECT:
		do_detect();
		full = true;
		break;

	case W_SRC:
		if (src == SRC_FILE && !z_soc_has_feature(Z_FEATURE_MEM_ROM)) {
			// No mapped boot flash on this board, so offering it
			// would be offering a window that reads as whatever the
			// bus resolves to.
			snprintf(status, sizeof(status),
				"This bitstream has no memory-mapped ROM.");
			snprintf(detail, sizeof(detail), "Z_FEATURE_MEM_ROM is clear "
				"-- see docs/csrs.md.");
			full = true;
			break;
		}
		src = (src == SRC_FILE) ? SRC_ROM : SRC_FILE;
		if (src == SRC_ROM) {
			snprintf(status, sizeof(status), "Source is the boot flash at "
				"%08lx, %lu bytes.", (unsigned long)Z_ROM_BASE,
				(unsigned long)Z_ROM_SIZE);
			snprintf(detail, sizeof(detail), "WRITE copies it to the "
				"device; VERIFY compares them. READ needs a file.");
		} else {
			snprintf(status, sizeof(status), "Source is a file.");
			detail[0] = 0;
		}
		full = true;
		break;

	case W_CLASS:
		dev_class = (class_t)((dev_class + 1) % 4);
		// FRAM has no erase at all, so the sector size is meaningless
		// there and saying "4K" would be a lie.
		panel_to_profile();
		full = true;
		break;

	case W_ADDR:
		addr_idx = (addr_idx + 1) % ADDR_COUNT;
		panel_to_profile();
		full = true;
		break;

	case W_ALL:
		range_start = 0;
		range_len = dev_size;
		full = true;
		break;

	case W_ERASE:  erase_start(); full = true; break;

	case W_READ:   op_start(OP_READ);   full = true; break;
	case W_WRITE:  write_start(); full = true; break;
	case W_VERIFY: op_start(OP_VERIFY); full = true; break;
	case W_PROBE:  do_probe(); full = true; break;
	case W_SIZE:   not_yet("Editing SIZE"); full = true; break;
	case W_START:  edit_range(true);  full = true; break;
	case W_LEN:    edit_range(false); full = true; break;
	case W_CANCEL: op_cancel(); full = true; break;

	default: return;

	}

	if (full) {
		relabel();
		repaint();
	}

}

// -- input -------------------------------------------------------

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Titlebar samples arrive here too -- wm's hit test is the whole
	// window rect. Same guard as sw/apps/settings and sw/apps/term.
	if (!inside && wset.pressed < 0) return;

	act(z_widget_mouse(&wset, cx, cy, buttons));

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	switch (keysym) {
	case '\t':
		z_widget_focus_next(&wset, (mods & Z_KBD_MOD_SHIFT) != 0);
		z_widget_draw_all(&wset, false);
		return;
	case 0x0d:
	case ' ':
		act(z_widget_key_activate(&wset));
		return;
	case 'd':
	case 'D':
		act(W_DETECT);
		return;
	default:
		return;
	}

}

// -- main --------------------------------------------------------

int main(void) {

	printf("mmod: starting\n");

	if (!z_gpio_present() || !z_gpio_port_count()) {
		// Said on the console rather than in a window: a window whose
		// only content is "there is no hardware" is a window in the
		// way.
		printf("mmod: this bitstream has no GPIO ports with pins.\n");
		printf("mmod: try `zrelease build obst_uart_gpio` "
			"-- see docs/gpio.md\n");
		return 1;
	}

	if (z_win_create_flags(&win, "mmod", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER
		| Z_WIN_FLAG_OPEN_ICON | Z_WIN_FLAG_SAVE_ICON) != Z_OK) {
		printf("mmod: failed to create window -- is wm running?\n");
		return 1;
	}

	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}

	port = 0;
	state_init();

	// No default filename. An earlier version used "mmod.bin", which
	// is one character away from the app's own executable and reads
	// like something the user chose. READ asks for a destination the
	// first time instead.
	dlg.parent = &win;
	dlg.on_msg = dialog_msg;
	dlg.user = NULL;
	widgets_init();
	relabel();
	layout();
	repaint();

	for (;;) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

			case Z_WM_KEY:
				if (msg.obj.type != Z_UINT32) break;
				if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;
				handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
					(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));
				break;

			case Z_WM_MOUSE:
				if (msg.obj.type == Z_UINT32)
					handle_mouse(msg.obj.val.uint32);
				break;

			case Z_WM_TITLEBAR_ICON:
				// The titlebar's open and save icons exist because
				// the panel is built around a file, but the file
				// picker is not wired up yet. Saying so beats a
				// button that looks live and does nothing.
				if (msg.obj.type == Z_UINT32) {
					uint32_t v = msg.obj.val.uint32;
					if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;
					if (Z_WM_UNPACK_TBICON_KIND(v) == Z_WM_TBICON_OPEN)
						do_open();
					else
						do_save();
					relabel();
					repaint();
				}
				break;

			case Z_WM_SET_CLIP:
				z_win_apply_clip(&win, &msg.obj);
				break;

			case Z_WM_REDRAW:
				if (msg.obj.type != Z_UINT32) break;
				if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
				z_win_apply_redraw(&win, msg.obj.val.uint32);
				repaint();
				z_win_redraw_done(&win);
				break;

			case Z_WM_WINDOW_MOVED:
				z_win_parse_rect(&win, &msg.obj);
				break;

			default:
				break;

			}

		}

		if (op != OP_NONE) {

			// One slice, then back to the message loop. Several
			// slices per pass would move more per wakeup and make
			// CANCEL and the repaint proportionally later; one is
			// the responsive choice and the throughput difference is
			// small next to the SPI transfer itself.
			if (op_step()) repaint_progress();

			// No wait: an operation should run as fast as the
			// scheduler allows, and z_proc_wait() here would cap it
			// at 20 chunks a second -- 10 KB/s, far below what even
			// bit-banging manages.
			continue;

		}

		z_proc_wait(Z_TICK_HZ / 20);

	}

	release_pins();

	return 0;

}
