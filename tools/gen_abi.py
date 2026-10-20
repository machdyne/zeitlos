#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# The ABI fingerprint: one short id for "what a program on the card
# expects of what is in flash" (docs/update.md, "ABI").
#
# The kernel and the core apps (CORE_APPS: fs, wm, net, ...) are in
# flash; everything else runs from the card. `update` replaces the card
# and leaves flash alone, so a new card on an old flash works only if
# the two agree on the interfaces between them: the syscall numbers and
# their argument structs, the message envelope, the executable format,
# the address apps are linked at, and the protocols of the core apps.
# Those are the files in ABI_FILES below. This tool reduces them to
# what a compiled program depends on and hashes the result:
#
#   syscalls.def    the syscall NAMES in order (a syscall's number is
#                   its position). The kernel function that serves one
#                   is the kernel's business, so it is not part of it.
#   riscv-app.ld    every token, comments removed.
#   the headers     comments, #include lines and functions removed:
#                   prototypes and function definitions (static inline
#                   helpers) are code in the program, not an agreement
#                   with flash. What is left -- #define, typedef,
#                   struct, union, enum, data -- is compared token by
#                   token, so whitespace and comments never change it.
#
# The id is the first 16 hex digits of the SHA-256 of that, and it is
# COMMITTED, in sw/common/zabi.h, where the kernel (Z_SYS_ABI) and the
# release tool read it. The build fails when the committed id is not
# the one the sources have (sw/os/Makefile and sw/apps/Makefile run
# --check), and so does `zrelease check`: an interface change cannot
# reach a board, or a release, without its new id.
#
# A change that is compatible -- a syscall appended, a message added --
# still makes a new id. The check is "same interfaces", not "compatible
# interfaces": the second cannot be decided from text, and the cost of
# a false alarm is `update -f`.
#
# USAGE
#
#   tools/gen_abi.py            write sw/common/zabi.h (`make abi`)
#   tools/gen_abi.py --check    exit 1, saying so, if zabi.h is stale
#   tools/gen_abi.py --print    print the id the sources have
#   tools/gen_abi.py --dump     print the reduced text that is hashed

import hashlib
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
COMMON = "sw/common"
OUT = os.path.join(COMMON, "zabi.h")

# What a card program shares with flash. Paths relative to the root.
ABI_FILES = [
	# -- the kernel: syscalls, their arguments, what it loads --
	"sw/common/syscalls.def",
	"sw/common/riscv-app.ld",
	"sw/common/zeitlos.h",
	"sw/common/zobj.h",
	"sw/common/zmsg.h",
	"sw/common/zproc.h",
	"sw/common/zexec.h",
	"sw/common/zauth.h",
	"sw/common/zcfg.h",
	"sw/common/zconsole.h",
	"sw/common/zflash.h",
	"sw/common/zfs.h",
	"sw/common/zfsapp.h",
	"sw/common/zkv.h",
	# -- the core apps' protocols --
	"sw/common/zsubjects.h",
	"sw/common/zfsproto.h",
	"sw/common/znfsproto.h",
	"sw/common/zwm.h",
	"sw/common/znet.h",
	"sw/common/zlisten.h",
	"sw/common/zterm.h",
	"sw/common/zusbproto.h",
	"sw/common/zusbcdc.h",
]

ID_LEN = 16

TOKEN_RE = re.compile(r"""
	  (?P<ws>\s+)
	| (?P<lc>//[^\n]*)
	| (?P<bc>/\*.*?\*/)
	| (?P<str>"(?:\\.|[^"\\\n])*")
	| (?P<chr>'(?:\\.|[^'\\\n])*')
	| (?P<id>[A-Za-z_][A-Za-z0-9_]*)
	| (?P<num>\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.])*)
	| (?P<op>->|\+\+|--|<<=?|>>=?|[<>=!&|^+\-*/%]=|&&|\|\||\#\#|\.\.\.|.)
""", re.S | re.X)


