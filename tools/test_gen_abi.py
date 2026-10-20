#!/usr/bin/env python3
#
# Zeitlos -- tests for tools/gen_abi.py, the ABI fingerprint.
#
#   python3 tools/test_gen_abi.py
#
# Copies the ABI files into a scratch tree, changes them the ways people
# do, and checks which changes make a new id: an interface change must,
# a comment, a reformat or a new helper function must not.

import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True

import gen_abi   # noqa: E402

FAILS = []


def check(cond, what):
	print("  %s  %s" % ("ok  " if cond else "FAIL", what))
	if not cond:
		FAILS.append(what)


def scratch():
	r = tempfile.mkdtemp(prefix="abi-")
	for rel in gen_abi.ABI_FILES + [gen_abi.OUT]:
		d = os.path.join(r, os.path.dirname(rel))
		os.makedirs(d, exist_ok=True)
		shutil.copyfile(os.path.join(gen_abi.ROOT, rel), os.path.join(r, rel))
	return r


def edit(root, rel, old, new):
	p = os.path.join(root, rel)
	s = open(p).read()
	assert old in s, "%s: %r not found" % (rel, old)
	open(p, "w").write(s.replace(old, new, 1))


def append(root, rel, text):
	with open(os.path.join(root, rel), "a") as f:
		f.write(text)


def main():
	print("the committed id")
	check(gen_abi.committed_id() == gen_abi.abi_id(),
		  "sw/common/zabi.h is current (else: make abi)")

	base = gen_abi.abi_id()
	cases_same = [
		("a comment", "sw/common/zproc.h", lambda r: append(r, "sw/common/zproc.h",
			"\n// a new comment\n/* and\n   another */\n")),
		("whitespace", "sw/common/zproc.h", lambda r: edit(r, "sw/common/zproc.h",
			"uint32_t\tserial;", "uint32_t      serial ;")),
		("a new prototype", "sw/common/zeitlos.h", lambda r: edit(r, "sw/common/zeitlos.h",
			"uint32_t z_proc_serial(uint32_t pid);",
			"uint32_t z_proc_serial(uint32_t pid);\nint z_helper(int a, char *b);")),
		("a new static inline helper", "sw/common/zwm.h", lambda r: edit(r, "sw/common/zwm.h",
			"#endif", "static inline int z_wm_helper(int x) {\n\tif (x) { return 1; }\n\treturn 0;\n}\n#endif")),
		("an #include", "sw/common/zmsg.h", lambda r: edit(r, "sw/common/zmsg.h",
			'#include "zobj.h"', '#include "zobj.h"\n#include <string.h>')),
		("the kernel function behind a syscall", "sw/common/syscalls.def",
			lambda r: edit(r, "sw/common/syscalls.def", "Z_MKSYSCALL(ABI, k_abi)",
						   "Z_MKSYSCALL(ABI, k_abi_renamed)")),
		("a '#' inside a block comment", "sw/common/zobj.h", lambda r: append(r,
			"sw/common/zobj.h", "\n/*\n#define NOT_A_DEFINE 1\n*/\n")),
	]
	cases_new = [
		("a struct field", lambda r: edit(r, "sw/common/zproc.h",
			"uint32_t\tserial;", "uint32_t\tserial;\n\tuint32_t\textra;")),
		("a #define's value", lambda r: edit(r, "sw/common/zproc.h",
			"#define Z_ARENA_E_NOMEM\t\t(-2)", "#define Z_ARENA_E_NOMEM\t\t(-4)")),
		("a syscall appended", lambda r: append(r, "sw/common/syscalls.def",
			"\nZ_MKSYSCALL(NEWONE, k_newone)\n")),
		("two syscalls swapped", lambda r: edit(r, "sw/common/syscalls.def",
			"Z_MKSYSCALL(ARENA, k_arena)", "Z_MKSYSCALL(ABI2, k_arena)")),
		("the app link address", lambda r: edit(r, "sw/common/riscv-app.ld",
			"0x80000000", "0x80001000")),
		("a typedef'd function pointer", lambda r: edit(r, "sw/common/zeitlos.h",
			"typedef uint32_t *(*z_kernel_ptr_t)(uint32_t, uint32_t *, uint32_t);",
			"typedef uint32_t *(*z_kernel_ptr_t)(uint32_t, uint32_t *);")),
	]

	print("changes that keep the id")
	for what, _rel, fn in cases_same:
		r = scratch()
		try:
			fn(r)
			check(gen_abi.abi_id(r) == base, what)
		finally:
			shutil.rmtree(r)

	print("changes that make a new id")
	for what, fn in cases_new:
		r = scratch()
		try:
			fn(r)
			check(gen_abi.abi_id(r) != base, what)
			check(gen_abi.committed_id(r) != gen_abi.abi_id(r),
				  "  ... and the committed one is then stale")
		finally:
			shutil.rmtree(r)

	print("the reduction")
	text = gen_abi.reduced()
	check("z_proc_serial (" not in text and "z_abi (" not in text,
		  "prototypes are not in it")
	check("z_proc_serial_args_t" in text and "z_abi_args_t" in text,
		  "the syscall argument structs are")
	check("\n%d ABI\n" % (text.split("== sw/common/riscv-app.ld")[0].count("\n") - 2) in text,
		  "the ABI syscall is last in syscalls.def")

	if FAILS:
		print("\n%d FAILED" % len(FAILS))
		return 1
	print("\nall passed")
	return 0


if __name__ == "__main__":
	sys.exit(main())
