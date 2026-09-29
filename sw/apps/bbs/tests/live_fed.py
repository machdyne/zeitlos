#!/usr/bin/env python3
# Two BBS nodes over zfed: each a fed daemon and bbs-linux, callers on
# telnet. docs/bbs.md, "Federation".
#
#   make -C sw/apps/bbs live       (builds bbs-linux and ../fed's fed-linux)
#
# alpha publishes network t's node list, naming both. A caller's post on
# alpha reaches beta's forum as handle@alpha, and the other way; a local
# forum stays local; with alpha's fed down a post waits in the outbox and
# goes when it is back; what arrives from elsewhere cannot carry escape
# sequences or pose as someone on another node; one post sent twice is
# one message.
import os, re, socket, subprocess, sys, tempfile, time, shutil, json

BBS = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./bbs-linux")
FED = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "../fed/fed-linux")
checks = fails = 0
def ck(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL:", what)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

base = tempfile.mkdtemp(prefix="bbs-fed-")
FA, FB, BA, BB = [os.path.join(base, d) for d in ("fed-a", "fed-b", "bbs-a", "bbs-b")]
for d in (FA, FB, BA, BB): os.makedirs(d)
def key(d): return subprocess.check_output([FED, "--dir", d, "--print-key"]).decode().strip()
ka, kb = key(FA), key(FB)
pa, ta, tb = free_port(), free_port(), free_port()
open(os.path.join(FA, "fed.cfg"), "w").write(
    "name: alpha\nlisten: %d\npoll_seconds: 1\npeer: %s\nsubscribe: t/*\nnetwork: t %s\n" % (pa, kb, ka))
open(os.path.join(FB, "fed.cfg"), "w").write(
    "name: beta\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\nsubscribe: t/*\nnetwork: t %s\n" % (ka, pa, ka))
FORUMS = "general; General; 0; 10; Anything at all; t/forum/general\nlocal; Local; 0; 10; This node only\n"
for d, f in ((BA, FA), (BB, FB)):
    open(os.path.join(d, "bbs.cfg"), "w").write("name: %s\nfed: %s/fed.sock\n" % (os.path.basename(d), f))
    open(os.path.join(d, "forums.cfg"), "w").write(FORUMS)

procs = {}
def start(name, argv, sock=None):
    procs[name] = subprocess.Popen(argv, stderr=open(os.path.join(base, name + ".log"), "a"))
    if sock:
        for _ in range(200):
            if os.path.exists(sock): return
            time.sleep(0.05)
        raise SystemExit(name + " did not start")
def stop(name):
    procs[name].terminate(); procs[name].wait(timeout=10)

class Caller:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.settimeout(0.2); self.screen = b""
        # The BBS asks where the cursor is (ESC [6n) to tell what the
        # terminal is: answered as a VT100 answers it, and the login
        # prompt waited for before anything is typed.
        until = time.time() + 10
        answered = False
        while time.time() < until and b"NEW to join" not in self.screen:
            self.drain(0.2)
            if not answered and b"\x1b[6n" in self.screen:
                self.s.sendall(b"\x1b[1;1R"); answered = True
    def drain(self, secs):
        until = time.time() + secs
        while time.time() < until:
            try:
                d = self.s.recv(65536)
                if not d: return
                self.screen += d
            except socket.timeout: pass
    def type(self, s, wait=0.35):
        self.s.sendall(s.encode()); self.drain(wait)
    def join(self, handle, pw="hunter22"):
        self.type("new\r"); self.type(handle + "\r"); self.type(pw + "\r"); self.type(pw + "\r")
        self.type("Somewhere\r"); self.type("y", 0.6)
        for _ in range(8): self.type("\r", 0.25)
    def post(self, forum, subject, lines):
        # the full-screen editor (zetta): the Subject field, Enter, the
        # text, Ctrl-S to post
        self.type("f"); self.type("%d\r" % forum); self.type("w"); self.type(subject + "\r")
        self.type("\r".join(lines))
        self.type("\x13", 0.8)
        text = self.screen.decode("utf-8", "replace")
        # any key -> the forum's menu; q -> the forum list; Enter (it is a
        # field) -> the main menu
        self.type("\r"); self.type("q"); self.type("\r")
        return text

def mail(c, to, subject, lines):
    # the mail's menu, W: zetta with To first; Ctrl-S sends
    c.type("m"); c.type("w"); c.type(to + "\r"); c.type(subject + "\r")
    c.type("\r".join(lines))
    c.type("\x13", 0.8)
    text = c.screen.decode("utf-8", "replace")
    c.type("\r"); c.type("q")                    # any key -> the mail's menu; q -> the main menu
    return text

def in_fed_store(feddir, needle):
    for root, _, files in os.walk(os.path.join(feddir, "store")):
        for f in files:
            if needle in open(os.path.join(root, f), "rb").read(): return True
    return False

def deleted(bbsdir, tag, needle):
    """Is the message holding `needle` marked deleted in the forum's index?
    (32-byte entries, little-endian: offset, length, ..., flags at 24.)"""
    import struct
    lp, ip = os.path.join(bbsdir, "msgs", tag + ".log"), os.path.join(bbsdir, "msgs", tag + ".idx")
    if not os.path.exists(ip): return None
    log, idx = open(lp, "rb").read(), open(ip, "rb").read()
    for i in range(0, len(idx) - 31, 32):
        off, ln = struct.unpack_from("<II", idx, i)
        if needle.encode() in log[off:off + ln]:
            return bool(struct.unpack_from("<I", idx, i + 24)[0] & 1)
    return None

def log_of(bbsdir, tag):
    p = os.path.join(bbsdir, "msgs", tag + ".log")
    return open(p, "rb").read().decode("utf-8", "replace") if os.path.exists(p) else ""
def wait_for(pred, secs):
    until = time.time() + secs
    while time.time() < until:
        if pred(): return True
        time.sleep(0.2)
    return False

def fed_pub(fdir, topic, payload):
    s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(fdir, "fed.sock"))
    p = payload.encode()
    s.sendall(b"PUB %s bbs.post json log - %d\n" % (topic.encode(), len(p)) + p)
    r = s.recv(200); s.close(); return r

