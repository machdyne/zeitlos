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
# or window code at all. main.c's plumbing is not covered here and
# needs a machine.

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
# `zcc` has to EXIST for the run builtin to report success, since the
# stub checks for it -- see host_fs.c's z_proc_run().
printf 'not really a compiler\n' > "$work/zcc"
# Exits 1, so the failure arms of && and || are reachable -- see
# host_fs.c's z_proc_run().
printf 'exits nonzero\n' > "$work/failprog"

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
if [ -x "$here/px_case_test" ]; then
    echo
    "$here/px_case_test" "$work" "$here/cases.txt" || exit 1
fi
