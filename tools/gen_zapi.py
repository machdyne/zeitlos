#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# Makes the Scheme API reference -- every procedure repl has -- from the
# entries beside the code, so it cannot drift from what repl really has:
#
#   sw/common/zapi/zapi.c          the Zeitlos procedures, `//:` entries
#   sw/common/zapi/zapi_scheme.txt the Scheme language: ms's builtins,
#                                special forms and standard library
#
# into
#
#   zapi.txt      plain text, in flash at /docs/zapi.txt: what (help) and
#                 (apropos) read (sw/common/zapi/zapi_doc.c)
#   docs/zapi.md  the same as a page, committed, with the OS version and
#                 the date
#
# It FAILS, naming them, on a name repl binds that has no entry, and on
# an entry for a name repl does not bind. The names come from the code,
# not from the entries: ms_def_builtin() calls and ms_load_string()
# definitions in zapi_register(); ms.c's def_builtin() calls as built
# with -DLIX (through the C preprocessor, since a third of them are
# compiled out); its special forms (sym_* = mk_sym(...)); and every
# top-level define in ms_stdlib.l. Names starting with % are internal.
#
# USAGE
#
#   tools/gen_zapi.py --txt OUT.txt            write zapi.txt
#   tools/gen_zapi.py --md docs/zapi.md        write the page (keeps its
#                                              date if nothing else changed)
#   tools/gen_zapi.py --check-md docs/zapi.md  exit 1 if the page is stale
#   tools/gen_zapi.py --run-examples MS        run the Scheme-language
#                                              examples through a desktop
#                                              build of ms, and compare
#
# The entry format is in docs/scheme_api.md.

import argparse
import datetime
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ZAPI_C = "sw/common/zapi/zapi.c"
ZAPI_SCHEME = "sw/common/zapi/zapi_scheme.txt"
MS_C = "sw/ext/ms/ms.c"
MS_STDLIB = "sw/ext/ms/ms_stdlib.l"
VERSION_H = "sw/common/zversion.h"

# Sections in the order they are printed. A section not here is an
# error, so a new one is a decision about where it goes.
SECTIONS = [
	"Help",
	"Syntax", "Numbers", "Equality and types", "Lists", "Strings",
	"Characters", "Vectors", "Output", "Evaluation",
	"Files", "Windows and apps", "Display", "Gamepads",
	"Processes and messages", "Network", "Time and memory",
	"GPIO and LEDs", "I2C and SPI", "UART1", "Random numbers",
]

LINE_MAX = 100		# zapi_doc.h's ZDOC_LINE is 128
HEAD_MAX = 480		# the call shapes of one entry: zdoc_help()'s buffer is 512


class Entry:
	def __init__(self, section, where):
		self.section = section
		self.where = where			# "file:line", for messages
		self.shapes = []			# "(name args)"
		self.desc = []				# lines of description
		self.examples = []			# (expr, result or None)

	def names(self):
		out = []
		for s in self.shapes:
			n = shape_name(s)
			if n not in out:
				out.append(n)
		return out


def internal(name):
	"""%win-event and the like; `%` itself is the remainder."""
	return len(name) > 1 and name.startswith("%")


def shape_name(shape):
	m = re.match(r"\(([^\s()]+)", shape)
	return m.group(1) if m else None


def fail(msg):
	sys.stderr.write("gen_zapi: %s\n" % msg)
	sys.exit(1)


def read(rel):
	with open(os.path.join(ROOT, rel)) as f:
		return f.read()


# -- the entries --

def parse_entries(rel, prefix):
	"""Entries from lines starting with `prefix` + "@" / ":"; in zapi.c
	the prefix is "//" and an entry also ends at any other line."""

	entries = []
	section = None
	cur = None
	last_kind = None

	for n, raw in enumerate(read(rel).split("\n"), 1):
		line = raw.strip() if prefix == "//" else raw.rstrip()
		where = "%s:%d" % (rel, n)

		if line.startswith(prefix + "@"):
			section = line[len(prefix) + 1:].strip()
			if section not in SECTIONS:
				fail("%s: section '%s' is not in SECTIONS (tools/gen_zapi.py)" % (where, section))
			cur = None
			continue

		if not line.startswith(prefix + ":"):
			cur = None
			last_kind = None
			continue

		body = line[len(prefix) + 1:]
		if body.startswith(" ("):
			if cur is None or last_kind != "shape":
				if section is None:
					fail("%s: an entry before any @ section line" % where)
				cur = Entry(section, where)
				entries.append(cur)
			cur.shapes.append(body[1:])
			last_kind = "shape"
		elif body.startswith("   "):
			if cur is None:
				fail("%s: a description with no call shape above it" % where)
			text = body[3:]
			if text.startswith("> "):
				ex = text[2:]
				if "  =>  " in ex:
					e, r = ex.split("  =>  ", 1)
					cur.examples.append((e.strip(), r.strip()))
				else:
					cur.examples.append((ex.strip(), None))
				last_kind = "example"
			else:
				cur.desc.append(text)
				last_kind = "desc"
		elif body.strip() == "":
			cur = None
			last_kind = None
		else:
			fail("%s: expected ': (call shape)' or ':   text' -- got '%s'" % (where, raw.strip()))

	for e in entries:
		if not e.desc:
			fail("%s: %s has no description" % (e.where, e.shapes[0]))
		if not e.examples:
			fail("%s: %s has no example" % (e.where, e.shapes[0]))
		if any(n is None for n in e.names()):
			fail("%s: a call shape that names nothing" % e.where)
		if sum(len(s) + 1 for s in e.shapes) > HEAD_MAX:
			fail("%s: too many call shapes for (help) to hold" % e.where)

	return entries