class AbiError(Exception):
	pass


def tokens(text):
	"""C tokens, comments and whitespace dropped."""
	out = []
	pos = 0
	while pos < len(text):
		m = TOKEN_RE.match(text, pos)
		if not m:
			raise AbiError("cannot tokenize at %r" % text[pos:pos + 20])
		pos = m.end()
		if m.lastgroup in ("ws", "lc", "bc"):
			continue
		out.append(m.group())
	return out


def split_pp(text):
	"""(kind, text) items in order: 'pp' for a preprocessor line
	(continuations joined), 'c' for a run of C between them."""
	items = []
	buf = []
	lines = text.split("\n")
	i = 0
	in_comment = False
	while i < len(lines):
		line = lines[i]
		s = line.lstrip()
		if not in_comment and s.startswith("#"):
			while line.endswith("\\") and i + 1 < len(lines):
				i += 1
				line = line[:-1] + " " + lines[i]
			if buf:
				items.append(("c", "\n".join(buf)))
				buf = []
			items.append(("pp", line))
		else:
			buf.append(line)
			# track /* ... */ spanning lines, so a '#' in a comment is
			# not taken for a directive
			for m in re.finditer(r"//[^\n]*|/\*|\*/|\"(?:\\.|[^\"\\])*\"", line):
				t = m.group()
				if t == "/*" and not in_comment:
					in_comment = True
				elif t == "*/" and in_comment:
					in_comment = False
				elif t.startswith("//") and not in_comment:
					break
		i += 1
	if buf:
		items.append(("c", "\n".join(buf)))
	return items


ATTR = {"__attribute__", "__attribute", "__aligned", "__packed"}
TYPEKW = {"struct", "union", "enum"}


def _is_function(decl):
	"""decl: the tokens of one top-level declaration so far. A function
	(prototype or definition) when an identifier other than an attribute
	is followed by '(' before any '=' or a struct/union/enum body, and it
	is not a typedef."""
	if not decl or decl[0] == "typedef":
		return False
	for i, t in enumerate(decl):
		if t in ("=", "{"):
			return False
		if t == "(" and i > 0:
			p = decl[i - 1]
			if p in ATTR:
				return False
			return bool(re.match(r"[A-Za-z_]", p))
	return False


def reduce_header(text):
	"""What of a header a compiled program depends on, as lines."""
	out = []
	decl = []		# the current top-level declaration: tokens and
	depth = 0		# preprocessor lines met inside it
	skip = 0		# >0: inside a function body being dropped
	for kind, chunk in split_pp(text):
		if kind == "pp":
			if skip:
				continue
			t = tokens(chunk)
			if t[:2] == ["#", "include"]:
				continue
			line = " ".join(t)
			if decl:
				decl.append("\n" + line + "\n")
			else:
				out.append(line)
			continue
		for t in tokens(chunk):
			if skip:
				if t == "{":
					skip += 1
				elif t == "}":
					skip -= 1
				continue
			if depth == 0 and t == "{":
				toks = [x for x in decl if not x.startswith("\n")]
				if _is_function(toks):
					# a definition: drop it, body and all; the
					# preprocessor lines met in its head are kept
					out.extend(x.strip() for x in decl if x.startswith("\n"))
					decl = []
					skip = 1
					continue
			decl.append(t)
			if t in "{([":
				depth += 1
			elif t in "})]":
				depth -= 1
			elif t == ";" and depth == 0:
				toks = [x for x in decl if not x.startswith("\n")]
				if _is_function(toks):
					out.extend(x.strip() for x in decl if x.startswith("\n"))
				else:
					out.append(" ".join(decl).replace(" \n ", "\n").strip())
				decl = []
	if skip or depth:
		raise AbiError("unbalanced braces")
	if decl:
		out.append(" ".join(decl).strip())
	return [l for l in "\n".join(out).split("\n") if l.strip()]


