#!/bin/sh
#
# posix shell tests.
#
# Builds a small filesystem, runs tests/script.sh.txt through the shell
# against it, and compares the result with tests/expected.txt.
#
# A GOLDEN-FILE test, unlike zcc's differential suite, and the trade is
# the other way round: there is no second shell to compare against, so
# this catches regressions well and cannot catch a behaviour that was
# wrong from the first run. Which is why the transcript is meant to be
# READ when it changes, not just accepted -- `make test` failing is an
# invitation to look, not to re-record.
#
# What it exercises is sh.c and vfs.c, which contain no port, message
# or window code at all. main.c's loop is covered at the end, by
# run_main.c with a fake kernel; the terminal handoff still needs a
# machine.

set -e

here=$(cd "$(dirname "$0")" && pwd)
work="${TMPDIR:-/tmp}/posix-tests"
bin="$here/px_shell_test"

[ -x "$bin" ] || { echo "run 'make' in tests/ first" >&2; exit 1; }

rm -rf "$work"
mkdir -p "$work/src" "$work/docs"
printf 'hello from a file\nsecond line\n' > "$work/notes.txt"
printf 'int main(void){return 0;}\n' > "$work/src/a.c"
# 1..12, one per line -- the fixture the assertion cases count.
i=1; : > "$work/numbers.txt"
while [ $i -le 12 ]; do echo $i >> "$work/numbers.txt"; i=$((i + 1)); done

# For sort/uniq: deliberately unsorted, with an adjacent duplicate.
printf 'cherry\napple\nbanana\napple\n' > "$work/words.txt"

# One line of 699 characters plus a newline -- longer than PX_LINE_MAX,
# so `wc` must still count it exactly while the line filters report a cut.
awk 'BEGIN { s=""; while (length(s) < 699) s = s "x"; print s }' \
    > "$work/long.txt" 2>/dev/null || \
    python3 -c "print('x'*699)" > "$work/long.txt"
# Installed programs, in apps/ where the kernel finds a bare name --
# see host_fs.c's z_proc_run(). `zcc` has to EXIST for the run builtin
# to report success; `failprog` exits 1, so the failure arms of && and
# || are reachable.
mkdir -p "$work/apps"
printf 'not really a compiler\n' > "$work/apps/zcc"
printf 'exits nonzero\n' > "$work/apps/failprog"
# Programs of the person's own, for the working-directory search: a
# `hello` built in proj/, and a `zcc` there that hides the installed one.
mkdir -p "$work/proj"
printf 'built here\n' > "$work/proj/hello"
printf 'a zcc of my own\n' > "$work/proj/zcc"
# zcat, gunzip: notes.gz as real gzip writes it (with the file's name,
# which zinflate refuses unless asked -- zgz asks); bad.gz with its
# trailer's CRC flipped; cut.gz missing its last bytes.
gzip -c "$work/notes.txt" > "$work/notes.gz"
cp "$work/notes.gz" "$work/copy.gz"
python3 -c "
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[-8] ^= 1
open(sys.argv[2], 'wb').write(d)
open(sys.argv[3], 'wb').write(d[:-5])
" "$work/notes.gz" "$work/bad.gz" "$work/cut.gz"
python3 -c "
import sys
d = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(d[:-5])
" "$work/notes.gz" "$work/cut.gz"

# The output goes OUTSIDE the tree the shell can see. With it inside,
# `ls` listed the transcript being written, so the golden file
# contained its own filename and every change to the harness churned
# it.
out="${TMPDIR:-/tmp}/posix-actual.txt"
"$bin" "$work" < "$here/script.sh.txt" > "$out" 2>&1 || true

if [ -n "$PX_RECORD" ]; then
    cp "$out" "$here/expected.txt"
    echo "recorded $here/expected.txt -- READ IT before committing"
    exit 0
fi

if ! diff -u "$here/expected.txt" "$out"; then
    echo
    echo "posix shell output changed. If the new behaviour is right,"
    echo "re-record with: PX_RECORD=1 ./tests/run.sh"
    exit 1
fi

echo "posix shell: transcript matches"

