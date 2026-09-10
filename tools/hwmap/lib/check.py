#
# Zeitlos hwmap -- checks across every define combination.
#
# A single board build only ever exercises one combination of defines,
# so a construct that is broken in some OTHER combination compiles,
# boots and ships. hwmap sees every construct with its condition, so it
# can ask the questions a build cannot:
#
#   undeclared   an identifier is used under a condition its declaration
#                does not cover (a compile error, or worse an implicit
#                1-bit net, in that combination)
#   no-ack       an address window is decoded but no slave acks it there
#                -- the CPU waits for an ack forever
#   overlap      two decoded windows claim the same address at once
#   drivers      a net has two drivers at once
#   undriven     an instance input is connected to a net nothing drives
#   unconnected  a bus initiator's address goes nowhere
#   no-mux       a decode that appears in no data/ack mux at all
#
# Each finding carries the exact combination it applies to ("when
# MONTMUL & !TRNG"). Findings are grouped by that combination, because
# a single missing guard usually shows up as several symptoms.
#

from . import cond as C
from . import model as M
from . import vparse as V

SEVERITY = {"E": 0, "W": 1, "N": 2}

# Within a severity, what a reader should see first.
KIND_RANK = {"no-ack": 0, "overlap": 1, "drivers": 2, "no-mux": 3, "undeclared": 4,
             "unconnected": 5, "undriven": 6, "no-clock": 7, "include": 8,
             "exclusive": 9, "duplicate": 10}


def _declared_parts(cubes, decl_cover):
    """Restrict cubes to where the name is declared at all."""
    out = []
    for u in cubes:
        for dc in decl_cover:
            x = C.overlap(u, dc)
            if x is not None:
                out.append(x)
    return C.absorb(out)


def _slices_overlap(a, b):
    if not isinstance(a, tuple) or not isinstance(b, tuple):
        return True
    return not (a[1] > b[0] or b[1] > a[0])


