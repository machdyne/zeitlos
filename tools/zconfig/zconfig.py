#!/usr/bin/env python3
#
# tools/zconfig/zconfig.py -- the system configurator (docs/zconfig.md).
#
#   make config                     the menus (also `make menu-config`)
#   zconfig.py [target]             the menus, starting from a target
#   zconfig.py --report <target>    what a target is estimated to cost
#
# A target is a board, the PMODs plugged into it, and define changes
# (release/targets/*.spec, docs/releases.md). The menus change the
# define set; Save writes it back as a spec -- the difference from the
# board and its PMODs -- so `zrelease build --targets <name>` builds it
# like any other. The estimates come from tools/zconfig/catalog.cfg;
# Measure synthesises the saved target for the real numbers
# (tools/zconfig/measure.py).
#
import configparser
import curses
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))
import spec as zspec       # noqa: E402
import mkfatimg            # noqa: E402

DEVICES = {"12k": (12288, 56, 28), "25k": (24288, 56, 28),
           "45k": (43848, 108, 72), "85k": (83640, 208, 156)}
WARN_PERCENT = 95          # release/lib/build.py's UTIL_WARN_PERCENT

# Defines a target cannot take: rtl/boards.vh sets them for every board,
# and only an off flag (catalog `off`) removes them.
UNIVERSAL = None


def universal():
    global UNIVERSAL
    if UNIVERSAL is None:
        UNIVERSAL = zspec.universal_defines(ROOT)
    return UNIVERSAL


# ----------------------------------------------------------------------
# the catalog
# ----------------------------------------------------------------------

class Entry:
    def __init__(self, key, sec):
        self.key = key
        self.menu = sec.get("menu", "Other")
        self.title = sec.get("title", key)
        self.define = sec.get("define", "") or None
        self.off = sec.get("off", "") or None
        self.choice = sec.get("choice", "") or None
        self.values = sec.get("values", "").split()
        self.rule = sec.get("rule", "") or None
        self.lut = int(sec.get("lut", "0"))
        self.bram = int(sec.get("bram", "0"))
        self.dsp = int(sec.get("dsp", "0"))
        self.vcost = {}
        for v in self.values:
            self.vcost[v] = (int(sec.get("lut." + v, "0")),
                             int(sec.get("bram." + v, "0")),
                             int(sec.get("dsp." + v, "0")))
        self.with_ = {}
        for k in sec:
            m = re.match(r"(lut|bram|dsp)_with\.(\w+)$", k)
            if m:
                self.with_.setdefault(m.group(2), [0, 0, 0])[
                    ("lut", "bram", "dsp").index(m.group(1))] = int(sec[k])
        self.requires = sec.get("requires", "").split()
        self.needs_any = sec.get("needs_any", "").split()
        self.conflicts = sec.get("conflicts", "").split()
        self.pins = sec.get("pins", "") or None
        if self.pins == "yes" and self.choice:
            self.pins = "choice:" + self.choice
        self.families = sec.get("families", "").split()
        self.help = sec.get("help", "")


class Catalog:
    def __init__(self, path=os.path.join(HERE, "catalog.cfg")):
        cp = configparser.ConfigParser(interpolation=None)
        cp.optionxform = str
        with open(path) as f:
            cp.read_file(f)
        self.entries = [Entry(k, cp[k]) for k in cp.sections()]
        self.by_key = {e.key: e for e in self.entries}
        self.menus = []
        for e in self.entries:
            if e.menu != "-" and e.menu not in self.menus:
                self.menus.append(e.menu)
        self.defines = {e.define for e in self.entries if e.define}

    def in_menu(self, menu):
        return [e for e in self.entries if e.menu == menu]

    def choice(self, name):
        return [e for e in self.entries if e.choice == name]


# ----------------------------------------------------------------------
# a configuration: a board, PMODs, a define set
# ----------------------------------------------------------------------

def boards():
    d = os.path.join(ROOT, "release", "hw", "boards")
    return sorted(f[:-5] for f in os.listdir(d) if f.endswith(".spec"))


def pmods_available():
    d = os.path.join(ROOT, "release", "hw", "pmods")
    return sorted(f[:-5] for f in os.listdir(d) if f.endswith(".spec"))


def compose(base, pmods, defines_ops=(), extra=None):
    """load_target() on a scratch spec: (Target, error or None)."""
    name = "_zconfig_%d" % os.getpid()
    path = os.path.join(ROOT, "release", "targets", name + ".spec")
    text = "base = %s\n" % base
    if pmods:
        text += "pmods = %s\n" % " ".join(pmods)
    if defines_ops:
        text += "defines =\n" + "".join("\t%s\n" % o for o in defines_ops)
    with open(path, "w") as f:
        f.write(text)
    try:
        return zspec.load_target(ROOT, name), None
    except zspec.SpecError as e:
        return None, str(e).replace(path, "the target")
    finally:
        os.remove(path)


