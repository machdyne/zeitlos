#!/usr/bin/env python3
# Two fed daemons on this machine, over TCP, with local clients.
# docs/fed.md, "Building it", step 5.
#
#   make -C sw/apps/fed live
#
# A listens; B is outbound only and polls A every second. Posts published
# at either end must reach a subscriber at the other -- each checked by the
# independent implementation (fobj_ref.py) -- subscriptions resume where
# they left off, across a client going away and a daemon restarting, and a
# stranger gets not one byte back.
import os, socket, subprocess, sys, tempfile, time, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import fobj_ref as R
except ImportError:
    R = None

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

base = tempfile.mkdtemp(prefix="fed-live-")
A, B = os.path.join(base, "a"), os.path.join(base, "b")
os.makedirs(A); os.makedirs(B)
def key(d): return subprocess.check_output([FED, "--dir", d, "--print-key"]).decode().strip()
ka, kb = key(A), key(B)
port = free_port()
# A publishes network t's node list; B is A's configured peer
open(os.path.join(A, "fed.cfg"), "w").write(
    "name: a\nlisten: %d\npoll_seconds: 1\npeer: %s\nsubscribe: t/*\nnetwork: t %s\n" % (port, kb, ka))
open(os.path.join(B, "fed.cfg"), "w").write(
    "name: b\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\nsubscribe: t/*\n" % (ka, port))

procs = {}
def start(d):
    log = open(os.path.join(d, "log.txt"), "a")
    procs[d] = subprocess.Popen([FED, "--dir", d, "--bind", "127.0.0.1"], stderr=log)
    for _ in range(100):
        if os.path.exists(os.path.join(d, "fed.sock")): return
        time.sleep(0.05)
    raise SystemExit("fed did not start in " + d)
def stop(d):
    procs[d].terminate(); procs[d].wait(timeout=10)

class Client:
    def __init__(self, d):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(os.path.join(d, "fed.sock"))
        self.buf = b""; self.s.settimeout(0.2)
    def send(self, b): self.s.sendall(b)
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
    def pub(self, topic, text, kind="log", key="-"):
        p = text.encode()
        self.send(b"PUB %s bbs.post text %s %s %d\n" % (topic.encode(), kind.encode(), key.encode(), len(p)) + p)
        return self.line()
    def sub(self, name, pattern):
        self.send(b"SUB %s %s\n" % (name.encode(), pattern.encode())); return self.line()
    def obj(self, timeout=10):
        """(position, bytes) of the next delivered object, ACKed; None on timeout."""
        until = time.time() + timeout
        l = self.line(timeout)
        if not l or not l.startswith("OBJ "): return None
        _, pos, n = l.split(); n = int(n)
        while len(self.buf) < n and time.time() < until: self.fill(until)
        b, self.buf = self.buf[:n], self.buf[n:]
        self.send(b"ACK %s\n" % pos.encode())
        return int(pos), b
    def close(self): self.s.close()

def checked(b, origin_hex):
    if R is None: return True
    p = R.parse(b, 0)
    return p is not None and p[2].hex() == origin_hex and R.verify(p[0], p[2], p[3])

