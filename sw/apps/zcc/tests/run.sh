#!/bin/sh
#
# zcc test suite.
#
# Every test is compiled TWICE -- once with zcc, once with the RISC-V
# GCC the tree already uses -- and both images are run under
# sim/zsim-headless. The test passes when the two outputs are
# identical.
#
# Differential rather than golden-file testing, deliberately. A golden
# file records what zcc did on the day it was written, which makes it
# excellent at catching regressions and useless at catching a bug that
# was there from the start. GCC does not share zcc's bugs, so a
# disagreement is a real one -- and when a test is wrong, both compilers
# agree and the test silently passes, which is the failure mode worth
# accepting.
#
# Set ZCC_NO_REF=1 to skip the reference build (no RISC-V toolchain
# available); the tests then only check that zcc produces something
# that runs to completion, which is much weaker but better than
# nothing.

set -e

here=$(cd "$(dirname "$0")" && pwd)
zcc="$here/../zcc"
sim="$here/../../../../sim/zsim-headless"
work="${TMPDIR:-/tmp}/zcc-tests"

RISCV=${RISCV:-riscv64-unknown-elf-}
mkexec="$here/../../../../tools/mkexec.py"

rm -rf "$work"
mkdir -p "$work/root"

if [ ! -x "$zcc" ]; then
    echo "no zcc at $zcc -- run 'make -f Makefile.host' first" >&2
    exit 1
fi
if [ ! -x "$sim" ]; then
    echo "no simulator at $sim -- run 'make' in sim/ first" >&2
    exit 1
fi

have_ref=1
[ -n "$ZCC_NO_REF" ] && have_ref=0
command -v "${RISCV}gcc" >/dev/null 2>&1 || have_ref=0
[ "$have_ref" = 0 ] && echo "note: no reference compiler, comparing against nothing"

pass=0
fail=0

# t*.c are freestanding: no runtime, zcc emits its own entry stub.
# l*.c link the libz runtime blob, which is a different code path on
# both sides -- zcc prepends the blob and patches it, and the reference
# build links libz's objects with its own start stub. Both are worth
# testing; the freestanding path is what the compiler falls back to and
# what most of the feature tests use.
for src in "$here"/t*.c "$here"/l*.c; do
    name=$(basename "$src" .c)
    case "$name" in
        l*) uselibz=1 ;;
        *)  uselibz=0 ;;
    esac

    # No -U__riscv any more. sw/common/zeitlos.h now has a __zcc__ arm
    # for maskirq (the one function with inline assembly in it), so the
    # header compiles as-is and the target-correct branch is taken.
    # Passing -U__riscv would silently select the host stub that
    # returns 0.
    zccargs="-I $here -I $here/../include -I $here/../../../common"
    if [ "$uselibz" = 1 ]; then
        if [ ! -f "$here/../libz/libz.bin" ]; then
            echo "skip $name: no libz blob (make -C ../libz)"
            continue
        fi
        zccargs="$zccargs -I $here/../libz -L $here/../libz"
    else
        zccargs="$zccargs -nolibz"
    fi

    rm -rf "$work/root"; mkdir -p "$work/root"

    if ! "$zcc" $zccargs -o "$work/$name.zcc.bin" "$src" 2> "$work/$name.zcc.err"; then
        echo "FAIL $name: zcc did not compile it"
        sed 's/^/      /' "$work/$name.zcc.err"
        fail=$((fail + 1))
        continue
    fi

    # --root gives the filesystem syscalls somewhere to go. A fresh,
    # empty directory per run, so a test that leaves a file behind
    # cannot make the next run of the suite behave differently.
    if ! "$sim" -q -r "$work/root" -n 200000000 "$work/$name.zcc.bin" \
            > "$work/$name.zcc.out" 2> "$work/$name.zcc.simerr"; then
        echo "FAIL $name: zcc output did not run cleanly"
        sed 's/^/      /' "$work/$name.zcc.simerr"
        fail=$((fail + 1))
        continue
    fi

    if [ "$have_ref" = 1 ]; then
        if [ "$uselibz" = 1 ]; then
            refstart="$here/ref_libz_start.S"
            reflibs="$here/../libz/syscall.o $here/../libz/mem.o \
                     $here/../libz/str.o $here/../libz/fmt.o \
                     $here/../libz/glue.o $here/../libz/zgfx.o \
                     $here/../libz/zwin.o $here/../libz/zobj.o \
                     $here/../libz/zkbd.o $here/../libz/zfont_data.o \
                     $here/../libz/zport.o -lgcc"
            refinc="-I $here/../libz"
        else
            refstart="$here/ref_start.S"
            reflibs=""
            refinc=""
        fi

        # -mno-relax to match how libz itself was built. Not cosmetic:
        # mixing relaxed and unrelaxed objects means some accesses go
        # through gp and some do not, and gp is never set at all here.
        # A reference build that will not link is a FAILED test, not a
        # reason to abandon the run -- `set -e` would otherwise take
        # the whole suite down with it and report nothing at all,
        # which is what happened the first time a libz test was added.
        if ! "${RISCV}gcc" -march=rv32im -mabi=ilp32 -nostdlib -ffreestanding \
            -mno-relax -msmall-data-limit=0 \
            -O1 -fno-builtin -I "$here" -I "$here/../../../common" $refinc \
            -T "$here/ref.ld" \
            -o "$work/$name.ref.elf" "$refstart" "$src" $reflibs \
            2> "$work/$name.ref.err"; then
            echo "FAIL $name: the reference build did not link"
            grep -v "LOAD segment" "$work/$name.ref.err" | sed 's/^/      /' | head -10
            fail=$((fail + 1))
            continue
        fi

        edata=$("${RISCV}nm" "$work/$name.ref.elf" | awk '$3=="_edata"{print "0x"$1}')
        end=$("${RISCV}nm" "$work/$name.ref.elf" | awk '$3=="_end"{print "0x"$1}')
        "${RISCV}objcopy" -O binary "$work/$name.ref.elf" "$work/$name.ref.data"
        python3 "$mkexec" "$work/$name.ref.data" "$work/$name.ref.bin" \
            $((end - edata)) > /dev/null

        rm -rf "$work/root"; mkdir -p "$work/root"
        "$sim" -q -r "$work/root" -n 200000000 "$work/$name.ref.bin" \
            > "$work/$name.ref.out" 2>/dev/null || true

        if ! cmp -s "$work/$name.zcc.out" "$work/$name.ref.out"; then
            echo "FAIL $name: zcc and $RISCV gcc disagree"
            diff -u "$work/$name.ref.out" "$work/$name.zcc.out" \
                | sed 's/^/      /' | head -30
            fail=$((fail + 1))
            continue
        fi
    fi

    echo "ok   $name"
    pass=$((pass + 1))
done

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
