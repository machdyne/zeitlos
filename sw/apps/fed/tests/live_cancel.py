#!/usr/bin/env python3
# Cancels and moderation, live: four fed daemons over TCP. docs/fed.md,
# "Moderation".
#
#   make -C sw/apps/fed live
#
# a publishes network t's list: a, b, c and x, with c the moderator of
# t/forum/*. An author's cancel is honoured everywhere, and a cancelled
# post is not given to anyone again; a stranger's cancel of someone
# else's post is not honoured; a moderator's is -- on the topics it
# moderates, and only when the cancel names the target's true topic. And
# a cancel that arrives BEFORE its target: x publishes while offline, c
# cancels it by id, x comes back -- and its post is refused.
import os, socket, subprocess, sys, tempfile, time, shutil, json

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

base = tempfile.mkdtemp(prefix="fed-cancel-")
A, B, C, X = [os.path.join(base, n) for n in "abcx"]
for d in (A, B, C, X): os.makedirs(d)
ka, kb, kc, kx = key(A), key(B), key(C), key(X)
pa, pb = free_port(), free_port()
open(os.path.join(A, "fed.cfg"), "w").write(
    "name: a\nlisten: %d\npoll_seconds: 1\npeer: %s\npeer: %s\nsubscribe: t/*\nnetwork: t %s\n" % (pa, kb, kc, ka))
open(os.path.join(B, "fed.cfg"), "w").write(
    "name: b\nlisten: %d\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\npeer: %s\nsubscribe: t/*\nnetwork: t %s\n" % (pb, ka, pa, kx, ka))
open(os.path.join(C, "fed.cfg"), "w").write(
    "name: c\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\nsubscribe: t/*\nnetwork: t %s\n" % (ka, pa, ka))
# x, offline at first: no peers
open(os.path.join(X, "fed.cfg"), "w").write("name: x\npoll_seconds: 1\nsubscribe: t/*\nnetwork: t %s\n" % ka)

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
        self.d = d
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
    def cmd(self, line, payload=b""):
        self.s.sendall(line.encode() + b"\n" + payload); return self.line()
    def pub(self, topic, text, typ="bbs.post", fmt="text"):
        p = text.encode()
        r = self.cmd("PUB %s %s %s log - %d" % (topic, typ, fmt, len(p)), p)
        return r.split()[1] if r and r.startswith("OK ") else None
    def cancel(self, oid, topic):
        return self.pub("t/cancel", json.dumps({"id": oid, "topic": topic}), "fed.cancel", "json")
    def cancelled(self, oid): return self.cmd("CANCELLED %s" % oid)
    def objs(self, pattern, name, timeout=4):
        """Every object a fresh subscription is given, within timeout -- on
        a connection of its own, closed after: a subscription left open
        would put OBJ lines where the next command's answer is expected."""
        c = Client(self.d)
        got = c._objs(pattern, name, timeout)
        c.s.close()
        return got
    def _objs(self, pattern, name, timeout):
        self.cmd("SUB %s %s" % (name, pattern))
        got, until = [], time.time() + timeout
        while time.time() < until:
            l = self.line(timeout=0.5)
            if not l or not l.startswith("OBJ "): continue
            _, pos, n = l.split()
            while len(self.buf) < int(n): self.fill(time.time() + 1)
            got.append(self.buf[:int(n)]); self.buf = self.buf[int(n):]
            self.s.sendall(b"ACK %s\n" % pos.encode())
        return got

def wait(pred, secs):
    until = time.time() + secs
    while time.time() < until:
        if pred(): return True
        time.sleep(0.3)
    return False

try:
    for d in (A, B, C): start(d)
    lst = os.path.join(A, "list.json")
    open(lst, "w").write(json.dumps({"network": "t",
        "nodes": [{"key": k, "name": n} for k, n in ((ka, "a"), (kb, "b"), (kc, "c"), (kx, "x"))],
        "moderators": [{"key": kc, "topics": ["t/forum/*"]}]}))
    ck(subprocess.run([FED, "--dir", A, "--publish-list", lst], capture_output=True).returncode == 0, "a publishes the list, c its moderator")
    ca, cb, cc = Client(A), Client(B), Client(C)
    time.sleep(4)

    def everywhere(oid, answer, nodes, secs=15):
        return wait(lambda: all(c.cancelled(oid) == "OK " + answer for c in nodes), secs)

    # -- 1. an author's cancel --
    p1 = cb.pub("t/forum/general", "b's post, to be withdrawn")
    time.sleep(4)
    ck(p1 and cb.cancel(p1, "t/forum/general"), "b cancels its own post")
    ck(everywhere(p1, "yes", (ca, cb, cc)), "honoured on a, b and c")
    seen = [o for o in ca.objs("t/forum/*", "fresh1") if b"b's post, to be withdrawn" in o]
    ck(not seen, "a new subscriber on a is not given it")

    # -- 2. a stranger's cancel: not honoured --
    p2 = ca.pub("t/forum/general", "a's post")
    time.sleep(4)
    cb.cancel(p2, "t/forum/general")
    time.sleep(5)
    ck(all(c.cancelled(p2) == "OK no" for c in (ca, cb, cc)), "b cancelling a's post: honoured nowhere")

    # -- 3. the moderator's: honoured --
    cc.cancel(p2, "t/forum/general")
    ck(everywhere(p2, "yes", (ca, cb, cc)), "c, the forum's moderator, cancelling it: honoured everywhere")

    # -- 4. a moderator outside its topics, or naming a false topic --
    p3 = ca.pub("t/news", "not a forum")
    time.sleep(4)
    cc.cancel(p3, "t/forum/general")				# a lie about its topic
    cc.cancel(p3, "t/news")						# the truth -- but c does not moderate t/news
    time.sleep(5)
    ck(all(c.cancelled(p3) == "OK no" for c in (ca, cb, cc)),
       "c cancelling on t/news -- lying about the topic, or not: honoured nowhere (judged by the target's own topic)")

    # -- 5. a cancel that arrives before its target --
    start(X)
    cx = Client(X)
    p4 = cx.pub("t/forum/general", "x's post, written offline")
    cx.s.close(); stop(X)
    ck(p4 is not None, "x publishes while offline")
    cc.cancel(p4, "t/forum/general")
    time.sleep(4)
    open(os.path.join(X, "fed.cfg"), "a").write("peer: %s 127.0.0.1:%d\n" % (kb, pb))
    start(X)
    ck(wait(lambda: "cancelled before it came" in open(os.path.join(B, "log.txt")).read(), 20),
       "x comes back: b refuses its post -- cancelled before it came")
    time.sleep(3)
    ck(not [o for o in cb.objs("t/forum/*", "fresh2") if b"written offline" in o] and
       not [o for o in ca.objs("t/forum/*", "fresh3") if b"written offline" in o], "and it is on neither b nor a")
finally:
    for d in list(procs):
        try: stop(d)
        except Exception: procs[d].kill()
print("fed cancel live: %d checks, %d failed" % (checks, fails))
if fails: print("logs in", base)
else: shutil.rmtree(base)
sys.exit(1 if fails else 0)