class Config:
    def __init__(self, catalog):
        self.cat = catalog
        self.name = "custom"
        self.description = ""
        self.board = None          # release/hw/boards/<board>.spec
        self.pmods = []            # "langkatze@a"
        self.defines = {}          # composed: name -> value or None
        self.base_defines = {}     # the board and its PMODs alone
        self.family = None
        self.device = None
        self.card_sets = ["base"]
        self.card_packs = []
        self.error = None
        self.from_target = None
        # a target's keys the menus do not change, kept as they were
        self.extra = {}            # make_vars, lpf_drop, notes: [lines]
        self.make_vars = []

    # -- loading ---------------------------------------------------------

    def load_board(self, board, pmods=()):
        t, err = compose(board, list(pmods))
        if err:
            self.error = err
            return False
        self.board = board
        self.pmods = list(pmods)
        self.extra = {}
        self.base_defines = dict(t.defines)
        self.defines = dict(t.defines)
        self.family = t.family
        self.device = make_value(t, "DEVICE") or "25k"
        self.make_board = t.board
        self.error = None
        return True

    def load_target(self, name):
        tpath = os.path.join(ROOT, "release", "targets", name + ".spec")
        data = zspec.parse_file(tpath)
        board = data["base"][0]
        pmods = []
        for chunk in data.get("pmods", []):
            pmods += chunk.split()
        if not self.load_board(board, pmods):
            return False
        t = zspec.load_target(ROOT, name)
        self.defines = dict(t.defines)
        self.extra = {k: list(data[k]) for k in ("make_vars", "lpf_drop", "notes")
                      if k in data}
        self.make_vars = t.make_vars
        self.device = make_value(t, "DEVICE") or self.device
        self.name = name
        self.from_target = name
        self.description = t.description if t.description != name else ""
        self.card_sets = sum((c.split() for c in data.get("card_sets", [])), []) or ["base"]
        self.card_packs = sum((c.split() for c in data.get("card_packs", [])), [])
        return True

    # -- the define set as the build sees it -----------------------------

    def effective(self):
        """Composed defines, the universal ones less their off flags, and
        rtl/boards.vh's tail rules (USB_CDC displaces UART0)."""
        d = dict(self.defines)
        for k, v in universal().items():
            d.setdefault(k, v)
        for e in self.cat.entries:
            if e.off and e.off in d:
                d.pop(e.define, None)
        eff = zspec.effective_defines(ROOT, d.keys())
        return {k: d.get(k) for k in eff}

    # -- one entry -------------------------------------------------------

    def is_on(self, e, eff=None):
        eff = self.effective() if eff is None else eff
        if e.choice and not e.define:
            others = [o.define for o in self.cat.choice(e.choice) if o.define]
            return not any(o in eff for o in others)
        if not e.define:
            return True
        return e.define in eff

    def value(self, e):
        v = self.defines.get(e.define)
        return v if v is not None else None

    def pins_ok(self, e):
        """Can the board (with its PMODs) take this, if it is pin-bound?"""
        if not e.pins:
            return True
        if e.define and e.define in self.base_defines:
            return True
        if e.pins.startswith("choice:"):
            group = e.pins[len("choice:"):]
            # USB_HID and USB_HOST use the same pins; the NICs do not
            if group == "usb":
                return any(o.define in self.base_defines
                           for o in self.cat.choice(group) if o.define)
            return False
        return False

    def family_ok(self, e):
        return not e.families or self.family in e.families

    def available(self, e):
        return self.pins_ok(e) and self.family_ok(e)

    def set_on(self, e, on):
        if e.choice:
            for o in self.cat.choice(e.choice):
                if o.define:
                    self.defines.pop(o.define, None)
            if e.define:
                self.defines[e.define] = None
            return
        if not e.define:
            return
        if e.off:
            if on:
                self.defines.pop(e.off, None)
            else:
                self.defines[e.off] = None
            return
        if on:
            if e.values:
                self.defines[e.define] = self.base_defines.get(e.define) or e.values[-1]
            else:
                self.defines[e.define] = self.base_defines.get(e.define)
        else:
            self.defines.pop(e.define, None)

    def cycle_value(self, e):
        if not e.values:
            return
        cur = self.defines.get(e.define)
        opts = list(e.values)
        if cur not in opts:
            self.defines[e.define] = opts[0]
        else:
            i = opts.index(cur)
            if i + 1 < len(opts):
                self.defines[e.define] = opts[i + 1]
            else:
                self.defines.pop(e.define, None)

    # -- what it costs ---------------------------------------------------

    def vram_bram(self, eff):
        if "MEM_VRAM" not in eff:
            return 0
        planes = int(eff.get("GAME_PLANES") or 0) if "COLOR" in eff else 0
        if "ZEITLOS_CONSOLE" in eff or "VRAM_SDRAM" in eff:
            return 5 * max(planes, 2)
        return 5 * max(4, planes)

    def cost(self, e, eff):
        lut, bram, dsp = e.lut, e.bram, e.dsp
        if e.values:
            v = eff.get(e.define)
            vl, vb, vd = e.vcost.get(str(v), (0, 0, 0))
            lut, bram, dsp = lut + vl, bram + vb, dsp + vd
        for d, (wl, wb, wd) in e.with_.items():
            if d in eff:
                lut, bram, dsp = lut + wl, bram + wb, dsp + wd
        if e.rule == "vram":
            bram += self.vram_bram(eff)
        return lut, bram, dsp

    def costs(self):
        """[(entry, lut, bram, dsp)] for what is on, and the totals."""
        eff = self.effective()
        rows = []
        core = self.cat.by_key.get("core")
        if core:
            rows.append((core, core.lut, core.bram, core.dsp))
        for e in self.cat.entries:
            if e.menu == "-" or not self.is_on(e, eff):
                continue
            lut, bram, dsp = self.cost(e, eff)
            if lut or bram or dsp:
                rows.append((e, lut, bram, dsp))
        tot = (sum(r[1] for r in rows), sum(r[2] for r in rows),
               sum(r[3] for r in rows))
        return rows, tot

    def capacity(self):
        return DEVICES.get(self.device, (None, None, None))

    def problems(self):
        eff = self.effective()
        out = []
        for e in self.cat.entries:
            if e.menu == "-" or not self.is_on(e, eff) or not e.define or e.values:
                continue
            for r in e.requires:
                if r not in eff:
                    out.append("%s needs %s" % (e.title, r))
            if e.needs_any and not any(r in eff for r in e.needs_any):
                out.append("%s needs one of %s" % (e.title, ", ".join(e.needs_any)))
            for c in e.conflicts:
                if c in eff:
                    out.append("%s cannot be built with %s" % (e.title, c))
            if not self.available(e):
                out.append("%s: %s has no pins for it" % (e.title, self.board))
        if self.family == "ecp5":
            _rows, (lut, bram, dsp) = self.costs()
            cl, cb, cd = self.capacity()
            if cl and lut > cl:
                out.append("about %d LUTs, more than the %d there are" % (lut, cl))
            if cb and bram > cb:
                out.append("about %d block RAMs, more than the %d there are" % (bram, cb))
            if cd and dsp > cd:
                out.append("about %d multipliers, more than the %d there are" % (dsp, cd))
        if "GAME_CHUNKY" in eff and "VRAM_SDRAM" not in eff and \
                int(eff.get("GAME_PLANES") or 0) < 8:
            out.append("chunky game mode needs 8 planes of VRAM, or VRAM_SDRAM")
        return out

    # -- the spec --------------------------------------------------------

    def ops(self):
        """The define changes from the board and its PMODs."""
        out = []
        for k in self.base_defines:
            if k not in self.defines:
                out.append("-" + k)
        for k, v in self.defines.items():
            if k not in self.base_defines or self.base_defines[k] != v:
                out.append(k if v is None else "%s=%s" % (k, v))
        return out

    def spec_text(self):
        _rows, (lut, bram, dsp) = self.costs()
        cl, cb, _cd = self.capacity()
        lines = ["# %s -- made with `make config` (tools/zconfig, docs/zconfig.md)." % self.name,
                 "#"]
        if self.from_target:
            lines.append("# Started from %s." % self.from_target)
        if self.family == "ecp5" and cl:
            lines.append("# Estimated: about %d LUTs of %d (%d%%), %d block RAMs of %d."
                         % (lut, cl, 100 * lut // cl, bram, cb))
        lines.append("")
        lines.append("description = %s" % (self.description or
                                           "%s, configured" % self.board))
        lines.append("")
        lines.append("base = %s" % self.board)
        if self.pmods:
            lines.append("pmods = %s" % " ".join(self.pmods))
        ops = self.ops()
        if ops:
            lines.append("")
            lines.append("defines =")
            lines += ["\t" + o for o in ops]
        for k in ("make_vars", "lpf_drop", "notes"):
            if k in self.extra:
                lines.append("")
                vals = self.extra[k]
                if len(vals) == 1:
                    lines.append("%s = %s" % (k, vals[0]))
                else:
                    lines.append("%s =" % k)
                    lines += ["\t" + v for v in vals]
        lines.append("")
        lines.append("# the SD card made with it (zrelease sdcard --sets; ask packs)")
        lines.append("card_sets = %s" % " ".join(self.card_sets))
        if self.card_packs:
            lines.append("card_packs = %s" % " ".join(self.card_packs))
        return "\n".join(lines) + "\n"

    def spec_path(self):
        return os.path.join(ROOT, "release", "targets", self.name + ".spec")

    def save(self):
        with open(self.spec_path(), "w") as f:
            f.write(self.spec_text())
        try:
            zspec.load_target(ROOT, self.name)
        except zspec.SpecError as e:
            return str(e)
        return None


def make_value(target, var):
    common = ["BOARD=" + target.board, "EXTRA_DEFINES=-DZSPEC"] + target.make_vars
    r = subprocess.run(["make", "-s", "--no-print-directory", "-C", ROOT] + common +
                       ["--eval", "zc-print: ; @echo $(%s)" % var, "zc-print"],
                       capture_output=True, text=True)
    lines = r.stdout.strip().splitlines()
    return lines[-1].strip() if lines else ""


def os_version():
    with open(os.path.join(ROOT, "sw", "common", "zversion.h")) as f:
        m = re.search(r'#define\s+Z_OS_VERSION\s+"([^"]*)"', f.read())
    return m.group(1) if m else "0.0.0"


# ----------------------------------------------------------------------
# the report (no menus)
# ----------------------------------------------------------------------

def report(cfg, out=sys.stdout):
    rows, (lut, bram, dsp) = cfg.costs()
    cl, cb, cd = cfg.capacity()
    print("%s: %s%s, %s %s" % (cfg.name, cfg.board,
                               (" + " + " ".join(cfg.pmods)) if cfg.pmods else "",
                               cfg.family, cfg.device), file=out)
    if cfg.family != "ecp5":
        print("  (estimates are for ECP5 only)", file=out)
    print("  %-46s %7s %5s %4s" % ("", "LUT", "BRAM", "DSP"), file=out)
    for e, l, b, d in sorted(rows, key=lambda r: -r[1]):
        name = e.title
        if e.values:
            name += " = %s" % cfg.effective().get(e.define)
        print("  %-46s %7d %5d %4d" % (name[:46], l, b, d), file=out)
    print("  %-46s %7d %5d %4d" % ("estimated total", lut, bram, dsp), file=out)
    if cl:
        print("  %-46s %7d %5d %4d   (%d%% of the LUTs)" %
              ("the device", cl, cb, cd, 100 * lut // cl), file=out)
    for p in cfg.problems():
        print("  ! " + p, file=out)


# ----------------------------------------------------------------------
# the menus
# ----------------------------------------------------------------------

class UI:
    def __init__(self, scr, cfg):
        self.scr = scr
        self.cfg = cfg
        self.cat = cfg.cat
        self.msg = ""
        self.measured = None
        curses.curs_set(0)
        try:
            curses.start_color()
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_BLACK, curses.COLOR_CYAN)    # title, status
            curses.init_pair(2, curses.COLOR_YELLOW, -1)                  # warnings
            curses.init_pair(3, curses.COLOR_RED, -1)                     # over
            curses.init_pair(4, curses.COLOR_GREEN, -1)                   # bar
        except curses.error:
            pass

    # -- drawing ---------------------------------------------------------

    def put(self, y, x, text, attr=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h and x < w:
            try:
                self.scr.addnstr(y, x, text, max(0, w - x - 1), attr)
            except curses.error:
                pass

    def frame(self, title, items, sel, top, help_text):
        scr = self.scr
        scr.erase()
        h, w = scr.getmaxyx()
        bar = " Zeitlos configuration -- %s (%s%s) " % (
            self.cfg.name, self.cfg.board or "no board",
            ", %s" % self.cfg.device if self.cfg.device else "")
        self.put(0, 0, bar.ljust(w), curses.color_pair(1))
        self.put(1, 1, title, curses.A_BOLD)
        rows = h - 7
        for i in range(top, min(len(items), top + rows)):
            text, enabled = items[i][0], items[i][1]
            attr = curses.A_REVERSE if i == sel else 0
            if not enabled:
                attr |= curses.A_DIM
            self.put(3 + i - top, 2, text, attr)
        if len(items) > rows:
            self.put(2, w - 12, "%d/%d" % (sel + 1, len(items)), curses.A_DIM)
        self.status(h, w, help_text)
        scr.refresh()

    def status(self, h, w, help_text):
        cfg = self.cfg
        _rows, (lut, bram, dsp) = cfg.costs()
        cl, cb, cd = cfg.capacity()
        probs = cfg.problems()
        y = h - 4
        self.put(y, 0, "-" * (w - 1), curses.A_DIM)
        if cfg.family == "ecp5" and cl:
            pct = 100 * lut // cl
            colour = curses.color_pair(3) if pct > 100 else \
                curses.color_pair(2) if pct >= WARN_PERCENT else curses.color_pair(4)
            n = 20
            fill = min(n, n * lut // cl)
            line = "LUT ~%d / %d (%d%%) [%s%s]   BRAM %d / %d   DSP %d / %d" % (
                lut, cl, pct, "#" * fill, "." * (n - fill), bram, cb, dsp, cd)
            self.put(y + 1, 1, line, colour | curses.A_BOLD)
        else:
            self.put(y + 1, 1, "no estimates for %s (ECP5 only)" % cfg.family)
        if self.measured:
            self.put(y + 1, w - len(self.measured) - 2, self.measured, curses.A_DIM)
        if probs:
            self.put(y + 2, 1, "! " + probs[0] + (" (+%d more)" % (len(probs) - 1)
                                                if len(probs) > 1 else ""),
                     curses.color_pair(2))
        else:
            self.put(y + 2, 1, self.msg or help_text, curses.A_DIM)
        self.put(h - 1, 1, "Up/Down move  Enter/Space choose  Esc back  "
                 "s save  b build  m measure  r report  ? help  q quit", curses.A_DIM)

    # -- a generic list --------------------------------------------------

    def run_list(self, title, build_items, on_select, help_of=None, on_key=None,
                 top_level=False):
        """build_items() -> [(text, enabled, payload)]; on_select(payload)
        returns True to leave the list."""
        sel = top = 0
        while True:
            items = build_items()
            if not items:
                return
            sel = max(0, min(sel, len(items) - 1))
            h, _w = self.scr.getmaxyx()
            rows = max(1, h - 7)
            if sel < top:
                top = sel
            if sel >= top + rows:
                top = sel - rows + 1
            help_text = help_of(items[sel][2]) if help_of else ""
            self.frame(title, items, sel, top, help_text)
            k = self.scr.getch()
            self.msg = ""
            if k in (curses.KEY_UP, ord("k")):
                sel = (sel - 1) % len(items)
            elif k in (curses.KEY_DOWN, ord("j")):
                sel = (sel + 1) % len(items)
            elif k == curses.KEY_PPAGE:
                sel = max(0, sel - rows)
            elif k == curses.KEY_NPAGE:
                sel = min(len(items) - 1, sel + rows)
            elif k in (27, curses.KEY_LEFT, curses.KEY_BACKSPACE, 127):
                if not top_level or self.confirm_quit():
                    return
            elif k in (10, 13, ord(" "), curses.KEY_RIGHT):
                if items[sel][1] and on_select(items[sel][2]):
                    return
            elif k == ord("?"):
                self.show_text("Help", (help_of(items[sel][2]) if help_of else "") +
                               "\n\n" + HELP)
            elif k == ord("q"):
                if self.confirm_quit():
                    raise SystemExit(0)
            elif k == ord("s"):
                self.do_save()
            elif k == ord("b"):
                self.do_build()
            elif k == ord("m"):
                self.do_measure()
            elif k == ord("r"):
                self.do_report()
            elif on_key:
                on_key(k, items[sel][2])

    # -- the main menu ---------------------------------------------------

    def main(self):
        def items():
            c = self.cfg
            out = [("Target name:  %s" % c.name, True, "name"),
                   ("Description:  %s" % (c.description or "-"), True, "desc"),
                   ("Start from a target ...", True, "from"),
                   ("Board:        %s" % (c.board or "-"), True, "board"),
                   ("PMODs:        %s" % (" ".join(c.pmods) or "none"), True, "pmods"),
                   ("", False, None)]
            for m in self.cat.menus:
                out.append(("%-26s >" % m, True, ("menu", m)))
            out.append(("%-26s >" % "Card: app sets", True, "sets"))
            out.append(("%-26s >" % "Card: packages", True, "packs"))
            out.append(("%-26s >" % "Other defines (the board's)", True, "other"))
            out.append(("", False, None))
            out.append(("Estimate, block by block", True, "report"))
            out.append(("Save as release/targets/%s.spec" % c.name, True, "save"))
            out.append(("Build it ...", True, "build"))
            out.append(("Measure it (real synthesis) ...", True, "measure"))
            out.append(("Quit", True, "quit"))
            return out

        def select(p):
            if p == "name":
                v = self.ask("Target name (letters, digits, _): ", self.cfg.name)
                if v and re.match(r"^[a-z0-9_]+$", v):
                    self.cfg.name = v
                elif v:
                    self.msg = "a target name is lower-case letters, digits and _"
            elif p == "desc":
                v = self.ask("Description: ", self.cfg.description)
                if v is not None:
                    self.cfg.description = v
            elif p == "from":
                self.pick_target()
            elif p == "board":
                self.pick_board()
            elif p == "pmods":
                self.pick_pmods()
            elif isinstance(p, tuple) and p[0] == "menu":
                self.component_menu(p[1])
            elif p == "sets":
                self.pick_sets()
            elif p == "packs":
                self.pick_packs()
            elif p == "other":
                self.other_defines()
            elif p == "report":
                self.do_report()
            elif p == "save":
                self.do_save()
            elif p == "build":
                self.do_build()
            elif p == "measure":
                self.do_measure()
            elif p == "quit":
                if self.confirm_quit():
                    return True
            return False

        def help_of(p):
            return {"from": "load an existing target's board, PMODs and defines",
                    "board": "the FPGA board: its pins and its defaults",
                    "pmods": "what is plugged into the board's PMOD ports",
                    "sets": "which app sets go on the SD card (docs/update.md, \"Sets\")",
                    "packs": "which ask packs (documents, the Ark) go on the card",
                    "other": "defines that come with the board and are not in the catalog",
                    "build": "zrelease build for this target, and its card"}.get(p, "")

        if not self.cfg.board:
            self.pick_board()
        self.run_list("Main menu", items, select, help_of, top_level=True)

    # -- components ------------------------------------------------------

    def component_menu(self, menu):
        cfg = self.cfg

        def items():
            eff = cfg.effective()
            out = []
            for e in self.cat.in_menu(menu):
                on = cfg.is_on(e, eff)
                avail = cfg.available(e)
                if e.choice:
                    mark = "(*)" if on else "( )"
                elif e.values:
                    v = eff.get(e.define)
                    mark = "<%s>" % (v if v is not None else "-")
                else:
                    mark = "[*]" if on else "[ ]"
                cost = self.cost_text(e, eff)
                note = "" if avail else "  (no pins on this board)"
                miss = [r for r in e.requires if r not in eff]
                if on and miss and avail:
                    note = "  (needs %s)" % " ".join(miss)
                out.append(("%-5s %-50s %s%s" % (mark, e.title[:50], cost, note),
                            avail or on, e))
            return out

        def select(e):
            eff = cfg.effective()
            if e.values:
                cfg.cycle_value(e)
            elif e.choice:
                cfg.set_on(e, True)
            else:
                cfg.set_on(e, not cfg.is_on(e, eff))
            return False

        def help_of(e):
            bits = [e.help] if e.help else []
            if e.define:
                bits.append("define %s%s" % (e.define, (" (off: %s)" % e.off) if e.off else ""))
            if e.requires:
                bits.append("needs " + " ".join(e.requires))
            return "; ".join(bits)

        self.run_list(menu, items, select, help_of)

    def cost_text(self, e, eff):
        lut, bram, dsp = self.cfg.cost(e, eff)
        parts = []
        if lut:
            parts.append("%6s LUT" % ("~%d" % lut))
        if bram:
            parts.append("%3d BRAM" % bram)
        if dsp:
            parts.append("%2d DSP" % dsp)
        return "  ".join(parts)

    def other_defines(self):
        cfg = self.cfg
        known = self.cat.defines | {e.off for e in self.cat.entries if e.off}

        def items():
            out = []
            for k, v in cfg.defines.items():
                if k in known:
                    continue
                out.append(("[*] %s%s" % (k, "" if v is None else " = " + v), True, k))
            return out or [("(none)", False, None)]

        def select(k):
            if k is None:
                return False
            v = self.ask("%s = (empty: no value, '-' to remove): " % k,
                         cfg.defines.get(k) or "")
            if v == "-":
                cfg.defines.pop(k, None)
            elif v is not None:
                cfg.defines[k] = v or None
            return False

        def on_key(k, payload):
            if k == ord("a"):
                v = self.ask("Add a define (NAME or NAME=VALUE): ", "")
                if v:
                    n, _, val = v.partition("=")
                    cfg.defines[n.strip()] = val.strip() or None

        self.msg = "a: add a define"
        self.run_list("Other defines", items, select,
                      lambda k: "Enter: change or remove; a: add one", on_key)

    # -- board, target, PMODs ---------------------------------------------

    def pick_board(self):
        def items():
            return [("%s %s" % ("(*)" if b == self.cfg.board else "( )", b), True, b)
                    for b in boards()]

        def select(b):
            if self.cfg.load_board(b, []):
                self.cfg.from_target = None
                self.msg = "board %s: its defaults" % b
            else:
                self.msg = self.cfg.error
            return True

        self.run_list("Board", items, select)

    def pick_target(self):
        def items():
            return [(t, True, t) for t in zspec.list_targets(ROOT)
                    if not t.startswith("_zconfig")]

        def select(t):
            if self.cfg.load_target(t):
                self.msg = "started from %s" % t
                if t == self.cfg.name:
                    self.msg += " (Save overwrites it)"
            else:
                self.msg = self.cfg.error
            return True

        self.run_list("Start from a target", items, select)

    def pick_pmods(self):
        cfg = self.cfg
        t, _err = compose(cfg.board, [])
        ports = sorted(t.pmod_ports) if t else []

        def items():
            out = []
            for p in pmods_available():
                for port in ports or [None]:
                    e = "%s@%s" % (p, port) if port else p
                    out.append(("[%s] %s" % ("*" if e in cfg.pmods else " ", e), True, e))
            return out or [("(this board has no PMOD ports)", False, None)]

        def select(e):
            new = [x for x in cfg.pmods if x != e] if e in cfg.pmods else cfg.pmods + [e]
            keep = {k: v for k, v in cfg.defines.items()}
            if cfg.load_board(cfg.board, new):
                # what was changed by hand stays changed
                for k in list(keep):
                    cfg.defines.setdefault(k, keep[k])
                self.msg = "PMODs: %s" % (" ".join(new) or "none")
            else:
                self.msg = cfg.error
            return False

        self.run_list("PMODs (each in a port)", items, select)

    # -- the card --------------------------------------------------------

    def pick_sets(self):
        cfg = self.cfg

        def items():
            return [("[%s] %-10s %s" % ("*" if n in cfg.card_sets else " ", t, d),
                     n != "base", n) for n, t, d in mkfatimg.SETS]

        def select(n):
            if n in cfg.card_sets:
                cfg.card_sets.remove(n)
            else:
                cfg.card_sets.append(n)
            return False

        self.run_list("Card: app sets (base is always there)", items, select)

    def pick_packs(self):
        cfg = self.cfg
        packs = sorted(set(mkfatimg.PACK_SET) | {"arkmed"})

        def items():
            return [("[%s] %s" % ("*" if p in cfg.card_packs else " ", p), True, p)
                    for p in packs]

        def select(p):
            if p in cfg.card_packs:
                cfg.card_packs.remove(p)
            else:
                cfg.card_packs.append(p)
            return False

        self.run_list("Card: ask packs (documents, the Ark; arkmed needs its pack built)",
                      items, select)

    # -- actions ---------------------------------------------------------

    def do_report(self):
        import io
        buf = io.StringIO()
        report(self.cfg, buf)
        self.show_text("Estimate", buf.getvalue())

    def do_save(self):
        cfg = self.cfg
        path = cfg.spec_path()
        official = cfg.name in zspec.list_targets(ROOT) and cfg.from_target == cfg.name \
            and not _made_by_zconfig(path)
        if os.path.exists(path) and official:
            if not self.yes("release/targets/%s.spec is a release target: overwrite it? (y/n) " % cfg.name):
                return False
        err = cfg.save()
        self.msg = ("saved, but: " + err) if err else \
            "saved release/targets/%s.spec" % cfg.name
        return err is None

    def outside(self, cmds, env=None):
        """Leave the menus, run commands, come back."""
        curses.endwin()
        rc = 0
        for c in cmds:
            print("\n$ " + " ".join(c), flush=True)
            rc = subprocess.call(c, cwd=ROOT, env=env)
            if rc:
                break
        input("\n[%s] Enter to go back to the menus " % ("done" if rc == 0 else "failed: %d" % rc))
        self.scr.refresh()
        return rc

    def do_build(self):
        cfg = self.cfg
        if cfg.problems() and not self.yes("There are problems (status line): build anyway? (y/n) "):
            return
        if not self.do_save():
            return
        what = self.choose("Build what?", ["the bitstream and flash image",
                                           "the SD card",
                                           "both"])
        if what is None:
            return
        cmds = []
        env = dict(os.environ)
        if what in (0, 2):
            cmds.append(["./release/zrelease", "build", "v" + os_version(),
                         "--targets", cfg.name, "--no-sdcard", "--allow-dirty"])
        if what in (1, 2):
            sets = ",".join(cfg.card_sets)
            env["ZEITLOS_ASK_PACKS"] = " ".join(cfg.card_packs)
            cmds.append(["./release/zrelease", "sdcard", "--sets", sets,
                         "--out", "images/%s.img" % cfg.name])
        self.outside(cmds, env)

    def do_measure(self):
        cfg = self.cfg
        if cfg.family != "ecp5":
            self.msg = "measuring is for ECP5 boards"
            return
        if not self.do_save():
            return
        yosys = "yosys"
        if shutil.which("yowasp-yosys") and not shutil.which("yosys"):
            yosys = "yowasp-yosys"
        env = dict(os.environ)
        env.setdefault("YOSYS", yosys)
        log = os.path.join(ROOT, "output", "zconfig-%s.measure" % cfg.name)
        os.makedirs(os.path.dirname(log), exist_ok=True)
        curses.endwin()
        print("\nSynthesising %s as a release build does (10-25 minutes) ..." % cfg.name, flush=True)
        r = subprocess.run([sys.executable, os.path.join(HERE, "measure.py"), cfg.name],
                           cwd=ROOT, env=env, capture_output=True, text=True)
        with open(log, "w") as f:
            f.write(r.stdout + r.stderr)
        print(r.stdout or r.stderr[-2000:])
        m = re.search(r"TRELLIS_COMB\s+(\d+)\s*/\s*(\d+)", r.stdout)
        b = re.search(r"DP16KD\s+(\d+)", r.stdout)
        if m:
            self.measured = "measured: %s LUT, %s BRAM" % (m.group(1), b.group(1) if b else "?")
        input("\n(also in %s) Enter to go back to the menus " % os.path.relpath(log, ROOT))
        self.scr.refresh()

    # -- small dialogs ---------------------------------------------------

    def ask(self, prompt, default=""):
        h, w = self.scr.getmaxyx()
        curses.curs_set(1)
        curses.echo()
        self.put(h - 2, 1, " " * (w - 2))
        self.put(h - 2, 1, prompt + "[" + default + "] ")
        try:
            raw = self.scr.getstr(h - 2, min(w - 2, len(prompt) + len(default) + 4), 60)
        except curses.error:
            raw = b""
        curses.noecho()
        curses.curs_set(0)
        s = raw.decode(errors="replace").strip()
        return s if s else default

    def yes(self, prompt):
        h, w = self.scr.getmaxyx()
        self.put(h - 2, 1, " " * (w - 2))
        self.put(h - 2, 1, prompt, curses.A_BOLD)
        return self.scr.getch() in (ord("y"), ord("Y"))

    def choose(self, title, options):
        res = []

        def items():
            return [(o, True, i) for i, o in enumerate(options)]

        def select(i):
            res.append(i)
            return True

        self.run_list(title, items, select)
        return res[0] if res else None

    def show_text(self, title, text):
        lines = text.splitlines() or [""]
        top = 0
        while True:
            self.scr.erase()
            h, w = self.scr.getmaxyx()
            self.put(0, 0, (" " + title + " ").ljust(w), curses.color_pair(1))
            for i, l in enumerate(lines[top:top + h - 3]):
                self.put(1 + i, 1, l)
            self.put(h - 1, 1, "Up/Down scroll, any other key back", curses.A_DIM)
            self.scr.refresh()
            k = self.scr.getch()
            if k == curses.KEY_DOWN and top + h - 3 < len(lines):
                top += 1
            elif k == curses.KEY_UP and top > 0:
                top -= 1
            else:
                return

    def confirm_quit(self):
        return self.yes("Quit? Anything not saved is lost. (y/n) ")


def _made_by_zconfig(path):
    try:
        with open(path) as f:
            return "made with `make config`" in f.readline()
    except OSError:
        return False


HELP = """The status line is the estimate: LUTs (ECP5 TRELLIS_COMB, as a release
build packs them), block RAMs (DP16KD) and multipliers, against the
board's FPGA, from tools/zconfig/catalog.cfg. A sum of parts is within
about 10% of a real build; Measure synthesises it for the real numbers.

[*] on, [ ] off, (*) the one chosen of a group, <8> a value (Enter cycles
it). A dim entry needs pins this board does not have.

Save writes release/targets/<name>.spec: the board, the PMODs, and the
define changes from them; `./release/zrelease check` checks it against
rtl/boards.vh. Build runs `zrelease build` for it (and the card:
`zrelease sdcard --sets ...`)."""


def main():
    args = sys.argv[1:]
    cat = Catalog()
    cfg = Config(cat)
    if args and args[0] == "--report":
        if len(args) < 2:
            sys.exit("zconfig: --report <target>")
        name = args[1]
        if name in zspec.list_targets(ROOT):
            ok = cfg.load_target(name)
        elif name in boards():
            ok = cfg.load_board(name)
            cfg.name = name
        else:
            sys.exit("zconfig: no target or board %s" % name)
        if not ok:
            sys.exit(cfg.error)
        report(cfg)
        return
    if args and args[0] in ("-h", "--help"):
        print(__doc__ if __doc__ else "zconfig.py [target] | --report <target>")
        print("\n".join(l[2:] for l in open(__file__).read().split("\n")[2:16]))
        return
    if args:
        if args[0] in zspec.list_targets(ROOT):
            ok = cfg.load_target(args[0])
        elif args[0] in boards():
            ok = cfg.load_board(args[0])
        else:
            sys.exit("zconfig: no target or board %s" % args[0])
        if not ok:
            sys.exit(cfg.error)
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        sys.exit("zconfig: the menus need a terminal (--report <target> does not)")
    try:
        curses.wrapper(lambda scr: UI(scr, cfg).main())
    except SystemExit:
        pass


if __name__ == "__main__":
    main()
