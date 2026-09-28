#!/usr/bin/env python3
# Mail between nodes, live: three fed daemons over TCP. docs/fed.md,
# "Mail between nodes".
#
#   make -C sw/apps/fed live      (with live_fed.py)
#
# a publishes network t's list, naming a, b and c; b and c are a's
# peers. Every node's info (its mail keys) spreads by itself; a letter
# from a to b, by name, is sealed, carried, and opened on b to exactly
# what was sent -- and on c, which carries it too, it does not open, nor
# on a, which sealed it. Forged node info is refused.
import hashlib, os, socket, subprocess, sys, tempfile, time, shutil, json

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
def sid(k): return hashlib.sha256(bytes.fromhex(k)).hexdigest()[:16]

base = tempfile.mkdtemp(prefix="fed-mail-")
A, B, C = [os.path.join(base, x) for x in "abc"]
for d in (A, B, C): os.makedirs(d)
ka, kb, kc = key(A), key(B), key(C)
port = free_port()
open(os.path.join(A, "fed.cfg"), "w").write(
    "name: a\nlisten: %d\npoll_seconds: 1\npeer: %s\npeer: %s\nsubscribe: t/*\nnetwork: t %s\n" % (port, kb, kc, ka))
for d, n in ((B, "b"), (C, "c")):
    open(os.path.join(d, "fed.cfg"), "w").write(
        "name: %s\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\nsubscribe: t/*\nnetwork: t %s\n" % (n, ka, port, ka))

procs = {}
def start(d):
    procs[d] = subprocess.Popen([FED, "--dir", d, "--bind", "127.0.0.1"], stderr=open(os.path.join(d, "log.txt"), "a"))
    for _ in range(100):
        if os.path.exists(os.path.join(d, "fed.sock")): return
        time.sleep(0.05)
    raise SystemExit("fed did not start in " + d)
def stop(d): procs[d].terminate(); procs[d].wait(timeout=10)

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
    def data(self, n, timeout=5):
        until = time.time() + timeout
        while len(self.buf) < n and time.time() < until: self.fill(until)
        b, self.buf = self.buf[:n], self.buf[n:]; return b
    def cmd(self, line, payload=b""):
        self.s.sendall(line.encode() + b"\n" + payload); return self.line()
    def obj(self, timeout=10):
        l = self.line(timeout)
        if not l or not l.startswith("OBJ "): return None
        _, pos, n = l.split(); b = self.data(int(n)); self.s.sendall(b"ACK %s\n" % pos.encode()); return b
    def open(self, sealed):
        r = self.cmd("OPEN %d" % len(sealed), sealed)
        if not r or not r.startswith("OK "): return r
        return self.data(int(r.split()[1]))

def payload(obj):
    head, _, rest = obj.partition(b"\n\n")
    n = int([l for l in head.split(b"\n") if l.startswith(b"len: ")][0][5:])
    return rest[:n]

def wait_obj(c, what, timeout=20):
    until = time.time() + timeout
    while time.time() < until:
        o = c.obj(timeout=max(0.5, until - time.time()))
        if o and what(o): return o
    return None