def hidden_names(rel):
	out = set()
	for line in read(rel).split("\n"):
		if line.startswith("~"):
			out.update(line[1:].split())
	return out


# -- the names repl really binds --

def zapi_names():
	src = read(ZAPI_C)
	m = re.search(r"void zapi_register\(void\) \{(.*?)\n\}", src, re.S)
	if not m:
		fail("%s: no zapi_register()" % ZAPI_C)
	body = m.group(1)
	order = []
	for mm in re.finditer(r'ms_def_builtin\("([^"]+)"|\(define \(([^\s()]+)|\(define ([^\s()]+)', body):
		name = mm.group(1) or mm.group(2) or mm.group(3)
		if name not in order:
			order.append(name)
	return [n for n in order if not internal(n)]


def ms_names(cpp):
	cmd = cpp.split() + ["-E", "-P", "-DLIX", "-DMS_STATIC_HEAP",
		"-DMS_HEAP_SIZE=1000", "-DMS_PROTECT_STACK_SIZE=100",
		"-I" + os.path.join(ROOT, "sw/common/ms_platform"),
		"-I" + os.path.join(ROOT, "sw/apps/repl"),
		os.path.join(ROOT, MS_C)]
	try:
		out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
	except (OSError, subprocess.CalledProcessError) as e:
		fail("could not preprocess %s with '%s': %s" % (MS_C, cpp, e))
	builtins = re.findall(r'def_builtin\(env, "([^"]+)"', out)
	if len(builtins) < 50:
		fail("only %d builtins found in %s -- has the submodule moved on?" % (len(builtins), MS_C))
	forms = re.findall(r'sym_\w+ = mk_sym\("([^"]+)"\)', read(MS_C))
	# and the constants: nil t true false
	forms += re.findall(r'env_define\(ms_global_env, mk_sym\("([^"]+)"\)', read(MS_C))
	return builtins, forms


def stdlib_names():
	return re.findall(r'^\(define \(?([^\s()]+)', read(MS_STDLIB), re.M)


# -- output --

def version():
	m = re.search(r'#define\s+Z_OS_VERSION\s+"([^"]+)"', read(VERSION_H))
	if not m:
		fail("%s: no Z_OS_VERSION" % VERSION_H)
	return m.group(1)


def ordered(entries, zorder):
	"""By section, in SECTIONS order; within one, the Scheme language
	first in its file's order, then the Zeitlos procedures in the
	order zapi_register() registers them."""
	rank = {n: i for i, n in enumerate(zorder)}
	def key(ie):
		i, e = ie
		zr = min((rank[n] for n in e.names() if n in rank), default=None)
		return (SECTIONS.index(e.section), 0 if zr is None else 1, zr if zr is not None else i)
	return [e for _, e in sorted(enumerate(entries), key=key)]


def example_text(ex):
	e, r = ex
	return "  > %s  =>  %s" % (e, r) if r is not None else "  > %s" % e


def make_txt(entries, ver):
	out = []
	out.append("ZAPI -- every procedure in repl, Zeitlos %s" % ver)
	out.append("")
	out.append("help name shows one entry; apropos text searches them all.")
	out.append("How to use repl and Scheme: /docs/repl.txt. Made by tools/gen_zapi.py.")
	out.append("Each entry: how to call it, what it does, and an example, with")
	out.append("what the example returns after =>.")
	section = None
	for e in entries:
		if e.section != section:
			section = e.section
			out.append("")
			out.append("== %s ==" % section)
		out.append("")
		out.extend(e.shapes)
		out.extend("  " + d for d in e.desc)
		out.extend(example_text(x) for x in e.examples)
	out.append("")
	for n, line in enumerate(out, 1):
		if len(line) > LINE_MAX:
			fail("zapi.txt line %d is %d characters (most %d): %s" % (n, len(line), LINE_MAX, line))
	return "\n".join(out)


DATE_RE = re.compile(r"^(\*\*Zeitlos [^*]+\*\* · )\d{4}-\d{2}-\d{2}", re.M)


def md_anchor(s):
	return re.sub(r"[^a-z0-9 -]", "", s.lower()).replace(" ", "-")


