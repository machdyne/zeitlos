#
# Zeitlos hwmap -- condition-tracking Verilog reader.
#
# This is deliberately NOT a preprocessor. A preprocessor answers "what
# does this file say for one define set"; hwmap wants "what does this
# file say, and under which defines does each piece exist". So `ifdef
# and friends are never resolved. The lexer keeps a stack of the
# branches it is inside and stamps every token with that condition (see
# cond.py), and the parser propagates it to each construct it builds:
# a port, a declaration, an assignment, an instance, one port connection
# of an instance, one term of an expression.
#
# The parser is structural and lenient, not a Verilog front end. It
# understands module headers (ANSI and non-ANSI), declarations, assign,
# always/initial blocks (for the assignments inside them), and module
# instances, including `ifdef'd parameters and connections and the
# trailing commas yosys tolerates. Anything else at module level is
# skipped to the next ';'. It raises ParseError with file:line when the
# structure it does rely on is broken, rather than guessing.
#

import os
import re

from . import cond as C


class ParseError(Exception):
    pass


KEYWORDS = set("""
always always_comb always_ff and assign automatic begin buf case casex casez
cell config deassign default defparam design disable edge else end endcase
endconfig endfunction endgenerate endmodule endprimitive endspecify endtable
endtask event for force forever fork function generate genvar highz0 highz1
if ifnone incdir include initial inout input instance integer join large
liblist library localparam macromodule medium module nand negedge nmos nor
noshowcancelled not notif0 notif1 or output parameter pmos posedge primitive
pull0 pull1 pulldown pullup pulsestyle_onevent pulsestyle_ondetect rcmos
real realtime reg release repeat rnmos rpmos rtran rtranif0 rtranif1
scalared showcancelled signed small specify specparam strong0 strong1
supply0 supply1 table task time tran tranif0 tranif1 tri tri0 tri1 triand
trior trireg unsigned use uwire vectored wait wand weak0 weak1 while wire
wor xnor xor logic
""".split())

DIRECTIVES = {"ifdef", "ifndef", "elsif", "else", "endif", "define",
              "undef", "include", "timescale", "default_nettype",
              "resetall", "celldefine", "endcelldefine", "line",
              "nounconnected_drive", "unconnected_drive"}

OPS = sorted("""
<<< >>> === !== <= >= == != && || << >> ** -: +: ~& ~| ~^ ^~ -> (* *)
""".split(), key=len, reverse=True)


class Tok:
    __slots__ = ("kind", "text", "line", "cond", "file")

    def __init__(self, kind, text, line, cond, file):
        self.kind = kind          # id num str op macro sys
        self.text = text
        self.line = line
        self.cond = cond
        self.file = file

    def __repr__(self):
        return "Tok(%s %r @%d %s)" % (self.kind, self.text, self.line,
                                     C.fmt(self.cond))


class DefineSite:
    def __init__(self, name, value, cond, file, line, kind):
        self.name = name
        self.value = value
        self.cond = cond
        self.file = file
        self.line = line
        self.kind = kind          # define | default | undef | guard


# ---------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------