# The assertion suite. Separate from the transcript above because it
# answers a different question: the transcript says "nothing changed",
# these say "the answers are right".
# Long file names (docs/posix.md, "Quoting" and "Wildcards"), made
# only now, after the transcript has run, so its `ls` of the root does
# not change, and in directories of their own: a name with a space, one in a directory with a space, a German
# one (built with printf -- sh does not expand \ooo in quotes), and
# three .c files and a .h for the wildcards.
mkdir -p "$work/names/Old Stuff" "$work/wild"
printf 'spaced out\n' > "$work/names/My Notes.txt"
printf 'inside\n' > "$work/names/Old Stuff/a b.txt"
printf 'gruss\n' > "$work/names/$(printf 'Gr\303\274\303\237e.txt')"
printf 'int a;\n' > "$work/wild/a.c"
printf 'int bb;\n' > "$work/wild/b.c"
printf 'int c;\n' > "$work/wild/c.c"
printf 'hdr\n' > "$work/wild/a.h"
# mv between volumes: host_fs.c treats /ram as a second volume, so a
# rename into it is refused and fs_move() copies and unlinks instead.
mkdir -p "$work/ram" "$work/mvdir"

if [ -x "$here/px_case_test" ]; then
    echo
    PX_STACK_FILE="$work/stack.txt" "$here/px_case_test" "$work" "$here/cases.txt" || exit 1
fi

# -- find, diff, unzip, tar (tools.c) --
#
# Their fixtures, made only now for the same reason as the names above.
# tools.txt asserts the answers; gen_tools.py adds cases whose expected
# output comes from the real thing -- GNU `diff -u` on pairs of files,
# Python's sorted walk for `find` -- so the comparison is with another
# implementation, not with what this one printed last time.
python3 "$here/gen_tools.py" "$work" "$work/gen_cases.txt" || exit 1
if [ -x "$here/px_case_test" ]; then
    echo
    PX_STACK_FILE="$work/stack.txt" "$here/px_case_test" "$work" "$here/tools.txt" || exit 1
    echo
    PX_STACK_FILE="$work/stack.txt" "$here/px_case_test" "$work" "$work/gen_cases.txt" || exit 1
fi

# The stack each command used in the coroutine (run_cases.c), deepest
# first: what PX_JOB_STACK is sized from (docs/posix.md, "The command's
# stack"). Host frames -- wider than the machine's.
if [ -s "$work/stack.txt" ]; then
    echo
    echo "command stack used (host, bytes), deepest per command:"
    sort -k2 -n -r "$work/stack.txt" | awk '!seen[$1]++' | head -40 |
        awk '{ printf "  %-8s %6d  ", $1, $2; $1 = ""; $2 = ""; sub(/^  /, ""); print }'
fi

# -- the main loop (run_main.c: the real main.c, a fake kernel) --
#
# Big inputs, so that a command takes many slices: big.txt about 1 MB
# (cat, grep, zcat ..., and in arch/huge.zip and huge.tgz), dbig*.txt for diff (inside its 256 KB and 8192
# lines), a directory of big files for cp -r.
python3 - "$work" <<'PY'
import gzip, os, random, sys
w = sys.argv[1]
r = random.Random(3)
lines = ["line %d %s" % (i, r.choice(["alpha", "beta", "gamma", "delta 7"])) for i in range(40000)]
open(os.path.join(w, "big.txt"), "w").write("\n".join(lines) + "\n")
open(os.path.join(w, "big.gz"), "wb").write(gzip.compress(open(os.path.join(w, "big.txt"), "rb").read()))
d = lines[:6000]
open(os.path.join(w, "dbig.txt"), "w").write("\n".join(d) + "\n")
d2 = list(d)
for k in range(0, 6000, 50):
    d2[k] = d2[k] + " changed"
open(os.path.join(w, "dbig2.txt"), "w").write("\n".join(d2) + "\n")
import io, tarfile, zipfile
data = open(os.path.join(w, "big.txt"), "rb").read()
with zipfile.ZipFile(os.path.join(w, "arch/huge.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("big.bin", data)
with tarfile.open(os.path.join(w, "arch/huge.tgz"), "w:gz") as t:
    ti = tarfile.TarInfo("big.bin")
    ti.size = len(data)
    t.addfile(ti, io.BytesIO(data))
os.makedirs(os.path.join(w, "bigdir"), exist_ok=True)
for k in range(4):
    open(os.path.join(w, "bigdir", "f%d.txt" % k), "w").write("\n".join(lines[:8000]) + "\n")
PY
if [ -x "$here/px_main_test" ]; then
    echo
    "$here/px_main_test" "$work" || exit 1
fi
