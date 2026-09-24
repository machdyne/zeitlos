#!/usr/bin/env python3
"""
Generates sw/common/zkbd_layouts.c -- the keyboard layout tables -- from
xkeyboard-config, the layout database every Linux desktop uses. See
docs/keyboard_layouts.md.

Not run by the build. Run it by hand when a layout is added or
xkeyboard-config should be re-synced, and commit the regenerated file:

    python3 tools/gen_kbd_layouts.py XKB_SYMBOLS_DIR KEYSYMDEF_H

    XKB_SYMBOLS_DIR  .../share/X11/xkb/symbols  (Debian/Ubuntu: xkb-data)
    KEYSYMDEF_H      .../include/X11/keysymdef.h (Debian/Ubuntu: x11proto-dev)

Why generate at all: a layout transcribed by hand is wrong in a dozen
small places nobody notices until they need that key. xkeyboard-config
is what the person's other computer already does, so matching it
exactly is the whole specification.

What is taken from xkb, and what is not:

  - The four shift levels of every key in the main block, the ISO key
    beside left Shift, the Brazilian ABNT2 key beside right Shift, and
    the keypad decimal. Level 1 plain, 2 Shift, 3 AltGr, 4 Shift+AltGr.
  - Whether the layout has AltGr at all (it includes level3(ralt_switch)).
  - Dead keys, as dead keys. What they COMBINE into is not taken from
    X11's Compose file (tens of thousands of lines, most of it for
    other scripts); it is computed from Unicode: dead key + base letter
    is the precomposed character, if Unicode has one.
  - Nothing else. Key types, groups, actions and the non-alphanumeric
    keys are the same on every layout here and live in zkbd.c.

Output is deterministic, so a re-run with the same inputs is a no-op.
"""

import re
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "sw" / "common" / "zkbd_layouts.c"

# The layouts, in ID order. The ID is what the kernel stores and stamps
# into key events (zkbd.h), so this order is an ABI within one build:
# append only. (name, xkb file, xkb section or None for default, label)
LAYOUTS = [
	("us",            "us",    None,          "US"),
	("gb",            "gb",    None,          "GB"),
	("de",            "de",    None,          "DE"),
	("de-nodeadkeys", "de",    "nodeadkeys",  "DE"),
	("it",            "it",    None,          "IT"),
	("fr",            "fr",    None,          "FR"),
	("es",            "es",    None,          "ES"),
	("latam",         "latam", None,          "LA"),
	("br",            "br",    None,          "BR"),
	# -- phase 5, docs/keyboard_layouts.md --
	("ch",            "ch",    None,          "CH"),
	("ch-fr",         "ch",    "fr",          "CH"),
	("se",            "se",    None,          "SE"),
	("fi",            "fi",    None,          "FI"),
	("dk",            "dk",    None,          "DK"),
	("no",            "no",    None,          "NO"),
	("pt",            "pt",    None,          "PT"),
	("be",            "be",    None,          "BE"),
	("us-intl",       "us",    "intl",        "US"),
	("jp",            "jp",    None,          "JP"),
	# -- phase 8: Japanese input methods. The keys are the base
	# layout's; wm turns the letters typed into kana (zkbd.h, "input
	# methods"). A fifth element marks an input method: 1 hiragana,
	# 2 katakana.
	("ja",            "jp",    None,          "JA", 1),
	("ja-kata",       "jp",    None,          "JK", 2),
	("ja-us",         "us",    None,          "JA", 1),
]