class Lexer:
    """Tokenises one file (and, inline, the files it `includes)."""

    def __init__(self, root, incdirs, tokens, defines, missing):
        self.root = root
        self.incdirs = incdirs
        self.tokens = tokens
        self.defines = defines
        self.missing = missing

    def rel(self, path):
        return os.path.relpath(path, self.root)

    def lex_file(self, path, outer=None, depth=0):
        if depth > 16:
            raise ParseError("%s: `include nesting too deep" % self.rel(path))
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        self._lex(text, self.rel(path), os.path.dirname(path),
                  list(outer or []), depth)

    def _lex(self, s, fname, fdir, stack, depth):
        # stack frames: dict(prior=[lits], cur=lit|None, guard=bool,
        #                    tested=name, line=int)
        def current():
            lits = set()
            for fr in stack:
                if fr["guard"]:
                    continue
                for p in fr["prior"]:
                    lits.add((p[0], not p[1]))
                if fr["cur"] is not None:
                    lits.add(fr["cur"])
            return frozenset(lits)

        base_depth = len(stack)
        i, n, line = 0, len(s), 1
        cur = current()
        toks = self.tokens

        def word_at(j):
            m = re.compile(r"[A-Za-z_][A-Za-z0-9_$]*").match(s, j)
            return (m.group(0), m.end()) if m else (None, j)

        while i < n:
            ch = s[i]
            if ch == "\n":
                line += 1
                i += 1
                continue
            if ch in " \t\r\f\v":
                i += 1
                continue
            if s.startswith("//", i):
                j = s.find("\n", i)
                i = n if j < 0 else j
                continue
            if s.startswith("/*", i):
                j = s.find("*/", i + 2)
                if j < 0:
                    raise ParseError("%s:%d: unterminated /* comment"
                                     % (fname, line))
                line += s.count("\n", i, j)
                i = j + 2
                continue
            if ch == '"':
                j = i + 1
                while j < n and s[j] != '"':
                    if s[j] == "\\":
                        j += 1
                    j += 1
                toks.append(Tok("str", s[i:j + 1], line, cur, fname))
                i = j + 1
                continue
            if ch == "`":
                w, j = word_at(i + 1)
                if w is None:
                    i += 1
                    continue
                if w in DIRECTIVES:
                    i = j
                    if w in ("ifdef", "ifndef", "elsif"):
                        name, i = word_at(self._skip_ws(s, i))
                        if name is None:
                            raise ParseError("%s:%d: `%s without a name"
                                             % (fname, line, w))
                        if w == "elsif":
                            if len(stack) <= base_depth:
                                raise ParseError("%s:%d: `elsif without `ifdef"
                                                 % (fname, line))
                            fr = stack[-1]
                            if fr["cur"] is None:
                                raise ParseError("%s:%d: `elsif after `else"
                                                 % (fname, line))
                            fr["prior"].append(fr["cur"])
                            fr["cur"] = (name, True)
                            fr["guard"] = False
                        else:
                            stack.append(dict(prior=[], cur=(name, w == "ifdef"),
                                              guard=False, tested=name,
                                              line=line, fresh=True))
                    elif w == "else":
                        if len(stack) <= base_depth:
                            raise ParseError("%s:%d: `else without `ifdef"
                                             % (fname, line))
                        fr = stack[-1]
                        if fr["cur"] is None:
                            raise ParseError("%s:%d: second `else" % (fname, line))
                        fr["prior"].append(fr["cur"])
                        fr["cur"] = None
                        fr["guard"] = False
                    elif w == "endif":
                        if len(stack) <= base_depth:
                            raise ParseError("%s:%d: `endif without `ifdef"
                                             % (fname, line))
                        stack.pop()
                    elif w in ("define", "undef"):
                        name, i = word_at(self._skip_ws(s, i))
                        if name is None:
                            raise ParseError("%s:%d: `%s without a name"
                                             % (fname, line, w))
                        val = ""
                        if w == "define":
                            # rest of line, honouring backslash continuation
                            j = i
                            buf = []
                            while j < n:
                                k = s.find("\n", j)
                                k = n if k < 0 else k
                                seg = s[j:k]
                                if seg.rstrip().endswith("\\"):
                                    buf.append(seg.rstrip()[:-1])
                                    line += 1
                                    j = k + 1
                                    continue
                                buf.append(seg)
                                j = k
                                break
                            val = " ".join(buf)
                            val = val.split("//", 1)[0].strip()
                            i = j
                        kind = w
                        if w == "define" and stack and len(stack) > base_depth:
                            fr = stack[-1]
                            if (fr.get("fresh") and fr["cur"] == (name, False)
                                    and not fr["prior"]):
                                # `ifndef X / `define X: an include guard
                                # or a default. Either way the test is
                                # bookkeeping, not a feature condition.
                                fr["guard"] = True
                                kind = "default" if val else "guard"
                        cur = current()
                        self.defines.append(DefineSite(name, val or None, cur,
                                                       fname, line, kind))
                    elif w == "include":
                        j = self._skip_ws(s, i)
                        m = re.compile(r'"([^"]+)"|<([^>]+)>').match(s, j)
                        if not m:
                            raise ParseError("%s:%d: malformed `include"
                                             % (fname, line))
                        i = m.end()
                        inc = m.group(1) or m.group(2)
                        found = None
                        for d in [fdir] + self.incdirs:
                            p = os.path.join(d, inc)
                            if os.path.exists(p):
                                found = p
                                break
                        if found is None:
                            self.missing.append((inc, current(), fname, line))
                        else:
                            self.lex_file(found, outer=stack, depth=depth + 1)
                    else:
                        # `timescale etc: skip the rest of the line
                        k = s.find("\n", i)
                        i = n if k < 0 else k
                    for fr in stack:
                        if fr is not (stack[-1] if stack else None):
                            fr["fresh"] = False
                    if stack and w not in ("ifdef", "ifndef"):
                        stack[-1]["fresh"] = stack[-1].get("fresh") and w == "define"
                    cur = current()
                    continue
                toks.append(Tok("macro", "`" + w, line, cur, fname))
                if stack:
                    stack[-1]["fresh"] = False
                i = j
                continue
            if stack:
                stack[-1]["fresh"] = False
            if ch.isalpha() or ch == "_":
                w, j = word_at(i)
                # sized literal written as  32 'h... is handled below
                toks.append(Tok("id", w, line, cur, fname))
                i = j
                continue
            if ch == "$":
                m = re.compile(r"\$[A-Za-z0-9_$]+").match(s, i)
                if m:
                    toks.append(Tok("sys", m.group(0), line, cur, fname))
                    i = m.end()
                    continue
            m = re.compile(r"(\d[\d_]*)?\s*'[sS]?[bBoOdDhH]\s*[0-9a-fA-FxXzZ_?]+"
                           r"|\d[\d_]*\.\d+|\d[\d_]*").match(s, i)
            if m and (ch.isdigit() or ch == "'"):
                txt = m.group(0)
                line += txt.count("\n")
                toks.append(Tok("num", re.sub(r"\s+", "", txt), line, cur,
                                fname))
                i = m.end()
                continue
            if s.startswith("(*", i):
                k = self._skip_ws(s, i + 2)
                if k < n and s[k] != ")":
                    j = s.find("*)", i + 2)
                    if j < 0:
                        raise ParseError("%s:%d: unterminated attribute"
                                         % (fname, line))
                    line += s.count("\n", i, j)
                    i = j + 2
                    continue
                toks.append(Tok("op", "(", line, cur, fname))
                i += 1
                continue
            for op in OPS:
                if op in ("(*", "*)"):
                    continue
                if s.startswith(op, i):
                    toks.append(Tok("op", op, line, cur, fname))
                    i += len(op)
                    break
            else:
                toks.append(Tok("op", ch, line, cur, fname))
                i += 1
        if len(stack) != base_depth:
            fr = stack[-1]
            raise ParseError("%s:%d: `%s %s is never closed"
                             % (fname, fr["line"], "ifdef", fr["tested"]))

    @staticmethod
    def _skip_ws(s, i):
        while i < len(s) and s[i] in " \t":
            i += 1
        return i


