#
# Zeitlos hwmap -- the model as plain data.
#
# One canonical, deterministic form of everything hwmap concluded from
# the RTL: sorted keys, conditions as strings, no absolute paths, no
# timestamps, no git state. `hwmap --json` writes it; the golden
# self-test compares it; `hwmap --diff` shows how it moved.
#

import hashlib
import json
import os

from . import cond as C
from . import model as M


def _cover(cubes):
    return M.cover_fmt(C.absorb(cubes))


def _blk(b):
    return None if b is None else b.id


def _tree(f):
    if f is None:
        return None
    if f["kind"] == "alts":
        return dict(kind="alts", options=[_tree(o) for o in f["options"]])
    out = dict(kind=f["kind"], bypass=f["bypass"],
               blocks=[dict(id=b.id, when=C.fmt(b.cond)) for b in f.get("blocks", [])])
    if f.get("iface"):
        out["iface"] = f["iface"]
    if "inputs" in f:
        out["inputs"] = [dict(port=i["port"], node=_tree(i["node"])) for i in f["inputs"]]
    if "up" in f:
        out["up"] = _tree(f["up"])
    if not f.get("blocks"):
        out["name"] = f.get("name")
    return out


def input_hash(design):
    h = hashlib.sha256()
    for rel in sorted(design.files + ["Makefile"]):
        p = os.path.join(design.root, rel)
        if os.path.exists(p):
            h.update(rel.encode() + b"\0")
            with open(p, "rb") as f:
                h.update(f.read())
    # includes are read through the top unit; hash rtl/*.vh as well
    rtl = os.path.join(design.root, "rtl")
    for name in sorted(os.listdir(rtl)):
        if name.endswith(".vh"):
            with open(os.path.join(rtl, name), "rb") as f:
                h.update(name.encode() + b"\0" + f.read())
    return h.hexdigest()


def model_dict(m, groups):
    d = m.d
    out = dict(
        format="zeitlos-hwmap-model/1",
        top=d.top.file,
        files=list(d.files),
        blocks=[dict(id=b.id, instance=b.inst.name, module=b.module, when=C.fmt(b.cond),
                     line=b.line, roles=sorted(b.roles), primitive=b.primitive,
                     interfaces={p: i["role"] for p, i in sorted(b.ifaces.items())},
                     alt_group=b.alt_group,
                     children=[dict(module=c[0], instance=c[1], when=C.fmt(c[2]))
                               for c in b.children])
                for b in m.blocks],
        buses=[dict(net=bus["net"], decodes=bus["decodes"]) for bus in m.buses],
        decodes={name: dict(
            windows=[dict(base="0x" + M.hexs(w["value"]).replace("_", ""),
                          mask="0x" + M.hexs(w["mask"]).replace("_", ""),
                          when=C.fmt(w["cond"]), line=w["line"],
                          excludes=[dict(decode=x, when=C.fmt(c)) for x, c in w["excludes"]])
                     for w in dec["windows"]],
            tenants=[dict(block=_blk(t["block"]), iface=t["iface"], inline=t["inline"],
                          when=C.fmt(t["cond"]))
                     for t in m.tenants.get(name, [])],
            mux=[dict(kind=t["kind"], signal=t["sig"], when=C.fmt(t["cond"]), line=t["line"])
                 for t in m.mux_terms.get(name, [])])
            for name, dec in sorted(m.decodes.items())},
        main_tree=_tree(m.main_tree),
        secondary=[dict(target=s["block"].id, iface=s["iface"], tree=_tree(s["tree"]))
                   for s in m.secondary],
        ghost_windows=[dict(block=g["block"].id, base="0x%08X" % g["value"],
                            param=g["param"], decode=g["decode"], when=C.fmt(g["cond"]))
                       for g in m.ghosts],
        virtual_windows=[dict(block=v["block"].id, base="0x%08X" % v["value"],
                              mask="0x%08X" % v["mask"], when=C.fmt(v["cond"]))
                         for v in m.virtual],
        irqs=[dict(n=i["n"], signal=i["signal"], when=C.fmt(i["cond"]), line=i["line"],
                   sources=[dict(block=_blk(s.get("block")), port=s.get("port"),
                                 pin=s.get("pin"), logic=s.get("logic"),
                                 when=C.fmt(s["cond"])) for s in i["sources"]])
              for i in m.irqs],
        clocks=[dict(net=e["net"], mhz=e["mhz"],
                     sources=[dict(block=_blk(s.get("block")), port=s.get("port"),
                                   pin=s.get("pin"), logic=s.get("logic"),
                                   when=C.fmt(s["cond"])) for s in e["sources"]],
                     users=sorted({u["block"].id for u in e["users"]}))
                for e in m.clocks],
        pins=[dict(label=g["label"], block=_blk(g["block"]), kind=g["kind"],
                   when=C.fmt(g["cond"]), ports=[p.name for p in g["ports"]])
              for g in m.pin_groups],
        links=[dict(src=l["src"].id, dst=l["dst"].id, signals=l["signals"],
                    when=M.cover_fmt(l["cover"])) for l in m.links],
        alternatives=[dict(kind=g["kind"], key=g["key"], blocks=[b.id for b in g["blocks"]])
                      for g in m.alt_groups],
        defaults={k: v[0] for k, v in sorted(m.defaults.items())},
        features={k: v for k, v in sorted(m.features.items())},
        findings=[dict(when=C.fmt(g["when"]), severity=g["severity"],
                       items=[dict(severity=it["severity"], kind=it["kind"], msg=it["msg"],
                                   file=it["file"], line=it["line"]) for it in g["items"]])
                  for g in groups],
    )
    return out


def dumps(obj):
    return json.dumps(obj, indent=1, sort_keys=True) + "\n"