# xkb key name -> USB HID keyboard usage(s). Linux sends both 0x31
# (US backslash) and 0x32 (ISO "Non-US #") to <BKSL>, so both map here:
# a keyboard has one or the other, never both.
XKB_TO_USAGE = {
	"TLDE": [0x35],
	"AE01": [0x1E], "AE02": [0x1F], "AE03": [0x20], "AE04": [0x21],
	"AE05": [0x22], "AE06": [0x23], "AE07": [0x24], "AE08": [0x25],
	"AE09": [0x26], "AE10": [0x27], "AE11": [0x2D], "AE12": [0x2E],
	"AD01": [0x14], "AD02": [0x1A], "AD03": [0x08], "AD04": [0x15],
	"AD05": [0x17], "AD06": [0x1C], "AD07": [0x18], "AD08": [0x0C],
	"AD09": [0x12], "AD10": [0x13], "AD11": [0x2F], "AD12": [0x30],
	"AC01": [0x04], "AC02": [0x16], "AC03": [0x07], "AC04": [0x09],
	"AC05": [0x0A], "AC06": [0x0B], "AC07": [0x0D], "AC08": [0x0E],
	"AC09": [0x0F], "AC10": [0x33], "AC11": [0x34],
	"BKSL": [0x31, 0x32],
	"LSGT": [0x64],
	"AB01": [0x1D], "AB02": [0x1B], "AB03": [0x06], "AB04": [0x19],
	"AB05": [0x05], "AB06": [0x11], "AB07": [0x10], "AB08": [0x36],
	"AB09": [0x37], "AB10": [0x38],
	"AB11": [0x87],	# ABNT2 / JIS key beside right Shift
	"AE13": [0x89],	# JIS yen key, left of Backspace
}

# Dead keys: xkb name -> (combining character, spacing form or None).
# The ID of a dead key is its index here; a table entry for dead key n
# is DEAD_BASE + n. Order is part of the generated tables only.
DEAD_KEYS = [
	("dead_grave",        0x0300, 0x0060),
	("dead_acute",        0x0301, 0x00B4),
	("dead_circumflex",   0x0302, 0x005E),
	("dead_tilde",        0x0303, 0x007E),
	("dead_macron",       0x0304, 0x00AF),
	("dead_breve",        0x0306, 0x02D8),
	("dead_abovedot",     0x0307, 0x02D9),
	("dead_diaeresis",    0x0308, 0x00A8),
	("dead_abovering",    0x030A, 0x00B0),
	("dead_doubleacute",  0x030B, 0x02DD),
	("dead_caron",        0x030C, 0x02C7),
	("dead_cedilla",      0x0327, 0x00B8),
	("dead_ogonek",       0x0328, 0x02DB),
	("dead_belowdot",     0x0323, None),
	("dead_hook",         0x0309, None),
	("dead_horn",         0x031B, None),
	("dead_belowmacron",  0x0331, None),
	("dead_belowcomma",   0x0326, None),
	("dead_stroke",       None,   None),
]
DEAD_INDEX = {name: i for i, (name, _, _) in enumerate(DEAD_KEYS)}
DEAD_BASE = 0xF000
UNKNOWN_DEAD = set()		# reported at the end: candidates for DEAD_KEYS		# zkbd.h Z_KBD_DEAD_BASE: private use, never a real key

# dead_stroke has no combining mark; Unicode has the letters.
STROKE = {
	"d": 0x0111, "D": 0x0110, "h": 0x0127, "H": 0x0126, "l": 0x0142,
	"L": 0x0141, "o": 0x00F8, "O": 0x00D8, "t": 0x0167, "T": 0x0166,
	"b": 0x0180, "g": 0x01E5, "G": 0x01E4, "i": 0x0268, "z": 0x01B6,
	"Z": 0x01B5,
}