# ---------------------------------------------------------------------
# Structures
# ---------------------------------------------------------------------

class Port:
    def __init__(self, name, dir, cond, file, line, width=None):
        self.name, self.dir, self.cond = name, dir, cond
        self.file, self.line, self.width = file, line, width


class Decl:
    def __init__(self, name, kind, cond, file, line, init=None):
        self.name, self.kind, self.cond = name, kind, cond
        self.file, self.line, self.init = file, line, init


class Assign:
    """lhs <- rhs. kind: assign | wire | proc. block groups procedural
    assignments from one always block (they are one driver)."""

    def __init__(self, lhs, rhs, cond, file, line, kind, block=None):
        self.lhs, self.rhs, self.cond = lhs, rhs, cond
        self.file, self.line, self.kind, self.block = file, line, kind, block


class Conn:
    def __init__(self, name, expr, cond, line):
        self.name, self.expr, self.cond, self.line = name, expr, cond, line


class Instance:
    def __init__(self, module, name, cond, file, line):
        self.module, self.name, self.cond = module, name, cond
        self.file, self.line = file, line
        self.params = []          # [Conn]
        self.conns = []           # [Conn]


class Module:
    def __init__(self, name, file, line, cond):
        self.name, self.file, self.line, self.cond = name, file, line, cond
        self.params = {}          # name -> (value tokens, cond)
        self.ports = []
        self.decls = []
        self.assigns = []
        self.instances = []
        self.reads = []           # (name, cond, file, line) identifier uses

    def port(self, name):
        for p in self.ports:
            if p.name == name:
                return p
        return None