def reduce_syscalls(text):
	names = re.findall(r"^\s*Z_MKSYSCALL\s*\(\s*([A-Za-z0-9_]+)\s*,", text, re.M)
	if not names:
		raise AbiError("no Z_MKSYSCALL lines")
	return ["%d %s" % (i, n) for i, n in enumerate(names)]


def reduce_file(path, text):
	if path.endswith(".def"):
		return reduce_syscalls(text)
	if path.endswith(".ld"):
		return [" ".join(tokens(text))]
	return reduce_header(text)


def reduced(root=ROOT):
	parts = []
	for rel in ABI_FILES:
		p = os.path.join(root, rel)
		try:
			with open(p, encoding="utf-8") as f:
				text = f.read()
		except OSError as e:
			raise AbiError("%s: %s" % (rel, e.strerror))
		try:
			lines = reduce_file(rel, text)
		except AbiError as e:
			raise AbiError("%s: %s" % (rel, e))
		parts.append("== %s\n" % rel + "\n".join(lines) + "\n")
	return "".join(parts)


def abi_id(root=ROOT):
	return hashlib.sha256(reduced(root).encode()).hexdigest()[:ID_LEN]


ID_RE = re.compile(r'^#define\s+Z_ABI_ID\s+"([0-9a-f]+)"\s*$', re.M)


def committed_id(root=ROOT):
	"""The id sw/common/zabi.h says, or None."""
	try:
		with open(os.path.join(root, OUT), encoding="utf-8") as f:
			m = ID_RE.search(f.read())
	except OSError:
		return None
	return m.group(1) if m else None


HEADER = """#ifndef ZABI_H
#define ZABI_H

/*
 * zabi.h -- GENERATED by tools/gen_abi.py (`make abi`). Do not edit.
 *
 * The ABI fingerprint: an id for the interfaces between what is in
 * flash (the kernel, the core apps) and the programs on the card --
 * the syscalls, their argument structs, the message envelope, the
 * executable format, the core apps' protocols. tools/gen_abi.py lists
 * the files and says what of them counts.
 *
 * The kernel reports it (Z_SYS_ABI, z_abi()); each release records the
 * id it was built with in its manifest; `update` compares the two and
 * refuses a card the flash does not match (docs/update.md, "ABI").
 *
 * Committed, and checked: the build and `zrelease check` fail when it
 * is not the id the sources have. After changing an interface, run
 * `make abi` and commit the result with the change.
 */

#define Z_ABI_ID "%s"

#endif
"""


def main(argv):
	mode = argv[1] if len(argv) > 1 else "--write"
	try:
		if mode == "--dump":
			sys.stdout.write(reduced())
			return 0
		new = abi_id()
		if mode == "--print":
			print(new)
			return 0
		if mode == "--check":
			old = committed_id()
			if old == new:
				print("gen_abi: %s is current (%s)" % (OUT, new))
				return 0
			sys.stderr.write(
				"gen_abi: %s is stale: it says %s, the sources say %s.\n"
				"gen_abi: an interface between flash and the card changed "
				"(tools/gen_abi.py, docs/update.md \"ABI\"); run `make abi` "
				"and commit sw/common/zabi.h with the change.\n"
				% (OUT, old or "nothing", new))
			return 1
		if mode == "--write":
			text = HEADER % new
			p = os.path.join(ROOT, OUT)
			try:
				with open(p, encoding="utf-8") as f:
					same = f.read() == text
			except OSError:
				same = False
			if not same:
				with open(p, "w", encoding="utf-8") as f:
					f.write(text)
			print("gen_abi: %s %s (%s)" % (OUT, "unchanged" if same else "written", new))
			return 0
	except AbiError as e:
		sys.stderr.write("gen_abi: %s\n" % e)
		return 1
	sys.stderr.write(__doc__ if __doc__ else "usage: gen_abi.py [--check|--print|--dump]\n")
	return 2


if __name__ == "__main__":
	sys.exit(main(sys.argv))