def load_keysyms(path):
	"""keysym name -> Unicode codepoint.

	keysymdef.h names most keysyms' character in a /* U+xxxx */ comment.
	Deprecated aliases (guillemotleft, masculine, ...) have no such
	comment, so every name is also resolved through its keysym VALUE:
	another name with the same value and a comment, or X11's own rules
	-- Latin-1 keysyms are their codepoint, and 0x01xxxxxx is
	U+xxxxxx."""
	pat = re.compile(r"#define XK_(\w+)\s+0x([0-9a-fA-F]+)(.*)")
	upat = re.compile(r"U\+([0-9A-Fa-f]+)")
	by_name, by_value = {}, {}
	for line in open(path, encoding="utf-8", errors="replace"):
		m = pat.match(line)
		if not m:
			continue
		name, val = m.group(1), int(m.group(2), 16)
		by_name.setdefault(name, val)
		u = upat.search(m.group(3))
		if u:
			by_value.setdefault(val, int(u.group(1), 16))
	table = {}
	for name, val in by_name.items():
		if val in by_value:
			table[name] = by_value[val]
		elif 0x20 <= val <= 0x7E or 0xA0 <= val <= 0xFF:
			table[name] = val
		elif val & 0xFF000000 == 0x01000000:
			table[name] = val & 0x00FFFFFF
	return table


def keysym_to_code(name, ks):
	"""A table value: a codepoint, a dead key, or 0 for nothing."""
	if name in ("NoSymbol", "VoidSymbol"):
		return 0
	if name in DEAD_INDEX:
		return DEAD_BASE + DEAD_INDEX[name]
	if name.startswith("dead_"):
		UNKNOWN_DEAD.add(name)
		return 0			# a dead key this table does not know: nothing
	m = re.fullmatch(r"U([0-9A-Fa-f]{4,6})", name)
	if m:
		cp = int(m.group(1), 16)
	elif len(name) == 1:
		cp = ord(name)
	elif name in ks:
		cp = ks[name]
	elif re.fullmatch(r"0x[0-9a-fA-F]+", name):
		cp = int(name, 16) - 0x01000000 if int(name, 16) > 0x01000000 else 0
	else:
		return 0			# a function keysym (arrows etc): not text
	if cp > 0xFFFF or 0xF000 <= cp < 0xF100:
		return 0			# does not fit the table; none of these layouts need it
	return cp


class Section:
	def __init__(self):
		self.keys = {}		# xkb key name -> list of keysym names
		self.altgr = False


def strip_comments(text):
	text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
	return re.sub(r"//[^\n]*", " ", text)


def find_section(symdir, fname, sect):
	text = strip_comments((symdir / fname).read_text(encoding="utf-8", errors="replace"))
	# Every xkb_symbols block, with the flags before it.
	blocks = []
	for m in re.finditer(r'((?:\w+\s+)*)xkb_symbols\s+"([^"]+)"\s*\{', text):
		depth, i = 1, m.end()
		while depth:
			c = text[i]
			depth += (c == "{") - (c == "}")
			i += 1
		blocks.append((m.group(1), m.group(2), text[m.end():i - 1]))
	if sect is None:
		for flags, name, body in blocks:
			if "default" in flags.split():
				return body
		return blocks[0][2]
	for flags, name, body in blocks:
		if name == sect:
			return body
	sys.exit(f"{fname}({sect}): no such section")


def parse(symdir, fname, sect, out, depth=0):
	if depth > 10:
		sys.exit("include loop")
	body = find_section(symdir, fname, sect)
	for stmt in re.finditer(r'(include|augment|override|replace)\s+"([^"]+)"|key\s*<(\w+)>\s*\{(.*?)\}\s*;', body, flags=re.S):
		if stmt.group(1):
			for part in re.split(r"[+|]", stmt.group(2)):
				m = re.fullmatch(r"([\w-]+)(?:\(([\w-]+)\))?(?::\d+)?", part.strip())
				if not m:
					continue
				f, s = m.group(1), m.group(2)
				if f == "level3" and s in ("ralt_switch", None):
					out.altgr = True
					continue
				if not (symdir / f).is_file():
					continue
				parse(symdir, f, s, out, depth + 1)
			continue
		key, spec = stmt.group(3), stmt.group(4)
		# The first [ ... ] list is Group1's symbols, whether written
		# bare or as symbols[Group1]= [ ... ].
		m = re.search(r"\[([^\]]*)\]", spec)
		if not m:
			continue
		syms = [s.strip() for s in m.group(1).split(",") if s.strip()]
		out.keys[key] = syms	# a redefinition replaces the key's levels


