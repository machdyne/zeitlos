#!/bin/sh
# sftp, scp and scp -O, the real OpenSSH programs, against the transfer
# code as netserve runs it (tests/xfer_host.c), with a directory for the
# card. Every file sent is compared byte for byte with what arrived.
#
#   sh tests/test_openssh.sh        (from sw/apps/net/netserve; make test runs it)
#
# Exit 0 pass, 1 fail, 77 skipped (no OpenSSH client here).

command -v sftp >/dev/null 2>&1 && command -v scp >/dev/null 2>&1 || {
	echo "test_openssh: no OpenSSH client -- skipped"; exit 77; }

HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PORT=$((20000 + $$ % 20000))
MONO=$HERE/../../../ext/monocypher
COMMON=$HERE/../../../common
cc --std=gnu99 -O2 -w -I"$COMMON" -o "$T/xfer_host" \
	"$HERE/tests/xfer_host.c" "$HERE/tests/nsfs_posix.c" \
	"$HERE/xfer.c" "$HERE/sftp.c" "$HERE/scp.c" \
	"$HERE/../ssh/ssh_server.c" "$HERE/../ssh/ssh_wire.c" "$HERE/../ssh/ssh_crypto.c" \
	"$COMMON/zsha256.c" "$MONO/monocypher.c" "$MONO/monocypher-ed25519.c" "$COMMON/z25519.c" || exit 1

R=$T/card
L=$T/local
mkdir -p "$R/home" "$L"
"$T/xfer_host" $PORT "$R" > "$T/host.log" 2>&1 &
HOST=$!
trap 'kill $HOST 2>/dev/null; rm -rf "$T"' EXIT
sleep 0.3

O="-q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR"
SFTP="sftp $O -P $PORT -b"
SCP="scp $O -P $PORT"
fails=0
checks=0
ok() { checks=$((checks + 1)); }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "FAIL: $*"; }
same() { if cmp -s "$1" "$2"; then ok; else bad "$3: $1 and $2 differ"; fi; }
nopart() { if [ -z "$(find "$R" -name '*.part')" ]; then ok; else bad "$1: a .part was left: $(find "$R" -name '*.part')"; fi; }

# files of awkward sizes: empty, one byte, the read and write sizes and
# either side of them, and big ones
for n in 0 1 2047 2048 2049 4095 4096 4097 32767 32768 32769 100000 1048576; do
	head -c $n /dev/urandom > "$L/f$n"
done
mkdir -p "$L/tree/a/b/c" "$L/tree/empty"
head -c 5000 /dev/urandom > "$L/tree/one"
head -c 70000 /dev/urandom > "$L/tree/a/two"
head -c 3 /dev/urandom > "$L/tree/a/b/c/three"
printf 'with a space\n' > "$L/tree/a/sp ace.txt"

# -- sftp --
{
	for n in 0 1 2047 2048 2049 4095 4096 4097 32767 32768 32769 100000 1048576; do echo "put $L/f$n"; done
	for n in 0 1 2049 32769 1048576; do echo "get f$n $L/back$n"; done
} > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 || bad "sftp put/get exited $?"
for n in 0 1 2047 2048 2049 4095 4096 4097 32767 32768 32769 100000 1048576; do same "$L/f$n" "$R/home/f$n" "sftp put $n"; done
for n in 0 1 2049 32769 1048576; do same "$L/f$n" "$L/back$n" "sftp get $n"; done

# replacing a file: the new content, no .part
head -c 9999 /dev/urandom > "$L/repl"
cp "$L/repl" "$L/f4096"
echo "put $L/f4096" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 || bad "sftp replace exited $?"
same "$L/repl" "$R/home/f4096" "sftp replace"
nopart "sftp replace"

# folders, recursively, both ways
printf 'put -r %s/tree\nget -r tree %s/tree2\n' "$L" "$L" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 || bad "sftp -r exited $?"
if diff -r "$L/tree" "$R/home/tree" >/dev/null; then ok; else bad "sftp put -r"; fi
if diff -r "$L/tree" "$L/tree2" >/dev/null; then ok; else bad "sftp get -r"; fi