class Unit:
    """Everything read from one top-level file."""

    def __init__(self, path):
        self.path = path
        self.modules = []
        self.global_decls = []
        self.defines = []
        self.missing_includes = []


# ---------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------

def expr_idents(expr):
    """Identifiers read by an expression token list."""
    out = []
    for k, t in enumerate(expr):
        if t.kind != "id" or t.text in KEYWORDS:
            continue
        if k > 0 and expr[k - 1].text == ".":
            continue
        out.append(t)
    return out


def expr_cond(expr, default=C.TRUE):
    if not expr:
        return default
    return C.conj(*[t.cond for t in expr])


def expr_text(expr):
    out = []
    for t in expr:
        if out and (t.kind in ("id", "num", "macro", "sys") and
                    out[-1][-1:].isalnum()):
            out.append(" ")
        out.append(t.text)
    return "".join(out)


def parse_number(txt):
    """Verilog literal -> int, or None for x/z/unsized-real."""
    t = txt.replace("_", "")
    m = re.match(r"^(\d+)?'[sS]?([bBoOdDhH])([0-9a-fA-F]+)$", t)
    if m:
        base = {"b": 2, "o": 8, "d": 10, "h": 16}[m.group(2).lower()]
        try:
            return int(m.group(3), base)
        except ValueError:
            return None
    if re.match(r"^\d+$", t):
        return int(t)
    return None


