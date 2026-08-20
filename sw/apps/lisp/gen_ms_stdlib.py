#!/usr/bin/env python3
"""
Generates sw/apps/lisp/ms_stdlib.h from sw/ext/ms/ms_stdlib.l (the
`ms` submodule's own standard library source).

Building Machdyne Scheme with -DLIX removes all file I/O -- no
fopen()/load, per ms.c's own top-of-file build comment and its
README's "Embedding" section -- so the standard library has to be
compiled in as a plain C string constant instead of loaded from disk
at runtime. ms's own README documents the exact expected shape:

    static const char ms_stdlib_l[] = "...(stdlib source)...";

which is all this script produces: read ms_stdlib.l, C-escape it, and
write that one line out. Nothing Zeitlos-specific about the format --
this is upstream's own expected embedding contract, not something we
invented.

Regenerated automatically by sw/apps/lisp/Makefile (a build
dependency on both this script and sw/ext/ms/ms_stdlib.l -- see that
Makefile) whenever the submodule's stdlib source is newer than the
last generated header, so `git submodule update` pulling in an
upstream stdlib change doesn't silently leave a stale ms_stdlib.h
behind. Not committed to the repo for that same reason -- regenerate,
don't hand-edit or check in the output.

Usage:
    python3 gen_ms_stdlib.py <path/to/ms_stdlib.l> <path/to/ms_stdlib.h>
"""

import sys
from pathlib import Path


def c_escape(text: str) -> str:
	out = []
	for ch in text:
		if ch == '\\':
			out.append('\\\\')
		elif ch == '"':
			out.append('\\"')
		elif ch == '\n':
			out.append('\\n"\n\t"')	# split into adjacent string
										# literals per source line --
										# purely cosmetic (a compiler
										# concatenates them into one
										# string same as if it were
										# all on one line), but keeps
										# the generated header from
										# being one single
										# multi-kilobyte source line,
										# which is friendlier to diff/
										# review if anyone ever needs
										# to actually look at it
		elif ch == '\t':
			out.append('\\t')
		elif ord(ch) < 0x20 or ord(ch) > 0x7e:
			out.append('\\x%02x' % ord(ch))
		else:
			out.append(ch)
	return ''.join(out)


def main():
	if len(sys.argv) != 3:
		print(f"usage: {sys.argv[0]} <ms_stdlib.l> <ms_stdlib.h>", file=sys.stderr)
		sys.exit(1)

	src_path = Path(sys.argv[1])
	out_path = Path(sys.argv[2])

	src = src_path.read_text()
	escaped = c_escape(src)

	out_path.write_text(
		"/* GENERATED FILE -- do not edit by hand.\n"
		f" * Produced by {Path(__file__).name} from {src_path.name}\n"
		" * (sw/ext/ms, a git submodule -- see sw/apps/lisp/Makefile). */\n\n"
		f'static const char ms_stdlib_l[] =\n\t"{escaped}";\n'
	)

	print(f"wrote {out_path} ({len(src)} bytes of stdlib source, "
		f"{out_path.stat().st_size} bytes generated)")


if __name__ == "__main__":
	main()