try:
    for d in (A, B, C): start(d)
    lst = os.path.join(A, "list.json")
    open(lst, "w").write(json.dumps({"network": "t", "nodes": [{"key": ka, "name": "a"}, {"key": kb, "name": "b"}, {"key": kc, "name": "c"}]}))
    ck(subprocess.run([FED, "--dir", A, "--publish-list", lst], capture_output=True).returncode == 0, "a publishes the list")
    ca, cb, cc = Client(A), Client(B), Client(C)

    # -- 1. node info spreads by itself --
    # on a connection of its own: a subscription left open puts OBJ lines
    # where a command's answer is expected (each node publishes its info
    # twice now -- full and small -- and not all of it is read here)
    ci = Client(A)
    ci.cmd("SUB info fed/node/*")
    seen = set()
    until = time.time() + 25
    while time.time() < until and len(seen) < 3:
        o = ci.obj(timeout=2)
        if o and b"type: fed.node" in o:
            for k in (ka, kb, kc):
                if ("origin: " + k).encode() in o: seen.add(k)
    ck(len(seen) == 3, "every node's info (its mail keys) reaches a -- its own, b's, c's (%d)" % len(seen))
    time.sleep(3)

    # -- 2. a letter from a to b, by name --
    letter = json.dumps({"from": "phil", "to": "anna", "subject": "Hello", "body": "A letter only b can read."}).encode()
    r = ca.cmd("MAIL b bbs.mail %d" % len(letter), letter)
    ck(r and r.startswith("OK "), "a: MAIL b -- sealed and published (%s)" % r)
    mt = "t/mail/" + sid(kb)
    cb.cmd("SUB m %s" % mt)
    ob = wait_obj(cb, lambda o: b"type: bbs.mail" in o)
    ck(ob is not None and ("origin: " + ka).encode() in ob and b"format: bytes" in ob, "b gets it, on %s, from a" % mt)
    sealed = payload(ob) if ob else b""
    ck(sealed.startswith(b"ZML1") and len(sealed) == len(letter) + 1164 and letter not in sealed,
       "sealed: ZML1, 1164 bytes more than the letter, the letter nowhere in it")
    ck(cb.open(sealed) == letter, "b opens it: exactly the letter")

    # -- 3. nobody else can --
    cc.cmd("SUB m %s" % mt)
    oc = wait_obj(cc, lambda o: b"type: bbs.mail" in o)
    ck(oc is not None and payload(oc) == sealed, "c carries the same sealed letter")
    r = cc.open(sealed)
    ck(isinstance(r, str) and r.startswith("ERR"), "c cannot open it (%s)" % r)
    r = ca.open(sealed)
    ck(isinstance(r, str) and r.startswith("ERR"), "nor can a, which sealed it (%s)" % r)
    tampered = bytearray(sealed); tampered[-20] ^= 1
    r = cb.open(bytes(tampered))
    ck(isinstance(r, str) and r.startswith("ERR"), "one byte changed: b cannot open it either")

    # -- 4. addressing --
    ck((ca.cmd("MAIL %s bbs.mail 2" % sid(kb), b"hi") or "").startswith("OK "), "MAIL by short id")
    ck((ca.cmd("MAIL %s bbs.mail 2" % kb, b"hi") or "").startswith("OK "), "MAIL by key")
    r = ca.cmd("MAIL zed bbs.mail 2", b"hi")
    ck(r and r.startswith("ERR") and "no node zed" in r, "MAIL to a name in no list: ERR (%s)" % r)
    r = cb.cmd("KEY")
    ck(r and r.split()[2] == sid(kb) and r.split()[3] == "b", "KEY: the key, the short id, the name (%s)" % r)

    # -- 4b. one name in two networks' lists: never a guess --
    # c publishes a second network, u, naming ITSELF "b"; a follows both
    ucfg = open(os.path.join(A, "fed.cfg")).read() + "network: u %s\n" % kc
    open(os.path.join(A, "fed.cfg"), "w").write(ucfg)
    stop(A); start(A); ca = Client(A)
    open(os.path.join(C, "fed.cfg"), "a").write("network: u %s\n" % kc)
    stop(C); start(C); cc = Client(C)
    ul = os.path.join(C, "u.json")
    open(ul, "w").write(json.dumps({"network": "u", "nodes": [{"key": kc, "name": "b"}, {"key": ka, "name": "a"}]}))
    ck(subprocess.run([FED, "--dir", C, "--publish-list", ul], capture_output=True).returncode == 0, "(c publishes network u, naming itself b)")
    time.sleep(5)
    r = ca.cmd("MAIL b bbs.mail 2", b"hi")
    ck(r and r.startswith("ERR") and "more than one network" in r, "MAIL b, now b in two lists: refused as ambiguous (%s)" % r)
    ck((ca.cmd("MAIL b@t bbs.mail 2", b"hi") or "").startswith("OK "), "MAIL b@t: t's b")
    ck((ca.cmd("MAIL b@u bbs.mail 2", b"hi") or "").startswith("OK "), "MAIL b@u: u's b")
    sb, sc = Client(B), Client(C)				# subscriptions: connections of their own
    sb.cmd("SUB m2 %s" % mt)
    ck(wait_obj(sb, lambda o: b"type: bbs.mail" in o and b"topic: t/mail/" in o) is not None, "t's b gets its letter on t")
    sc.cmd("SUB m3 u/mail/%s" % sid(kc))
    ck(wait_obj(sc, lambda o: b"type: bbs.mail" in o) is not None, "and u's b -- c -- gets its own, on u")

    # -- 5. forged node info: refused --
    fake = json.dumps({"name": "b", "mail": {"x25519": "00" * 32, "mlkem": "00" * 1184}}).encode()
    r = cc.cmd("PUB fed/node/%s fed.node json state info %d" % (sid(kb), len(fake)), fake)
    ck(r and r.startswith("ERR") and "may not publish" in r, "c publishing on b's fed/node/: refused (%s)" % r)

    # -- 6. a restart does not publish unchanged info again --
    before = open(os.path.join(B, "log.txt")).read().count("this node's info published")
    stop(B); start(B)
    time.sleep(1)
    ck(before == 2 and open(os.path.join(B, "log.txt")).read().count("this node's info published") == 2,
       "b restarted: its info -- full and small -- not published again")
finally:
    for d in list(procs):
        try: stop(d)
        except Exception: procs[d].kill()
print("fed mail live: %d checks, %d failed" % (checks, fails))
if fails: print("logs in", base)
else: shutil.rmtree(base)
sys.exit(1 if fails else 0)
