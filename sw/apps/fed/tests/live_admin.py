#!/usr/bin/env python3
# The node-list commands, live: `fed key`, `fed nodes`, `fed add`, `fed
# set`, `fed remove` -- a network managed without writing JSON. docs/fed.md,
# "Managing a network".
#
#   make -C sw/apps/fed live
import os, pwd, socket, subprocess, sys, tempfile, time, shutil, json, re

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

def fed(d, *args):
    r = subprocess.run([FED, "--dir", d] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr

base = tempfile.mkdtemp(prefix="fed-admin-")
A, B, C = [os.path.join(base, n) for n in "abc"]
for d in (A, B, C): os.makedirs(d)
# keys: the first run makes them
for d in (A, B, C): subprocess.run([FED, "--dir", d, "--print-key"], capture_output=True)
def pubkey(d):
    return re.search(r"\n([0-9a-f]{64})\n", fed(d, "key")[1]).group(1)
ka, kb, kc = pubkey(A), pubkey(B), pubkey(C)
pa = free_port()
# alpha lists no peers: beta dials in, as a board dials a server -- so the
# list alone decides whether it is let in (a peer: line would override it)
open(os.path.join(A, "fed.cfg"), "w").write("name: alpha\nlisten: %d\npoll_seconds: 1\nsubscribe: t/*\nnetwork: t %s\n" % (pa, ka))
open(os.path.join(B, "fed.cfg"), "w").write("name: beta\npoll_seconds: 1\nsubscribe: t/*\nnetwork: t %s\npeer: %s 127.0.0.1:%d\n" % (ka, ka, pa))

procs = {}
def start(d):
    procs[d] = subprocess.Popen([FED, "--dir", d, "--bind", "127.0.0.1"], stderr=open(os.path.join(d, "log.txt"), "a"))
    for _ in range(100):
        if os.path.exists(os.path.join(d, "fed.sock")): return
        time.sleep(0.05)
    raise SystemExit("fed did not start")
def stop(d): procs[d].terminate(); procs[d].wait(timeout=10); del procs[d]
def wait(pred, secs):
    until = time.time() + secs
    while time.time() < until:
        if pred(): return True
        time.sleep(0.3)
    return False

try:
    # -- key: public, and said to be; the private one only named --
    rc, out = fed(A, "key")
    ck(rc == 0 and "PUBLIC key" in out and "safe to share" in out and ka in out, "key: the public key, called public")
    ck("PRIVATE key is %s/node.key" % A in out and "never share" in out, "and where the private one is -- never its contents")
    seed = open(os.path.join(A, "node.key"), "rb").read()
    ck(seed.hex() not in out, "(the private key's bytes are nowhere in what it prints)")

    rc, out = fed(A, "nodes")
    ck(rc != 0 and "not running" in out, "fed not running: said so, and how to start it")
    start(A)

    rc, out = fed(A, "nodes")
    ck(rc == 0 and "no list yet" in out and "fed add" in out, "nodes, before any list: none yet -- and how to make one")

    # -- add: the first list, this node first; then beta joins --
    rc, out = fed(A, "add", "beta", kb, "sysop=bob")
    ck(rc == 0 and "published: t now has 2 nodes" in out and "as alpha" in out, "add beta: the first list -- alpha on it too (%s)" % out.strip())
    rc, out = fed(A, "nodes")
    ck(rc == 0 and re.search(r"alpha\s+[0-9a-f]{16}.*\(this node\)", out) and re.search(r"beta\s+[0-9a-f]{16}\s+bob", out),
       "nodes: both, with short ids, sysop, and this node marked")
    start(B)			# after it is added: `fed key` gave its key without running it
    ck(wait(lambda: "session to" in open(os.path.join(B, "log.txt")).read() and
            re.search(r"session to \S+: done", open(os.path.join(B, "log.txt")).read()), 15), "beta joins: its sessions done")

    # -- what is refused, and why --
    rc, out = fed(A, "add", "beta", kc)
    ck(rc == 1 and "already" in out and "fed set beta" in out, "a name in use: refused (%s)" % out.strip())
    rc, out = fed(A, "add", "gamma", kb)
    ck(rc == 1 and "already, as beta" in out, "a key already listed: refused")
    rc, out = fed(A, "add", "gamma", kc.upper())
    ck(rc == 1 and "not a public key" in out, "a key in capitals, or not 64 hex: refused")
    rc, out = fed(A, "add", "gamma", kc[:63])
    ck(rc == 1 and "not a public key" in out, "63 characters: refused")
    rc, out = fed(B, "add", "gamma", kc)
    ck(rc == 1 and "does not publish" in out, "add on a node that does not publish the list: refused")
    rc, out = fed(A, "remove", "alpha")
    ck(rc == 1 and "stays on it" in out, "the publisher removing itself: refused")

    # -- set; a profile kept --
    rc, out = fed(A, "set", "beta", "addr=beta.example.com:9070")
    ck(rc == 0 and "published" in out, "set beta addr=... (%s)" % out.strip())
    ck("beta.example.com:9070" in fed(A, "nodes")[1], "and it shows")
    s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(A, "fed.sock")); s.sendall(b"LIST t\n")
    time.sleep(0.3); cur = s.recv(65536).decode().split("\n", 1)[1]; s.close()
    lst = json.loads(cur); lst["profile"] = {"max_object": 4096}
    open(os.path.join(A, "l.json"), "w").write(json.dumps(lst))
    fed(A, "--publish-list", os.path.join(A, "l.json"))
    fed(A, "add", "gamma", kc)
    s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(A, "fed.sock")); s.sendall(b"LIST t\n")
    time.sleep(0.3); cur = json.loads(s.recv(65536).decode().split("\n", 1)[1]); s.close()
    ck(cur.get("profile") == {"max_object": 4096} and len(cur["nodes"]) == 3, "a list's profile kept through add")

    # -- remove: off the list, and its sessions refused --
    rc, out = fed(A, "remove", "beta")
    ck(rc == 0 and "removing beta" in out and "now has 2" in out, "remove beta (%s)" % out.strip())
    mark = len(open(os.path.join(A, "log.txt")).read())
    ck(wait(lambda: "not a node we talk to" in open(os.path.join(A, "log.txt")).read()[mark:], 20),
       "beta's next session: refused by alpha")
finally:
    for d in list(procs):
        try: stop(d)
        except Exception: procs[d].kill()

# -- as root, on a directory an ordinary account owns: become that account --
if os.geteuid() == 0:
    try:
        u = pwd.getpwnam("nobody")
        D = os.path.join(base, "owned")
        os.makedirs(D); os.chown(D, u.pw_uid, u.pw_gid); os.chmod(D, 0o700)
        os.chmod(base, 0o755)
        subprocess.run([FED, "--dir", D, "--print-key"], capture_output=True)
        owners = {os.stat(os.path.join(D, f)).st_uid for f in os.listdir(D)}
        ck(os.listdir(D) and owners == {u.pw_uid}, "run as root: what it makes belongs to the directory's owner, not root (%s)" % owners)
    except KeyError:
        pass
print("fed admin live: %d checks, %d failed" % (checks, fails))
if fails: print("logs in", base)
else: shutil.rmtree(base)
sys.exit(1 if fails else 0)