class Parser:
    def __init__(self, toks, unit):
        self.t = toks
        self.i = 0
        self.u = unit
        self.block_id = 0

    # -- helpers --
    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else None

    def at(self, text, k=0):
        p = self.peek(k)
        return p is not None and p.text == text and p.kind in ("op", "id")

    def err(self, msg, tok=None):
        tok = tok or self.peek() or (self.t[-1] if self.t else None)
        where = "%s:%d" % (tok.file, tok.line) if tok else self.u.path
        raise ParseError("%s: %s" % (where, msg))

    def expect(self, text):
        p = self.peek()
        if p is None or p.text != text:
            self.err("expected '%s', found %r" % (text, p.text if p else "EOF"))
        self.i += 1
        return p

    def group(self, open_="(", close=")"):
        """Consume a balanced group starting at the current token; return
        the inner tokens."""
        start = self.expect(open_)
        depth = 1
        j = self.i
        while j < len(self.t):
            tx = self.t[j].text
            if self.t[j].kind == "op":
                if tx in "([{":
                    depth += 1
                elif tx in ")]}":
                    depth -= 1
                    if depth == 0:
                        inner = self.t[self.i:j]
                        self.i = j + 1
                        return inner
            j += 1
        self.err("unbalanced '%s'" % open_, start)

    def skip_to(self, text):
        depth = 0
        while self.i < len(self.t):
            p = self.t[self.i]
            if p.kind == "op":
                if p.text in "([{":
                    depth += 1
                elif p.text in ")]}":
                    depth -= 1
                elif p.text == text and depth <= 0:
                    self.i += 1
                    return
            if p.kind == "id" and p.text in ("endmodule",) and text == ";":
                return
            self.i += 1

    def split_commas(self, toks):
        items, cur, depth = [], [], 0
        for t in toks:
            if t.kind == "op":
                if t.text in "([{":
                    depth += 1
                elif t.text in ")]}":
                    depth -= 1
                elif t.text == "," and depth == 0:
                    items.append(cur)
                    cur = []
                    continue
            cur.append(t)
        items.append(cur)
        return items

    # -- top level --
    def parse(self):
        while self.i < len(self.t):
            p = self.peek()
            if p.kind == "id" and p.text in ("module", "macromodule"):
                self.u.modules.append(self.module())
            elif p.kind == "id" and p.text in ("localparam", "parameter"):
                self.i += 1
                self.u.global_decls.extend(self.decl_list(p.text, p))
            else:
                self.i += 1
        return self.u

    def module(self):
        kw = self.peek()
        self.i += 1
        name = self.peek()
        if name is None or name.kind != "id":
            self.err("module without a name", kw)
        self.i += 1
        m = Module(name.text, kw.file, kw.line, kw.cond)
        if self.at("#"):
            self.i += 1
            inner = self.group()
            for item in self.split_commas(inner):
                item = [t for t in item if not (t.kind == "id" and
                                                t.text in ("parameter",
                                                           "localparam"))]
                nm, val = self.name_value(item)
                if nm:
                    m.params[nm.text] = (val, nm.cond)
        if self.at("("):
            inner = self.group()
            self.port_list(m, inner)
        self.expect(";")
        self.body(m)
        return m

    def name_value(self, item):
        """[type/range] NAME [= value] -> (name tok, value toks)."""
        eq = None
        for k, t in enumerate(item):
            if t.text == "=" and t.kind == "op":
                eq = k
                break
        head = item if eq is None else item[:eq]
        val = [] if eq is None else item[eq + 1:]
        nm = None
        depth = 0
        for t in head:
            if t.kind == "op" and t.text == "[":
                depth += 1
            elif t.kind == "op" and t.text == "]":
                depth -= 1
            elif depth == 0 and t.kind == "id" and t.text not in KEYWORDS:
                nm = t
        return nm, val

    def port_list(self, m, toks):
        dirn = None
        kind = "wire"
        for item in self.split_commas(toks):
            if not item:
                continue
            words = [t for t in item if t.kind == "id"]
            for w in words:
                if w.text in ("input", "output", "inout"):
                    dirn = w.text
                    kind = "wire"
                elif w.text in ("reg", "logic"):
                    kind = "reg"
            nm, _ = self.name_value(item)
            if nm is None:
                continue
            if dirn is None:
                # non-ANSI header; direction comes from the body
                m.ports.append(Port(nm.text, None, item[0].cond, nm.file,
                                    nm.line))
            else:
                m.ports.append(Port(nm.text, dirn, item[0].cond, nm.file,
                                    nm.line))
                m.decls.append(Decl(nm.text, kind, item[0].cond, nm.file,
                                    nm.line))

    def decl_list(self, kind, kwtok):
        """After a declaration keyword: names [= init], ... ;"""
        start = self.i
        self.skip_to(";")
        body = self.t[start:self.i - 1]
        out = []
        for item in self.split_commas(body):
            if not item:
                continue
            nm, val = self.name_value(item)
            if nm is None:
                continue
            out.append(Decl(nm.text, kind, C.conj(kwtok.cond, nm.cond),
                            nm.file, nm.line, val or None))
        return out

    def body(self, m):
        while self.i < len(self.t):
            p = self.peek()
            if p.kind == "id" and p.text == "endmodule":
                self.i += 1
                return
            if p.kind != "id":
                self.i += 1
                continue
            w = p.text
            if w in ("wire", "reg", "tri", "wand", "wor", "logic", "uwire",
                     "supply0", "supply1", "integer", "genvar", "real",
                     "localparam", "parameter"):
                self.i += 1
                decls = self.decl_list(w, p)
                for d in decls:
                    m.decls.append(d)
                    if w == "parameter":
                        m.params[d.name] = (d.init or [], d.cond)
                    if d.init and w in ("wire", "tri", "wand", "wor", "uwire",
                                        "logic"):
                        lhs = [Tok("id", d.name, d.line, d.cond, d.file)]
                        m.assigns.append(Assign(lhs, d.init, d.cond, d.file,
                                                d.line, "wire"))
                    if d.init:
                        self.note_reads(m, d.init, d.cond)
            elif w in ("input", "output", "inout"):
                self.i += 1
                decls = self.decl_list("wire", p)
                for d in decls:
                    pt = m.port(d.name)
                    if pt is None:
                        m.ports.append(Port(d.name, w, d.cond, d.file, d.line))
                    else:
                        pt.dir = w
                    m.decls.append(d)
            elif w == "assign":
                self.i += 1
                start = self.i
                self.skip_to(";")
                stmt = self.t[start:self.i - 1]
                for item in self.split_commas(stmt):
                    self.assign_item(m, item, p, "assign")
            elif w in ("always", "always_ff", "always_comb", "always_latch"):
                self.i += 1
                self.block_id += 1
                blk = self.block_id
                if self.at("@"):
                    self.i += 1
                    if self.at("("):
                        sens = self.group()
                        self.note_reads(m, sens, p.cond)
                    else:
                        self.i += 1
                self.stmt(m, blk, p.cond)
            elif w in ("initial", "final"):
                self.i += 1
                self.stmt(m, None, p.cond, record=False)
            elif w in ("function", "task", "generate", "specify", "table",
                       "primitive"):
                end = {"function": "endfunction", "task": "endtask",
                       "generate": "endgenerate", "specify": "endspecify",
                       "table": "endtable", "primitive": "endprimitive"}[w]
                while self.i < len(self.t) and self.peek().text != end:
                    self.i += 1
                self.i += 1
            elif w in KEYWORDS:
                self.skip_to(";")
            else:
                self.instance(m)
        self.err("module %s is missing endmodule" % m.name)

    def assign_item(self, m, item, kw, kind, blk=None):
        eq = None
        depth = 0
        for k, t in enumerate(item):
            if t.kind == "op":
                if t.text in "([{":
                    depth += 1
                elif t.text in ")]}":
                    depth -= 1
                elif depth == 0 and t.text in ("=", "<="):
                    eq = k
                    break
        if eq is None or eq == 0:
            return
        lhs, rhs = item[:eq], item[eq + 1:]
        cond = C.conj(kw.cond, item[0].cond)
        m.assigns.append(Assign(lhs, rhs, cond, item[0].file, item[0].line,
                                kind, blk))
        self.note_reads(m, rhs, cond)
        # index expressions on the lhs are reads too
        self.note_reads(m, [t for t in lhs[1:]], cond)

    def note_reads(self, m, expr, cond):
        for t in expr_idents(expr):
            m.reads.append((t.text, C.conj(cond, t.cond), t.file, t.line))

    def stmt(self, m, blk, cond, record=True):
        p = self.peek()
        if p is None:
            return
        cond = C.conj(cond, p.cond)
        w = p.text
        if p.kind == "id" and w == "begin":
            self.i += 1
            if self.at(":"):
                self.i += 2
            while self.i < len(self.t) and not self.at("end"):
                if self.at("endmodule"):
                    self.err("begin without end", p)
                self.stmt(m, blk, cond, record)
            self.i += 1
            if self.at(":"):
                self.i += 2
            return
        if p.kind == "id" and w == "if":
            self.i += 1
            c = self.group()
            if record:
                self.note_reads(m, c, cond)
            self.stmt(m, blk, cond, record)
            if self.at("else"):
                self.i += 1
                self.stmt(m, blk, cond, record)
            return
        if p.kind == "id" and w in ("case", "casez", "casex"):
            self.i += 1
            c = self.group()
            if record:
                self.note_reads(m, c, cond)
            while self.i < len(self.t) and not self.at("endcase"):
                # labels up to ':' at depth 0
                depth = 0
                while self.i < len(self.t):
                    q = self.peek()
                    if q.kind == "op" and q.text in "([{":
                        depth += 1
                    elif q.kind == "op" and q.text in ")]}":
                        depth -= 1
                    elif q.kind == "op" and q.text == ":" and depth == 0:
                        self.i += 1
                        break
                    elif q.kind == "id" and q.text == "endcase":
                        break
                    self.i += 1
                if self.at("endcase"):
                    break
                self.stmt(m, blk, cond, record)
            self.i += 1
            return
        if p.kind == "id" and w in ("for", "while", "repeat"):
            self.i += 1
            self.group()
            self.stmt(m, blk, cond, record)
            return
        if p.kind == "id" and w == "forever":
            self.i += 1
            self.stmt(m, blk, cond, record)
            return
        if p.kind == "op" and w in ("@", "#"):
            self.i += 1
            if self.at("("):
                self.group()
            else:
                self.i += 1
            self.stmt(m, blk, cond, record)
            return
        if p.kind == "op" and w == ";":
            self.i += 1
            return
        start = self.i
        self.skip_to(";")
        item = self.t[start:self.i - 1]
        if record and item and item[0].kind != "sys":
            fake = Tok("id", "", p.line, cond, p.file)
            self.assign_item(m, item, fake, "proc", blk)

    def instance(self, m):
        mod = self.peek()
        self.i += 1
        inst = Instance(mod.text, None, mod.cond, mod.file, mod.line)
        if self.at("#"):
            self.i += 1
            if self.at("("):
                inner = self.group()
                inst.params = self.connections(inner)
            else:
                self.i += 1
        nm = self.peek()
        if nm is None or nm.kind != "id":
            # not an instance after all (e.g. a stray statement); resync
            self.skip_to(";")
            return
        self.i += 1
        inst.name = nm.text
        if self.at("["):
            self.group("[", "]")
        if not self.at("("):
            self.skip_to(";")
            return
        inner = self.group()
        inst.conns = self.connections(inner)
        for c in inst.conns:
            self.note_reads(m, c.expr, C.conj(inst.cond, c.cond))
        for c in inst.params:
            self.note_reads(m, c.expr, C.conj(inst.cond, c.cond))
        if self.at(","):
            # mod a(...), b(...);  -- rare; keep the first, skip the rest
            self.skip_to(";")
        else:
            self.expect(";")
        m.instances.append(inst)

    def connections(self, toks):
        out = []
        pos = 0
        for item in self.split_commas(toks):
            if not item:
                continue
            if item[0].text == "." and len(item) >= 2:
                name = item[1]
                rest = item[2:]
                expr = []
                if rest and rest[0].text == "(":
                    # strip the outer parens
                    depth = 0
                    for k, t in enumerate(rest):
                        if t.kind == "op" and t.text == "(":
                            depth += 1
                        elif t.kind == "op" and t.text == ")":
                            depth -= 1
                            if depth == 0:
                                expr = rest[1:k]
                                break
                out.append(Conn(name.text, expr, item[0].cond, item[0].line))
            else:
                out.append(Conn(pos, item, item[0].cond, item[0].line))
                pos += 1
        return out


def read_unit(root, path, incdirs):
    toks, defines, missing = [], [], []
    lx = Lexer(root, incdirs, toks, defines, missing)
    lx.lex_file(path)
    unit = Unit(os.path.relpath(path, root))
    unit.defines = defines
    unit.missing_includes = missing
    Parser(toks, unit).parse()
    return unit