try:
    start("fed-a", [FED, "--dir", FA, "--bind", "127.0.0.1"], os.path.join(FA, "fed.sock"))
    start("fed-b", [FED, "--dir", FB, "--bind", "127.0.0.1"], os.path.join(FB, "fed.sock"))
    lst = os.path.join(FA, "list.json")
    open(lst, "w").write(json.dumps({"network": "t", "nodes": [{"key": ka, "name": "alpha"}, {"key": kb, "name": "beta"}]}))
    ck(subprocess.run([FED, "--dir", FA, "--publish-list", lst], capture_output=True).returncode == 0, "alpha publishes the list")
    start("bbs-a", [BBS, "-d", BA, "-t", str(ta)], os.path.join(BA, "bbs.sock"))
    start("bbs-b", [BBS, "-d", BB, "-t", str(tb)], os.path.join(BB, "bbs.sock"))
    time.sleep(2)

    # -- a post on alpha, in the federated forum --
    phil = Caller(ta); phil.join("phil")
    shown = phil.post(1, "Hello from Alpha", ["The first post over zfed.", "Two lines of it."])
    ck("Sent to the network" in shown, "the caller is told it went to the network")
    ck(wait_for(lambda: "Hello from Alpha" in log_of(BB, "general"), 20), "it reaches beta's forum")
    lb = log_of(BB, "general")
    ck("from: phil@alpha" in lb, "as phil@alpha, the name from the node list")
    ck("Two lines of it." in lb, "the whole body")
    la = log_of(BA, "general")
    ck("from: phil\n" in la and re.search(r"id: [0-9a-f]{64}\n", la) is not None,
       "on alpha too, as plain phil, under its object id")

    # -- one on beta, back --
    anna = Caller(tb); anna.join("anna")
    anna.post(1, "Hello from Beta", ["Right back at you."])
    ck(wait_for(lambda: "from: anna@beta" in log_of(BA, "general"), 20), "a post on beta reaches alpha as anna@beta")

    # -- deleting a federated post (docs/fed.md, "Moderation") --
    # anna is beta's first account: its sysop. She may delete phil's post
    # THERE -- but beta is neither its origin nor a moderator, so the
    # cancel she sends is honoured nowhere else.
    ck(deleted(BB, "general", "Hello from Alpha") is False, "(beta has phil's post)")
    anna.type("f"); anna.type("1\r"); anna.type("a"); anna.type("d", 0.8)
    ck("a cancel sent to the network" in anna.screen.decode("utf-8", "replace"), "anna, beta's sysop, deletes phil's post on beta")
    ck(deleted(BB, "general", "Hello from Alpha") is True, "deleted on beta")
    time.sleep(8)
    ck(deleted(BA, "general", "Hello from Alpha") is False, "but not on alpha: beta's cancel is not honoured there")
    anna.type("q"); anna.type("q"); anna.type("\r")
    # phil deletes a post of his own: alpha is its origin -- honoured everywhere
    phil.post(1, "To be withdrawn", ["Posted, then deleted by its writer."])
    ck(wait_for(lambda: deleted(BB, "general", "To be withdrawn") is False, 20), "(phil's second post on beta)")
    phil.type("f"); phil.type("1\r"); phil.type("a")
    for _ in range(3):
        if "To be withdrawn" in phil.screen.decode("utf-8", "replace")[-1500:]: break
        phil.type("n")
    phil.type("d", 0.8)
    ck(deleted(BA, "general", "To be withdrawn") is True, "phil deletes it on alpha")
    ck(wait_for(lambda: deleted(BB, "general", "To be withdrawn") is True, 25), "and on beta: his node's cancel honoured there")
    ck(deleted(BB, "general", "Hello from Beta") is False, "beta's own post untouched")
    phil.type("q"); phil.type("q"); phil.type("\r")

    # -- a local forum stays local --
    phil.post(2, "Just here", ["Not for the network."])
    time.sleep(4)
    ck("Just here" in log_of(BA, "local") and "Just here" not in log_of(BB, "local") and
       "Just here" not in log_of(BB, "general"), "a local forum's post stays on its node")

    # -- alpha's fed down: the post waits, then goes --
    stop("fed-a")
    time.sleep(1)
    shown = phil.post(1, "Queued while down", ["Sent when fed is back."])
    ck("Waiting for this node's fed" in shown, "fed down: the caller is told it waits")
    ob = os.path.join(BA, "fed-outbox")
    ck(os.path.exists(ob) and os.path.getsize(ob) > 0, "it is in the outbox")
    start("fed-a", [FED, "--dir", FA, "--bind", "127.0.0.1"], os.path.join(FA, "fed.sock"))
    ck(wait_for(lambda: "Queued while down" in log_of(BB, "general"), 30), "fed back: the post goes, and reaches beta")
    ck(wait_for(lambda: not os.path.exists(ob) or os.path.getsize(ob) == 0, 10), "and the outbox is empty")

    # -- what arrives from elsewhere: no escapes, no posing --
    evil = json.dumps({"from": "anna@beta", "subject": "Hi\u001b[2J", "date": int(time.time()),
                       "body": "ok\u001b]0;title\u0007 then \u009b31m red", "post": "evil1"})
    ck(fed_pub(FA, "t/forum/general", evil).startswith(b"OK"), "(a hostile post published on alpha)")
    ck(wait_for(lambda: "title then" in log_of(BB, "general"), 20), "it reaches beta")
    lb = log_of(BB, "general")
    ck("from: anna_beta@alpha" in lb, "an '@' in a remote handle cannot pose as anna@beta")
    raw = open(os.path.join(BB, "msgs", "general.log"), "rb").read()
    ck(b"\x1b" not in raw and b"\xc2\x9b" not in raw and b"\x07" not in raw, "no escape, no C1 CSI, no bell: taken out")

    # -- one post sent twice is one message --
    twice = json.dumps({"from": "carl", "subject": "Once", "date": int(time.time()), "body": "only once", "post": "tok-twice"})
    fed_pub(FA, "t/forum/general", twice)
    time.sleep(1.1)
    fed_pub(FA, "t/forum/general", twice)				# a new object: a later time
    time.sleep(6)
    ck(log_of(BB, "general").count("only once") == 1, "the same post twice (two objects, one token): one message")
    ck(log_of(BA, "general").count("only once") == 1, "on alpha too")
    # -- mail between nodes: sealed to the node, into the addressee's mailbox --
    shown = mail(phil, "anna@beta", "Hello Anna", ["A letter across nodes.", "Only beta can read it."])
    ck("Sent over the network, sealed" in shown, "phil writes to anna@beta: sent, sealed")
    ck(wait_for(lambda: "Hello Anna" in log_of(BB, "mail"), 25), "it arrives in beta's mail")
    lb = log_of(BB, "mail")
    ck("from: phil@alpha" in lb and "to: anna\n" in lb and "Only beta can read it." in lb,
       "from phil@alpha, to anna, the whole letter")
    ck(in_fed_store(FA, b"ZML1") and in_fed_store(FB, b"ZML1"), "(both fed stores hold the sealed letter)")
    ck(not in_fed_store(FA, b"Only beta can read it.") and not in_fed_store(FB, b"Only beta can read it."),
       "and in neither node's fed store in the clear: sealed all the way")
    mail(anna, "phil@alpha", "Re: Hello Anna", ["Got it. Hello back."])
    ck(wait_for(lambda: "Hello back." in log_of(BA, "mail"), 25) and "from: anna@beta" in log_of(BA, "mail"),
       "anna answers phil@alpha: it arrives, from anna@beta")

    # -- nobody by that name there: it comes back --
    mail(phil, "nobody@beta", "Anyone?", ["Is anybody home?"])
    ck(wait_for(lambda: "Not delivered: Anyone?" in log_of(BA, "mail"), 30), "a letter to nobody@beta: back to phil, not delivered")
    la = log_of(BA, "mail")
    ck("from: postmaster@beta" in la and "no user called nobody at beta" in la, "from beta's postmaster, saying why")

    # -- a node that is not on the network: refused at once --
    phil.type("m"); phil.type("w"); phil.type("anna@nowhere\r")
    ck("No node by that name on the network." in phil.screen.decode("utf-8", "replace"), "anna@nowhere: no such node, said at once")
    phil.type("\x18"); phil.type("y"); phil.type("\r"); phil.type("q")

    # -- a forum carried later: its history, once --
    older = json.dumps({"from": "carl", "subject": "Early news", "date": int(time.time()), "body": "news from before", "post": "tok-early"})
    ck(fed_pub(FA, "t/forum/news", older).startswith(b"OK"), "(a post on t/forum/news, which beta's BBS does not carry)")
    ck(wait_for(lambda: in_fed_store(FB, b"news from before"), 20), "it reaches beta's fed -- not shown: no forum for it")
    stop("bbs-b")
    open(os.path.join(BB, "forums.cfg"), "a").write("news; News; 0; 10; Carried later; t/forum/news\n")
    start("bbs-b", [BBS, "-d", BB, "-t", str(tb)], os.path.join(BB, "bbs.sock"))
    ck(wait_for(lambda: "news from before" in log_of(BB, "news"), 20), "beta's BBS carries it now: the older post arrives, as history")
    ck(wait_for(lambda: "the history of 1 forum taken" in open(os.path.join(base, "bbs-b.log")).read(), 10),
       "and says so: the history of 1 forum taken")
    ck(log_of(BB, "general").count("only once") == 1, "general, carried all along, not sent again")
finally:
    for n in list(procs):
        try: stop(n)
        except Exception: procs[n].kill()
print("bbs over fed: %d checks, %d failed" % (checks, fails))
if fails:
    print("logs in", base)
else:
    shutil.rmtree(base)
sys.exit(1 if fails else 0)