def find_name(symdir, fname, sect, depth=0):
	"""The layout's display name: its own name[Group1], or the first
	one found through its includes (se's default section has none of
	its own and takes se(se)'s)."""
	body = find_section(symdir, fname, sect)
	m = re.search(r'name\[Group1\]\s*=\s*"([^"]+)"', body)
	if m:
		return m.group(1)
	if depth > 5:
		return None
	for inc in re.findall(r'include\s+"([^"]+)"', body):
		m = re.fullmatch(r"([\w-]+)(?:\(([\w-]+)\))?", inc.strip())
		if m and (symdir / m.group(1)).is_file() and m.group(1) not in ("latin", "pc", "level3", "kpdl", "nbsp", "keypad"):
			n = find_name(symdir, m.group(1), m.group(2), depth + 1)
			if n:
				return n
	return None


def compose_table(ks_used):
	"""(dead, base, result) for every dead key + base letter Unicode can
	precompose, plus the few that are not combining marks."""
	bases = [chr(c) for c in range(0x41, 0x5B)] + [chr(c) for c in range(0x61, 0x7B)]
	# A handful of non-ASCII bases the layouts here put a dead key next
	# to: ü with acute, ç with acute (Vietnamese/Portuguese habits), æ.
	bases += ["\u00fc", "\u00dc", "\u00e6", "\u00c6", "\u00e7", "\u00c7", "\u00f8", "\u00d8"]
	rows = []
	for i, (name, comb, spacing) in enumerate(DEAD_KEYS):
		if DEAD_BASE + i not in ks_used:
			continue
		for b in bases:
			if name == "dead_stroke":
				r = STROKE.get(b)
				if r:
					rows.append((DEAD_BASE + i, ord(b), r))
				continue
			n = unicodedata.normalize("NFC", b + chr(comb))
			if len(n) == 1 and ord(n) <= 0xFFFF:
				rows.append((DEAD_BASE + i, ord(b), ord(n)))
	return sorted(rows)


def c_char_comment(cp):
	if cp == 0:
		return "-"
	if cp >= DEAD_BASE and cp < DEAD_BASE + 0x100:
		return DEAD_KEYS[cp - DEAD_BASE][0]
	ch = chr(cp)
	if cp < 0x20 or cp == 0x7F or unicodedata.category(ch).startswith(("M", "C", "Z")) and ch != " ":
		return f"U+{cp:04X}"
	if ch in "\\/*":
		return {"\\": "backslash", "/": "slash", "*": "asterisk"}[ch]
	return ch