def run(model):
    n = model.n
    top = model.d.top
    out = []

    def add(sev, kind, when, msg, file=None, line=None, subject=None):
        out.append(dict(severity=sev, kind=kind, when=when, msg=msg,
                        file=file, line=line, subject=subject or msg))

    clock_nets = {e["net"] for e in model.clocks}
    params = {d.name for d in top.decls + model.d.top_unit.global_decls
              if d.kind in ("localparam", "parameter")}

    # -- undeclared ---------------------------------------------------
    seen = set()
    for name, cnd, file, line in top.reads:
        if name in V.KEYWORDS:
            continue
        cover = n.decl_cover(name)
        if not cover:
            k = (name, "never")
            if k not in seen:
                seen.add(k)
                add("E", "undeclared", cnd,
                    "%s is used but never declared" % name, file, line,
                    subject=name)
            continue
        for u in C.uncovered(cnd, cover):
            k = (name, C.fmt(u))
            if k in seen:
                continue
            seen.add(k)
            # An `elsif chain declares its branches under !earlier, so a
            # use guarded only by a later branch's own define fails when
            # an earlier one is ALSO defined. That is the chain saying
            # the defines are one-of, not a missing guard.
            pair = _exclusive_pair(u, cover)
            if pair:
                add("N", "exclusive", C.cube((pair[0], True), (pair[1], True)),
                    "%s and %s cannot both be defined (%s is declared "
                    "in an `elsif chain but used outside it)"
                    % (pair[0], pair[1], name), file, line,
                    subject="%s/%s" % tuple(sorted(pair)))
                continue
            add("E", "undeclared", u,
                "%s is used here but declared only when %s"
                % (name, M.cover_fmt(C.absorb(cover))), file, line,
                subject=name)

    # -- undriven instance inputs --------------------------------------
    seen = set()
    for inst in top.instances:
        if model.d.is_primitive(inst.module):
            continue
        for cn in inst.conns:
            if model.d.port_dir(inst.module, cn.name) != "input":
                continue
            for t in V.expr_idents(cn.expr):
                name = t.text
                dcover = n.decl_cover(name)
                if not dcover or name in params:
                    continue
                use = C.conj(inst.cond, cn.cond, t.cond)
                drv = C.absorb([d.cond for d in n.drivers.get(name, [])])
                for u in _declared_parts(C.uncovered(use, drv), dcover):
                    if name in clock_nets:
                        # a missing clock is a property of the clock, not
                        # of each block that uses it: drop the reader's
                        # own presence literals where the gap remains
                        u = _generalise(u, C.conj(inst.cond, cn.cond), drv)
                        k = (name, C.fmt(u))
                        if k in seen:
                            continue
                        seen.add(k)
                        add("W", "no-clock", u,
                            "clock %s has no source (read by %s.%s)"
                            % (name, inst.name, cn.name), inst.file, cn.line,
                            subject=name)
                        continue
                    k = (name, inst.name, C.fmt(u))
                    if k in seen:
                        continue
                    seen.add(k)
                    add("W", "undriven", u,
                        "%s.%s reads %s, which nothing drives"
                        % (inst.name, cn.name, name), inst.file, cn.line,
                        subject="%s.%s" % (inst.name, cn.name))

    # -- multiple drivers ----------------------------------------------
    for name, drvs in sorted(n.drivers.items()):
        units = []
        blocks = {}
        for d in drvs:
            if d.kind == "pin":
                continue
            if d.kind == "proc" and d.block is not None:
                key = ("proc", d.block)
                if key in blocks:
                    blocks[key]["cover"].append(d.cond)
                    blocks[key]["slices"].append(d.slice)
                    continue
                ent = dict(cover=[d.cond], slices=[d.slice], drv=d)
                blocks[key] = ent
                units.append(ent)
            else:
                units.append(dict(cover=[d.cond], slices=[d.slice], drv=d))
        for i, a in enumerate(units):
            for b in units[i + 1:]:
                if not any(_slices_overlap(x, y) for x in a["slices"]
                           for y in b["slices"]):
                    continue
                for ca in C.absorb(a["cover"]):
                    for cb in C.absorb(b["cover"]):
                        x = C.overlap(ca, cb)
                        if x is None:
                            continue
                        da, db = a["drv"], b["drv"]
                        add("E", "drivers", x,
                            "%s has two drivers: %s and %s"
                            % (name, _where(da), _where(db)), da.file, da.line)

    # -- decodes: acked, muxed, overlapping ----------------------------
    for name, dec in sorted(model.decodes.items()):
        terms = model.mux_terms.get(name, [])
        if not terms:
            add("E", "no-mux", C.TRUE if not dec["cover"] else dec["cover"][0],
                "%s is decoded but appears in no data/ack mux" % name,
                dec["file"], dec["line"])
            continue
        tcover = C.absorb([t["cond"] for t in model.tenants.get(name, [])])
        dcover = C.absorb([t["cond"] for t in terms if t["kind"] == "dat"])
        for w in dec["windows"]:
            for u in C.uncovered(w["cond"], tcover):
                add("E", "no-ack", u,
                    "%s decodes 0x%s but no slave acks it (bus hang)"
                    % (name, M.hexs(w["value"])), dec["file"], w["line"])
            for u in C.uncovered(w["cond"], dcover):
                add("E", "no-ack", u,
                    "%s decodes 0x%s but has no data mux term"
                    % (name, M.hexs(w["value"])), dec["file"], w["line"])
    names = sorted(model.decodes)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            da, db = model.decodes[a], model.decodes[b]
            if da["net"] != db["net"]:
                continue
            for wa in da["windows"]:
                for wb in db["windows"]:
                    if (wa["value"] ^ wb["value"]) & wa["mask"] & wb["mask"]:
                        continue
                    joint = C.overlap(wa["cond"], wb["cond"])
                    if joint is None:
                        continue
                    excl = [c for x, c in wa["excludes"] if x == b]
                    excl += [c for x, c in wb["excludes"] if x == a]
                    for u in C.uncovered(joint, excl):
                        add("E", "overlap", u,
                            "%s (0x%s) and %s (0x%s) decode the same addresses"
                            % (a, M.hexs(wa["value"]), b, M.hexs(wb["value"])),
                            da["file"], wb["line"])

    # -- initiators that reach nothing ----------------------------------
    for b in model.blocks:
        if b.primitive:
            continue
        for pre, i in sorted(b.ifaces.items()):
            if i["role"] != "initiator":
                continue
            port = pre + "adr_o"
            reached = []
            for ident, ic in n.conn_idents(b.inst, port):
                for t, c in n.forward(ident, ic, stop=model._is_bus_net):
                    if t[0] in ("inst", "stop"):
                        reached.append(c)
            base = b.cond
            c0 = n.conn(b.inst, port)
            if c0 is not None:
                base = C.conj(base, c0.cond)
            for u in C.uncovered(base, C.absorb(reached)):
                add("W", "unconnected", u,
                    "%s (%s) initiator %s* is connected to no bus"
                    % (model.h.name(b), b.inst.name, pre), b.file, b.line)

    # -- housekeeping ----------------------------------------------------
    for inc, cnd, file, line in model.d.top_unit.missing_includes:
        if not cnd:
            add("W", "include", cnd, "`include \"%s\" not found" % inc, file, line)
    for name, f1, f2 in model.d.dup_modules:
        add("N", "duplicate", C.TRUE,
            "module %s is defined in both %s and %s; using the first"
            % (name, f1, f2))
    return out