def make_md(entries, ver, date):
	out = []
	out.append("# ZAPI: the Scheme procedures in repl")
	out.append("")
	out.append("**Zeitlos %s** · %s" % (ver, date))
	out.append("")
	out.append("Every procedure `repl` has, Scheme's and the system's. Generated by "
		"`tools/gen_zapi.py` from the entries in `sw/common/zapi/zapi.c` and "
		"`sw/common/zapi/zapi_scheme.txt` (`make zapi`); edit those, not this. "
		"The same list is in flash as `/docs/zapi.txt`, where `help name` and "
		"`apropos text` look things up. [repl.md](repl.md) is the guide to "
		"Scheme and repl; [scheme_api.md](scheme_api.md) is how procedures "
		"are added.")
	out.append("")
	out.append("In each example, `=>` is what it returns.")
	out.append("")
	by = []
	for e in entries:
		if not by or by[-1][0] != e.section:
			by.append((e.section, []))
		by[-1][1].append(e)
	out.append("| Section | Procedures |")
	out.append("|---|---|")
	for s, es in by:
		names = []
		for e in es:
			for n in e.names():
				if n not in names:
					names.append(n)
		out.append("| [%s](#%s) | %s |" % (s, md_anchor(s),
			" ".join("`%s`" % n.replace("|", "\\|") for n in names)))
	for s, es in by:
		out.append("")
		out.append("## %s" % s)
		for e in es:
			out.append("")
			out.append("```")
			out.extend(e.shapes)
			out.append("```")
			out.append("")
			out.append(" ".join(e.desc))
			out.append("")
			out.append("```")
			out.extend(example_text(x)[2:] for x in e.examples)
			out.append("```")
	out.append("")
	return "\n".join(out)


def md_body(text):
	return DATE_RE.sub(r"\1", text)


# -- checks --

def check_complete(entries, hidden, cpp):
	zorder = zapi_names()
	builtins, forms = ms_names(cpp)
	real = set(zorder) | set(builtins) | set(forms) | set(stdlib_names())
	real = {n for n in real if not internal(n)}

	documented = {}
	for e in entries:
		for n in e.names():
			if n in documented:
				fail("%s: %s already has an entry, at %s" % (e.where, n, documented[n]))
			documented[n] = e.where

	problems = []
	for n in sorted(real - set(documented) - hidden):
		problems.append("no entry for %s" % n)
	for n in sorted(set(documented) - real):
		problems.append("%s: an entry for %s, which repl does not have" % (documented[n], n))
	for n in sorted(hidden - real):
		problems.append("%s: hides %s, which repl does not have" % (ZAPI_SCHEME, n))
	for n in sorted(hidden & set(documented)):
		problems.append("%s is both hidden and documented" % n)
	if problems:
		fail("the reference does not match repl:\n  " + "\n  ".join(problems))
	return zorder


def run_examples(entries, ms):
	"""Runs every Scheme-language example with a `=>` value through a
	desktop ms with the standard library loaded, one session in file
	order, and compares what it prints."""
	exs = [x for e in entries if e.where.startswith(ZAPI_SCHEME) for x in e.examples]
	src = '(load "%s")\n' % os.path.join(ROOT, MS_STDLIB)
	src += "".join(x[0] + "\n" for x in exs)
	out = subprocess.run([ms], input=src, capture_output=True, text=True).stdout
	results = out.split("ms> ")[2:]		# the banner, then (load ...)'s
	bad = 0
	for (e, r), got in zip(exs, results):
		got = got.strip()
		if r is not None and not got.endswith(r):
			print("MISMATCH: %s\n  expected %s\n  got      %s" % (e, r, got))
			bad += 1
	if len(results) < len(exs):
		print("ms stopped after %d of %d examples" % (len(results), len(exs)))
		bad += 1
	print("%d examples, %d with values, %d wrong" % (len(exs), sum(1 for x in exs if x[1] is not None), bad))
	return bad == 0


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--txt")
	ap.add_argument("--md")
	ap.add_argument("--check-md")
	ap.add_argument("--run-examples", metavar="MS")
	ap.add_argument("--cpp", default=os.environ.get("ZAPI_CPP", "gcc"),
		help="C compiler to preprocess ms.c with (default gcc, or $ZAPI_CPP)")
	a = ap.parse_args()

	entries = parse_entries(ZAPI_SCHEME, "") + parse_entries(ZAPI_C, "//")
	zorder = check_complete(entries, hidden_names(ZAPI_SCHEME), a.cpp)
	entries = ordered(entries, zorder)
	ver = version()

	if a.txt:
		os.makedirs(os.path.dirname(os.path.abspath(a.txt)), exist_ok=True)
		with open(a.txt, "w") as f:
			f.write(make_txt(entries, ver))

	if a.md or a.check_md:
		path = a.md or a.check_md
		old = open(path).read() if os.path.exists(path) else ""
		new = make_md(entries, ver, datetime.date.today().isoformat())
		same = md_body(old) == md_body(new)
		if a.check_md:
			if not same:
				fail("%s is stale: run `make zapi` and commit it" % path)
		elif not same:
			with open(path, "w") as f:
				f.write(new)
			print("gen_zapi: wrote %s" % path)

	if a.run_examples and not run_examples(entries, a.run_examples):
		sys.exit(1)

	n = sum(len(e.names()) for e in entries)
	print("gen_zapi: %d names in %d entries" % (n, len(entries)))


if __name__ == "__main__":
	main()
