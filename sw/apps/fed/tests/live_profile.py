#!/usr/bin/env python3
# Network profiles and link limits, live: a MIXED network -- internet
# nodes, and nodes behind links too small for everything. docs/fed.md,
# "Network profiles".
#
#   make -C sw/apps/fed live
#
# Network t: {"max_object": 8192, "suite": "classical"}. a and b talk
# freely; c and d are each behind a link to b limited to 700 bytes -- set
# on b's side for c (b withholds), on d's side for d (d refuses). Larger
# than the network: refused. Larger than a link: stays on the internet
# side. Small: everywhere. Letters sealed classically, small enough to
# cross; node info in its small form crossing, the full form not.
import os, socket, subprocess, sys, tempfile, time, shutil, json, hashlib, re

FED = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./fed-linux")
checks = fails = 0
def ck(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL:", what)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
def key(d): return subprocess.check_output([FED, "--dir", d, "--print-key"]).decode().strip()

base = tempfile.mkdtemp(prefix="fed-profile-")
A, B, C, D = [os.path.join(base, n) for n in "abcd"]
for d in (A, B, C, D): os.makedirs(d)
ka, kb, kc, kd = key(A), key(B), key(C), key(D)
pa, pb = free_port(), free_port()
open(os.path.join(A, "fed.cfg"), "w").write(
    "name: a\nlisten: %d\npoll_seconds: 1\npeer: %s\nsubscribe: t/*\nnetwork: t %s\n" % (pa, kb, ka))
open(os.path.join(B, "fed.cfg"), "w").write(
    "name: b\nlisten: %d\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\npeer: %s max=700\npeer: %s\nsubscribe: t/*\nnetwork: t %s\n"
    % (pb, ka, pa, kc, kd, ka))
open(os.path.join(C, "fed.cfg"), "w").write(
    "name: c\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\nsubscribe: t/*\nnetwork: t %s\n" % (kb, pb, ka))
open(os.path.join(D, "fed.cfg"), "w").write(
    "name: d\npoll_seconds: 1\npeer: %s 127.0.0.1:%d max=700\nsubscribe: t/*\nnetwork: t %s\n" % (kb, pb, ka))

procs = {}
def start(d):
    procs[d] = subprocess.Popen([FED, "--dir", d, "--bind", "127.0.0.1"], stderr=open(os.path.join(d, "log.txt"), "a"))
    for _ in range(100):
        if os.path.exists(os.path.join(d, "fed.sock")): return
        time.sleep(0.05)
    raise SystemExit("fed did not start in " + d)
def stop(d): procs[d].terminate(); procs[d].wait(timeout=10); del procs[d]

class Client:
    def __init__(self, d):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(os.path.join(d, "fed.sock"))
        self.buf = b""; self.s.settimeout(0.2)
    def fill(self, until):
        while time.time() < until:
            try:
                d = self.s.recv(65536)
                if not d: return
                self.buf += d; return
            except socket.timeout: pass
    def line(self, timeout=5):
        until = time.time() + timeout
        while b"\n" not in self.buf and time.time() < until: self.fill(until)
        if b"\n" not in self.buf: return None
        l, self.buf = self.buf.split(b"\n", 1); return l.decode()
    def data(self, n):
        while len(self.buf) < n: self.fill(time.time() + 1)
        b, self.buf = self.buf[:n], self.buf[n:]; return b
    def cmd(self, line, payload=b""):
        self.s.sendall(line.encode() + b"\n" + payload); return self.line()

def everything(d, pattern, secs=3):
    """All a fresh subscriber is given (its own connection, closed after)."""
    c = Client(d); c.cmd("SUB look%d %s" % (int(time.time() * 1000) % 100000, pattern))
    got, until = [], time.time() + secs
    while time.time() < until:
        l = c.line(0.5)
        if not l or not l.startswith("OBJ "): continue
        _, pos, n = l.split(); got.append(c.data(int(n))); c.s.sendall(b"ACK %s\n" % pos.encode())
    c.s.close()
    return got

def has(d, needle, pattern="t/*"): return any(needle in o for o in everything(d, pattern))
def wait(pred, secs):
    until = time.time() + secs
    while time.time() < until:
        if pred(): return True
        time.sleep(0.5)
    return False
def payload(o):
    head, _, rest = o.partition(b"\n\n")
    n = int([l for l in head.split(b"\n") if l.startswith(b"len: ")][0][5:])
    return rest[:n]

try:
    for d in (A, B, C, D): start(d)
    lst = os.path.join(A, "list.json")
    open(lst, "w").write(json.dumps({"network": "t",
        "nodes": [{"key": k, "name": n} for k, n in ((ka, "a"), (kb, "b"), (kc, "c"), (kd, "d"))],
        "profile": {"max_object": 8192, "suite": "classical"}}))
    ck(subprocess.run([FED, "--dir", A, "--publish-list", lst], capture_output=True).returncode == 0,
       "a publishes t's list, with its profile: 8192 bytes, classical")
    ca = Client(A)
    time.sleep(5)

    # -- larger than the network: refused where it is written --
    big = "N" * 9000
    r = ca.cmd("PUB t/forum/general bbs.post text log - %d" % len(big), big.encode())
    ck(r and r.startswith("ERR") and "larger than this network takes" in r, "9000 bytes: the network refuses it (%s)" % r)

    # -- larger than a link: stays on the internet side --
    mid = "M" * 3000
    r = ca.cmd("PUB t/forum/general bbs.post text log - %d" % len(mid), mid.encode())
    ck(r and r.startswith("OK"), "3000 bytes: within the network's limit")
    small = "a short post, small enough for any link"
    ca.cmd("PUB t/forum/general bbs.post text log - %d" % len(small), small.encode())
    ck(wait(lambda: has(C, small.encode()) and has(D, small.encode()), 25), "a small post reaches c and d, behind their links")
    ck(has(B, mid.encode()), "the 3000-byte post reaches b, on the internet side")
    ck(not has(C, mid.encode()), "but not c: b withholds it (its side of the link is limited)")
    ck(not has(D, mid.encode()), "nor d: d refuses it (its side is)")
    blog, dlog = open(os.path.join(B, "log.txt")).read(), open(os.path.join(D, "log.txt")).read()
    ck("withheld: larger than this link takes" in blog, "b's log says it withheld something")
    ck(re.search(r"session to [0-9a-f]+: done -- \d+ new, \d+ had, [1-9]\d* refused", dlog) is not None,
       "d's log says it refused something")

    # -- node info: the small form crosses, the full form does not --
    infos = everything(C, "fed/node/*")
    from_a = [o for o in infos if ("origin: " + ka).encode() in o]
    ck(any(b"key: x25519" in o for o in from_a), "c has a's small node info (its X25519 key)")
    ck(not any(b"key: info" in o for o in from_a), "not the full one (2.6 KB, larger than the link)")

    # -- letters, sealed classically, cross the link --
    letter = json.dumps({"from": "phil", "to": "carol", "subject": "Hi", "body": "Across the radio link."}).encode()
    r = ca.cmd("MAIL c bbs.mail %d" % len(letter), letter)
    ck(r and r.startswith("OK"), "a writes to c (%s)" % r)
    mt = "t/mail/" + hashlib.sha256(bytes.fromhex(kc)).hexdigest()[:16]
    got = []
    ck(wait(lambda: bool([o for o in everything(C, mt) if b"type: bbs.mail" in o] and got.append(1) is None), 25),
       "the letter reaches c")
    sealed = payload([o for o in everything(C, mt) if b"type: bbs.mail" in o][0])
    ck(sealed.startswith(b"ZMX1") and len(sealed) == len(letter) + 76, "sealed classically: ZMX1, 76 bytes more (%d)" % len(sealed))
    cc = Client(C)
    r = cc.cmd("OPEN %d" % len(sealed), sealed)
    ck(r and r.startswith("OK ") and cc.data(int(r.split()[1])) == letter, "c opens it: exactly the letter")
    back = b'{"from":"carol","to":"phil","subject":"Re: Hi","body":"Received."}'
    r = cc.cmd("MAIL a bbs.mail %d" % len(back), back)
    ck(r and r.startswith("OK"), "c writes back to a, with a's small node info (%s)" % r)
finally:
    for d in list(procs):
        try: stop(d)
        except Exception: procs[d].kill()
print("fed profile live: %d checks, %d failed" % (checks, fails))
if fails: print("logs in", base)
else: shutil.rmtree(base)
sys.exit(1 if fails else 0)