# the other requests
cat > "$T/b" <<EOF
mkdir d1
rename f1 d1/g1
-rename f0 d1/g1
cd d1
ls -l
cd ..
rm f2047
rmdir tree/empty
-rmdir d1
-rm nothere
df
ln -s f2048 lnk
EOF
$SFTP "$T/b" t@127.0.0.1 > "$T/out" 2>&1
[ -f "$R/home/d1/g1" ] && ok || bad "sftp rename into a folder"
# OpenSSH's sftp renames with posix-rename@openssh.com when it is
# offered, which replaces: g1 is now f0's (empty)
[ ! -e "$R/home/f0" ] && [ ! -s "$R/home/d1/g1" ] && ok || bad "sftp rename onto a file (posix-rename)"
[ ! -e "$R/home/f2047" ] && ok || bad "sftp rm"
[ ! -e "$R/home/tree/empty" ] && ok || bad "sftp rmdir"
[ -d "$R/home/d1" ] && ok || bad "sftp rmdir of a full folder must refuse"
grep -q 'g1' "$T/out" && ok || bad "sftp ls -l"
grep -q 'Avail' "$T/out" && ok || bad "sftp df (statvfs@openssh.com)"

# posix-rename@openssh.com replaces (sftp's rename does, when offered)
echo "rename f2048 f2049" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 && ok || bad "sftp posix-rename exited"
same "$L/f2048" "$R/home/f2049" "sftp posix-rename"

# resume: reput and reget
head -c 50000 "$L/f100000" > "$R/home/half"
printf 'reput %s/f100000 half\n' "$L" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 || bad "sftp reput exited $?"
same "$L/f100000" "$R/home/half" "sftp reput"
head -c 30000 "$L/f100000" > "$L/half2"
printf 'reget f100000 %s/half2\n' "$L" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 || bad "sftp reget exited $?"
same "$L/f100000" "$L/half2" "sftp reget"

# -- scp, the SFTP way (OpenSSH 9's default) --
$SCP "$L/f32769" t@127.0.0.1:s1 2>/dev/null || bad "scp put exited $?"
same "$L/f32769" "$R/home/s1" "scp put"
$SCP t@127.0.0.1:s1 "$L/s1back" 2>/dev/null || bad "scp get exited $?"
same "$L/f32769" "$L/s1back" "scp get"
$SCP -r "$L/tree" t@127.0.0.1:/home/st 2>/dev/null || bad "scp -r put exited $?"
if diff -r "$L/tree" "$R/home/st" >/dev/null; then ok; else bad "scp -r put"; fi

# -- scp -O, the old protocol --
$SCP -O "$L/f100000" t@127.0.0.1:o1 2>/dev/null || bad "scp -O put exited $?"
same "$L/f100000" "$R/home/o1" "scp -O put"
$SCP -O "$L/f0" "$L/f1" "$L/f4097" t@127.0.0.1:/home/d1 2>/dev/null || bad "scp -O put many exited $?"
same "$L/f4097" "$R/home/d1/f4097" "scp -O put many"
same "$L/f0" "$R/home/d1/f0" "scp -O put empty"
$SCP -O t@127.0.0.1:o1 "$L/o1back" 2>/dev/null || bad "scp -O get exited $?"
same "$L/f100000" "$L/o1back" "scp -O get"
$SCP -O -r "$L/tree" t@127.0.0.1:/home/ot 2>/dev/null || bad "scp -O -r put exited $?"
if diff -r "$L/tree" "$R/home/ot" >/dev/null; then ok; else bad "scp -O -r put"; fi
$SCP -O -r t@127.0.0.1:/home/ot "$L/ot2" 2>/dev/null || bad "scp -O -r get exited $?"
if diff -r "$L/tree" "$L/ot2" >/dev/null; then ok; else bad "scp -O -r get"; fi
$SCP -O -rp "$L/tree" t@127.0.0.1:/home/otp 2>/dev/null || bad "scp -O -rp put exited $?"
if diff -r "$L/tree" "$R/home/otp" >/dev/null; then ok; else bad "scp -O -rp put"; fi
# -p: each T line answered before the next header; a miscount shows as
# "lost connection" now and then, so a few times over
for i in 1 2 3 4 5 6; do
	rm -rf "$L/otp2"
	$SCP -O -rp t@127.0.0.1:/home/otp "$L/otp2" 2>/dev/null || bad "scp -O -rp get exited $? (try $i)"
	if diff -r "$L/tree" "$L/otp2" >/dev/null; then ok; else bad "scp -O -rp get (try $i)"; fi