def main():
	if len(sys.argv) != 3:
		sys.exit(__doc__)
	symdir, ksh = Path(sys.argv[1]), Path(sys.argv[2])
	ks = load_keysyms(ksh)

	out = []
	w = out.append
	w("/* Generated by tools/gen_kbd_layouts.py from xkeyboard-config -- do not hand-edit. */")
	w("/* See docs/keyboard_layouts.md. */")
	w("")
	w("#include <stdint.h>")
	w('#include "zkbd.h"')
	w("")

	used = set()
	layout_rows = []
	emitted = {}
	for lid, entry in enumerate(LAYOUTS):
		name, fname, sect, label = entry[:4]
		ime = entry[4] if len(entry) > 4 else 0
		# What a desktop actually loads is "pc+<layout>": the pc symbols
		# first (the ISO key's < > |, the keypad), the layout on top.
		# pc never turns on AltGr, so only the layout decides that.
		s = Section()
		parse(symdir, "pc", None, s)
		s.altgr = False
		parse(symdir, fname, sect, s)
		# <KPDL>: KP_Separator is the comma, KP_Decimal the full stop.
		kp = ","
		if "KPDL" in s.keys:
			lv = s.keys["KPDL"]
			kp = "," if len(lv) > 1 and lv[1] in ("KP_Separator", "comma") else "."
		else:
			kp = "."
		display = find_name(symdir, fname, sect) or name
		if ime:
			display = {1: "Japanese (hiragana)", 2: "Japanese (katakana)"}[ime]
			if fname != "jp":
				display += ", " + (find_name(symdir, fname, sect) or fname) + " keys"

		entries = []
		for key, usages in XKB_TO_USAGE.items():
			if key not in s.keys:
				continue
			lv = (s.keys[key] + ["NoSymbol"] * 4)[:4]
			codes = [keysym_to_code(x, ks) for x in lv]
			if not s.altgr:
				codes[2] = codes[3] = 0
			# A single-level key (xkb lists one symbol) types it shifted
			# too -- the ONE_LEVEL key type.
			if len(s.keys[key]) == 1:
				codes[1] = codes[0]
			for u in usages:
				entries.append((u, codes))
			used.update(codes)
		entries.sort()

		cname = "kbd_" + name.replace("-", "_")
		# An input method's keys are its base layout's, byte for byte:
		# point at that table rather than emitting a copy.
		key = tuple((u, tuple(c)) for u, c in entries)
		if key in emitted:
			cname = emitted[key]
		else:
			emitted[key] = cname
			w(f"// {display} -- xkb {fname}" + (f"({sect})" if sect else ""))
			w(f"static const z_kbd_key_t {cname}_keys[] = {{")
			for u, codes in entries:
				vals = ", ".join(f"0x{c:04x}" for c in codes)
				comment = "  ".join(c_char_comment(c) for c in codes)
				w(f"\t{{ 0x{u:02x}, {{ {vals} }} }},\t// {comment}")
			w("};")
			w("")
		layout_rows.append((name, label, display, s.altgr, kp, cname, len(entries), ime))

	w("const z_kbd_layout_t z_kbd_layouts[] = {")
	for name, label, display, altgr, kp, cname, n, ime in layout_rows:
		w(f'\t{{ "{name}", "{label}", "{display}", {1 if altgr else 0}, \'{kp}\', {n}, {cname}_keys, {ime} }},')
	w("};")
	w("")
	w("const int z_kbd_layout_count = (int)(sizeof(z_kbd_layouts) / sizeof(z_kbd_layouts[0]));")
	w("")

	# Dead keys: spacing forms, and what each combines into.
	w("// Dead key n is Z_KBD_DEAD_BASE + n. Its spacing form (dead key then")
	w("// Space, or the dead key twice) and what it combines into.")
	w("const uint16_t z_kbd_dead_spacing[] = {")
	for i, (name, comb, spacing) in enumerate(DEAD_KEYS):
		w(f"\t0x{(spacing or 0):04x},\t// {name}")
	w("};")
	w(f"const int z_kbd_dead_count = {len(DEAD_KEYS)};")
	w("")
	rows = compose_table(used)
	w("// Sorted by (dead, base), for a binary search.")
	w("const z_kbd_compose_t z_kbd_compose_table[] = {")
	for d, b, r in rows:
		w(f"\t{{ 0x{d:04x}, 0x{b:04x}, 0x{r:04x} }},\t// {DEAD_KEYS[d - DEAD_BASE][0]} {chr(b)} -> {chr(r)}")
	w("};")
	w("const int z_kbd_compose_count = (int)(sizeof(z_kbd_compose_table) / sizeof(z_kbd_compose_table[0]));")

	OUT.write_text("\n".join(out) + "\n", encoding="utf-8")
	print(f"wrote {OUT}: {len(LAYOUTS)} layouts, {len(rows)} compositions")
	if UNKNOWN_DEAD:
		print("dead keys left empty (not in DEAD_KEYS):", " ".join(sorted(UNKNOWN_DEAD)))


if __name__ == "__main__":
	main()