try:
    start(A); start(B)
    cb = Client(B)
    replies = [cb.pub("t/forum", "post %d from b" % i) for i in range(3)]
    ck(all(r and r.startswith("OK ") for r in replies), "B publishes three: %s" % replies)
    ca = Client(A)
    ck((ca.sub("bbs", "t/*") or "").startswith("OK"), "A's client subscribes")
    got = [ca.obj() for _ in range(3)]
    ck(all(got), "the three reach A's subscriber")
    ck(all(g and checked(g[1], kb) for g in got), "each is B's, and the independent implementation verifies it")
    ck([g[1].split(b"\n\n", 1)[1].split(b"\nsig:")[0] for g in got if g] == [b"post %d from b" % i for i in range(3)],
       "in order, the right payloads")

    r = ca.pub("t/forum", "hello from a")
    sb = Client(B); sb.sub("b-reader", "t/*")
    seen = []
    for _ in range(4):
        g = sb.obj()
        if not g: break
        seen.append(g[1])
    ck(any(b"hello from a" in x and checked(x, ka) for x in seen), "A's post reaches B's subscriber, verified")

    # A client that goes away resumes after what it ACKNOWLEDGED: A's own
    # post was delivered to it (it matches t/*) but never acknowledged, so
    # it comes again first -- then the two posted since.
    ca.close()
    for i in range(2): cb.pub("t/forum", "later %d" % i)
    ca = Client(A); ca.sub("bbs", "t/*")
    got = [ca.obj() for _ in range(3)]
    ck(all(got) and b"hello from a" in got[0][1] and b"later 0" in got[1][1] and b"later 1" in got[2][1],
       "a returning subscriber: the unacknowledged one again, then only what came since")
    # Both daemons restart. (B's poll failed while A was down, so it is
    # backing off -- 30 s, as it should on a real network; a restarted
    # node polls at once.)
    ca.close(); cb.close(); stop(A); start(A); stop(B); start(B)
    cb = Client(B)
    cb.pub("t/forum", "after restart")
    ca = Client(A); ca.sub("bbs", "t/*")
    g = ca.obj(timeout=15)
    # "Nothing old again": the last ACK before the stop was sent and is in
    # A's socket, unread, when A is told to stop -- a clean stop reads it
    # first (linux/main.c). Without that, "later 1" comes again: delivery is
    # at least once, and this is the case a clean stop should not hit.
    ck(g is not None and b"after restart" in g[1], "after A restarts: the new post, and nothing old again")

    # the local interface refuses what it cannot publish
    ck((cb.pub("T/Bad", "x") or "").startswith("ERR"), "a bad topic: ERR")
    ck((cb.pub("t/" + "x" * 100, "x") or "").startswith("ERR"), "a topic too long: ERR, not cut short")
    ck((cb.pub("t/" + "y" * 300, "yy") or "").startswith("ERR"), "a field longer than any buffer: ERR")
    ck((cb.pub("t/state", "v1", kind="state", key="k") or "").startswith("OK"), "a state object")

    # -- networks (docs/fed.md, "Networks"): C joins by request, then leaves --
    C = os.path.join(base, "c"); os.makedirs(C); kc = key(C)
    req = subprocess.run([FED, "--dir", C, "--join-request", "name=c", "sysop=carol", "addr=127.0.0.1:1"], capture_output=True)
    fp_c = req.stderr.decode().split("fingerprint:")[-1].strip()
    ck(req.returncode == 0 and len(fp_c) == 19, "C makes a join request, and sees its fingerprint (%s)" % fp_c)
    open(os.path.join(C, "join.txt"), "wb").write(req.stdout)
    chk = subprocess.run([FED, "--check-join", os.path.join(C, "join.txt")], capture_output=True, text=True)
    ck(chk.returncode == 0 and fp_c in chk.stdout and kc in chk.stdout, "the publisher checks it: the same fingerprint, C's key")
    bad = bytearray(req.stdout); i = bad.index(b'"sysop":"carol"') + 10; bad[i] = ord("k")
    open(os.path.join(C, "bad.txt"), "wb").write(bytes(bad))
    ck(subprocess.run([FED, "--check-join", os.path.join(C, "bad.txt")], capture_output=True).returncode != 0,
       "a request altered on the way: refused")
    entry = [l for l in chk.stdout.splitlines() if l.startswith("entry: ")][0][7:]
    open(os.path.join(A, "list.json"), "w").write('{"network":"t","nodes":[%s]}' % entry)
    pl = subprocess.run([FED, "--dir", A, "--publish-list", os.path.join(A, "list.json")], capture_output=True, text=True)
    ck(pl.returncode == 0 and "OK" in pl.stdout, "A publishes the list with C on it: %s" % (pl.stdout + pl.stderr).strip())
    open(os.path.join(A, "broken.json"), "w").write('{"network":"t","nodes":[%s,%s]}' % (entry, entry))
    ck(subprocess.run([FED, "--dir", A, "--publish-list", os.path.join(A, "broken.json")], capture_output=True).returncode != 0,
       "a list with C twice: refused before it goes out")
    open(os.path.join(C, "fed.cfg"), "w").write(
        "name: c\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\nsubscribe: t/*\nnetwork: t %s\n" % (ka, port, ka))
    start(C)
    cc = Client(C)
    ck((cc.pub("t/forum", "hello from c") or "").startswith("OK"), "C posts")
    got = None
    for _ in range(8):
        g = ca.obj(timeout=10)
        if g is None: break
        if b"hello from c" in g[1]: got = g; break
    ck(got is not None and checked(got[1], kc), "C's post reaches A's subscriber: C is on A's list")
    time.sleep(1.5)
    ck("t: a list of 1 nodes" in open(os.path.join(C, "log.txt")).read(), "and C has the list, from its publisher")
    # taken off
    open(os.path.join(A, "empty.json"), "w").write('{"network":"t","nodes":[]}')
    subprocess.run([FED, "--dir", A, "--publish-list", os.path.join(A, "empty.json")], capture_output=True)
    time.sleep(3)
    cc.pub("t/forum", "after removal")
    late = None
    until = time.time() + 5
    while time.time() < until:
        g = ca.obj(timeout=1)
        if g and b"after removal" in g[1]: late = g
    ck(late is None, "taken off the list: C's next post never arrives")
    ck(open(os.path.join(A, "log.txt")).read().count("not a node we talk to") >= 1, "A refused C's next session")
    cc.close(); stop(C)

    # a stranger: not one byte back
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(b"ZFED1" + os.urandom(64))
    try: d = s.recv(100)
    except (socket.timeout, ConnectionResetError): d = b""
    ck(d == b"", "a stranger gets not one byte back (%d)" % len(d))
    s.close()
finally:
    for d in list(procs):
        try: stop(d)
        except Exception: procs[d].kill()
logs = open(os.path.join(A, "log.txt")).read() + open(os.path.join(B, "log.txt")).read()
ck("done" in logs and "not a node we talk to" in logs, "the logs say what happened")
print("fed live: %d checks, %d failed%s" % (checks, fails, "" if R else " (no `cryptography`: not verified independently)"))
if fails:
    print("logs in", base)
else:
    shutil.rmtree(base)
sys.exit(1 if fails else 0)
