#!/usr/bin/env python3
"""
convert's folder packing (pack.c, over zwalk.c and zf_pack.c), on the
build machine through tests/pack_host.c: a tree of files packed into
zip, tar and tar.gz, then read back with Python's zipfile and tarfile.
The archive written inside the folder it packs is left out of itself;
cancelling leaves no file open; a folder deeper than zwalk goes is
reported, not fatal.

    python3 sw/apps/convert/tests/test_pack.py      (or: make -C sw/apps/convert test)
"""
import os, sys, subprocess, tempfile, shutil, zipfile, tarfile, random

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
C = os.path.join(ROOT, 'sw', 'common')
TMP = tempfile.mkdtemp(prefix='pack_')
BIN = os.path.join(TMP, 'pack_host')
checks = fails = 0

def ck(c, what):
    global checks, fails
    checks += 1
    if not c:
        fails += 1
        print('FAIL:', what)

import glob
srcs = [os.path.join(HERE, 'pack_host.c'), os.path.join(HERE, '..', 'pack.c'), os.path.join(C, 'zwalk.c'),
        os.path.join(C, 'zinflate.c'), os.path.join(C, 'zfix.c'),
        os.path.join(ROOT, 'sw', 'apps', 'sheet', 'sheet_core.c')] + sorted(glob.glob(os.path.join(C, 'formats', '*.c')))
subprocess.check_call(['cc', '-std=gnu99', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                       '-fno-sanitize-recover=undefined', '-I', C, '-o', BIN] + srcs)

card = os.path.join(TMP, 'card')
rnd = random.Random(3)
tree = {
    'home/photos/a.txt': b'hello\n' * 100,
    'home/photos/2024/b.bin': bytes(rnd.randrange(256) for _ in range(40000)),
    'home/photos/2024/june/c.md': b'# June\n' + b'sunny ' * 3000,
    'home/photos/empty.txt': b'',
}
for i in range(40):                       # more than one page of a folder, and of zwalk's window
    tree['home/photos/many/f%02d.txt' % i] = ('file %d\n' % i).encode() * (i + 1)
for k, v in tree.items():
    p = os.path.join(card, k)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(v)
os.makedirs(os.path.join(card, 'home/photos/nothing'), exist_ok=True)
want = {k[len('home/'):]: v for k, v in tree.items()}

def run(*a, ok=True):
    p = subprocess.run([BIN] + list(a), capture_output=True, text=True, env=dict(os.environ, PACK_ROOT=card,
                       ASAN_OPTIONS='detect_leaks=0'))
    if 'runtime error' in p.stderr or 'Sanitizer' in p.stderr:
        ck(False, 'sanitizer: ' + p.stderr[-1500:])
    if ok:
        ck(p.returncode == 0, ' '.join(a) + ': ' + p.stderr)
    return p

for ext in ('zip', 'tar', 'tar.gz'):
    for out in ('/home/out.' + ext, '/home/photos/inside.' + ext):
        run('/home/photos/', out)
        got, dirs = {}, set()
        path = os.path.join(card, out[1:])
        if ext == 'zip':
            with zipfile.ZipFile(path) as z:
                ck(z.testzip() is None, 'zip CRCs')
                for i in z.infolist():
                    if i.filename.endswith('/'): dirs.add(i.filename[:-1])
                    else: got[i.filename] = z.read(i)
        else:
            with tarfile.open(path) as t:
                for m in t.getmembers():
                    if m.isdir(): dirs.add(m.name)
                    else: got[m.name] = t.extractfile(m).read()
        ck(got == want, '%s -> %s: the files (extra %s, missing %s)' % (ext, out, sorted(set(got) - set(want))[:3],
           sorted(set(want) - set(got))[:3]))
        ck({'photos', 'photos/2024', 'photos/2024/june', 'photos/many', 'photos/nothing'} <= dirs, ext + ': folders')
        os.unlink(path)

# deeper than zwalk goes: left out and counted
deep = 'home/deep/' + '/'.join('d%d' % i for i in range(20))
os.makedirs(os.path.join(card, deep), exist_ok=True)
open(os.path.join(card, deep, 'x.txt'), 'w').write('x')
p = run('/home/deep', '/home/deep.zip')
ck('skipped 1' in p.stdout, 'too deep: counted: ' + p.stdout)

# cancelling, at many points: an error, and nothing left open (pack_host checks)
for n in (0, 1, 3, 10, 30, 60):
    p = run('-c', str(n), '/home/photos', '/home/c.zip', ok=False)
    ck(p.returncode == 1 and 'cancelled' in p.stderr, 'cancel after %d: %d %s' % (n, p.returncode, p.stderr))

# no memory for the compressor: stored, still right
run('-m', '20000', '/home/photos', '/home/small.tar.gz')
with tarfile.open(os.path.join(card, 'home/small.tar.gz')) as t:
    ck(t.extractfile('photos/2024/june/c.md').read() == want['photos/2024/june/c.md'], 'stored deflate blocks')

# not a folder name to give: the root
p = run('/', '/home/root.zip', ok=False)
ck(p.returncode == 1, 'the root refused')

print('test_pack: %d checks, %d failed' % (checks, fails))
shutil.rmtree(TMP, ignore_errors=True)
sys.exit(1 if fails else 0)