done
mkdir -p "$L/globbed"
$SCP -O -p "t@127.0.0.1:/home/f32*" "$L/globbed/" 2>/dev/null || bad "scp -O pattern get exited $?"
[ "$(ls "$L/globbed" | wc -l)" -eq 3 ] && ok || bad "scp -O pattern: $(ls "$L/globbed")"
$SCP -O t@127.0.0.1:/home/nothere "$L/x" 2>/dev/null && bad "scp -O of a missing file must fail" || ok
$SCP -O t@127.0.0.1:/home/d1 "$L/x" 2>/dev/null && bad "scp -O of a folder without -r must fail" || ok
$SCP -O "$L/f1" t@127.0.0.1:/home/nodir/x 2>/dev/null && bad "scp -O into a missing folder must fail" || ok
nopart "scp"

# -- what is not offered: other commands, other subsystems --
ssh $O -p $PORT t@127.0.0.1 ls >/dev/null 2>&1 && bad "exec of a command other than scp must be refused" || ok
grep -q "exec refused" "$T/host.log" && ok || bad "the refused exec is logged"
ssh $O -p $PORT -s t@127.0.0.1 netconf </dev/null >/dev/null 2>&1 && bad "a subsystem other than sftp must be refused" || ok

# -- stopped part way: the old file stays, no .part --
head -c 7 /dev/urandom > "$R/home/keep"
cp "$R/home/keep" "$L/keep.old"
head -c 4000000 /dev/urandom > "$L/huge"
echo "put $L/huge keep" > "$T/b"
timeout -s KILL 0.4 sftp $O -P $PORT -l 2000 -b "$T/b" t@127.0.0.1 >/dev/null 2>&1
sleep 0.5
same "$L/keep.old" "$R/home/keep" "sftp put stopped: old file"
nopart "sftp put stopped"
timeout -s KILL 0.4 scp $O -P $PORT -O -l 2000 "$L/huge" t@127.0.0.1:/home/keep >/dev/null 2>&1
sleep 0.5
same "$L/keep.old" "$R/home/keep" "scp -O put stopped: old file"
nopart "scp -O put stopped"

# -- read-only --
kill $HOST
wait $HOST 2>/dev/null
"$T/xfer_host" $PORT "$R" read > "$T/host2.log" 2>&1 &
HOST=$!
sleep 0.3
printf 'get f4097 %s/ro\n' "$L" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 || bad "read-only get exited $?"
same "$L/f4097" "$L/ro" "read-only get"
printf 'put %s/f1 ro1\n' "$L" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 && bad "read-only put must fail" || ok
[ ! -e "$R/home/ro1" ] && ok || bad "read-only put wrote"
echo "rm f4097" > "$T/b"
$SFTP "$T/b" t@127.0.0.1 >/dev/null 2>&1 && bad "read-only rm must fail" || ok
$SCP -O "$L/f1" t@127.0.0.1:/home/ro2 2>/dev/null && bad "read-only scp -O put must fail" || ok
[ ! -e "$R/home/ro2" ] && ok || bad "read-only scp -O wrote"

echo "test_openssh: $checks checks, $fails failed"
[ $fails -eq 0 ] || { echo "--- host log ---"; tail -40 "$T/host.log"; exit 1; }
exit 0