def _exclusive_pair(u, cover):
    for n_, v in sorted(u):
        if not v:
            continue
        if cover and all((n_, False) in c for c in cover):
            others = sorted(x for x, xv in u if xv and x != n_)
            for c in cover:
                pos = sorted(x for x, xv in c if xv)
                if pos:
                    return (pos[0], n_)
            if others:
                return (others[0], n_)
    return None


def _generalise(u, reader_cond, drv_cover):
    names_in_drv = set()
    for c in drv_cover:
        names_in_drv |= C.names(c)
    for lit in sorted(u):
        if lit in reader_cond and lit[0] not in names_in_drv:
            cand = u - {lit}
            if all(C.overlap(cand, d) is None for d in drv_cover):
                u = cand
    return u


def _where(d):
    if d.kind == "inst":
        return "%s.%s (line %d)" % (d.inst.name, d.port, d.line)
    return "%s (line %d)" % ({"assign": "assign", "wire": "wire init",
                              "proc": "always block"}.get(d.kind, d.kind), d.line)


def group(findings):
    # the same subject reported under a narrower combination than
    # another finding already covers adds nothing
    keep = []
    for f in findings:
        if any(g is not f and g["kind"] == f["kind"] and
               g["subject"] == f["subject"] and g["when"] < f["when"]
               for g in findings):
            continue
        keep.append(f)
    findings = keep
    groups = {}
    for f in findings:
        k = C.fmt(f["when"])
        g = groups.setdefault(k, dict(when=f["when"], items=[], severity="N"))
        if (f["kind"], f["msg"]) not in [(x["kind"], x["msg"]) for x in g["items"]]:
            g["items"].append(f)
        if SEVERITY[f["severity"]] < SEVERITY[g["severity"]]:
            g["severity"] = f["severity"]
    out = list(groups.values())
    for g in out:
        g["items"].sort(key=lambda f: (SEVERITY[f["severity"]], f["kind"],
                                       f["line"] or 0))
    for g in out:
        g["items"].sort(key=lambda f: (SEVERITY[f["severity"]],
                                       KIND_RANK.get(f["kind"], 99), f["line"] or 0))
    out.sort(key=lambda g: (SEVERITY[g["severity"]],
                            min(KIND_RANK.get(f["kind"], 99) for f in g["items"]),
                            len(g["when"]), C.fmt(g["when"])))
    return out
