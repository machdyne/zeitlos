#
# Zeitlos release tooling -- hardware specs.
#
# A "target" is a board plus whatever is plugged into it: lakritz_uart
# and lakritz_langkatze are the same FPGA board with different PMODs,
# and they need different gateware AND different software. That second
# half is the part worth being careful about -- rtl/boards.vh decides
# whether an SPI master is wired to the ethernet pins, and
# sw/apps/net/Makefile decides whether enc28j60.c or rmii_eth.c is
# compiled, and nothing in the tree connects the two. sw/apps/net/Makefile's own header describes what
# happens when they disagree: undefined enc28j60_* at link if you are
# lucky, and a silent reversion of the DHCP/NTP/SSH configuration to
# defaults if you are not.
#
# So the rule here is: A SPEC STATES A HARDWARE FACT ONCE. The RTL
# defines and the C build flags are both DERIVED from it, by
# derive_sw() below. There is no key in any spec file that sets
# NET_PHY, because there is no way to set it wrong if it cannot be set
# at all.
#
# Three kinds of file, composed in this order:
#
#   hw/boards/<name>.spec   the board itself: FPGA, device, package,
#                           .lpf, flash tool, and the defines that are
#                           true of a bare board
#   hw/pmods/<name>.spec    a delta: what plugging this in adds,
#                           removes, and constrains
#   targets/<name>.spec     names a base board and a PMOD set, plus any
#                           final overrides
#
# Define operations, applied in order:
#
#   NAME            add `define NAME
#   NAME=VALUE      add `define NAME VALUE
#   -NAME           remove NAME if present (this is what lets a target
#                   express the LACK of a feature, which a plain
#                   additive -D on the yosys command line cannot)
#

import os
import re


class SpecError(Exception):
    pass


# ---------------------------------------------------------------------
# File format.
#
# key = value, '#' comments, and a continuation rule: a line that is
# indented belongs to the key above it. That makes the define lists
# readable as lists instead of as one very long line.
# ---------------------------------------------------------------------

def parse_file(path):
    if not os.path.exists(path):
        raise SpecError("%s: not found" % path)
    data = {}
    key = None
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            if raw[0] in " \t" and key is not None:
                data[key].append(line.strip())
                continue
            m = re.match(r"([\w.]+)\s*=\s*(.*)$", line)
            if not m:
                raise SpecError("%s:%d: expected 'key = value', got %r"
                                % (path, lineno, line.strip()))
            key = m.group(1)
            rest = m.group(2).strip()
            data[key] = [rest] if rest else []
    return data


def _one(data, key, default=None, where=""):
    if key not in data or not data[key]:
        if default is None:
            raise SpecError("%s: missing required key '%s'" % (where, key))
        return default
    if len(data[key]) != 1:
        raise SpecError("%s: key '%s' takes a single value" % (where, key))
    return data[key][0]


# PMOD pins. 5/6 and 11/12 are ground and power on the 12-pin
# connector and never carry a signal, so a spec naming one is a typo
# rather than an exotic board.
PMOD_PINS = (1, 2, 3, 4, 7, 8, 9, 10)


def _pinmap(data, key, where):
    """Parse `1=FOO 2=BAR` style pin lists into {int: str}."""
    out = {}
    for tok in _words(data, key):
        if "=" not in tok:
            raise SpecError("%s: %s: expected '<pin>=<value>', got %r"
                            % (where, key, tok))
        pin, val = tok.split("=", 1)
        try:
            pin = int(pin)
        except ValueError:
            raise SpecError("%s: %s: %r is not a pin number"
                            % (where, key, pin))
        if pin not in PMOD_PINS:
            raise SpecError("%s: %s: pin %d is not a PMOD signal pin "
                            "(valid: %s -- 5, 6, 11 and 12 are GND and "
                            "VCC)" % (where, key, pin,
                                      ", ".join(str(p) for p in PMOD_PINS)))
        if pin in out:
            raise SpecError("%s: %s: pin %d given twice"
                            % (where, key, pin))
        out[pin] = val
    return out


def _words(data, key):
    out = []
    for chunk in data.get(key, []):
        out.extend(chunk.split())
    return out


# ---------------------------------------------------------------------
# Composition
# ---------------------------------------------------------------------

def _apply_defines(defines, ops, where):
    """Apply add/remove operations to an ordered define dict."""
    for op in ops:
        # A leading '+' is optional and means the same as no prefix.
        # It is worth allowing because a PMOD spec's define list is a
        # DELTA, and "+SPI_ETH" reads as one where a bare "SPI_ETH"
        # reads as a declaration -- next to a "-UART0" on the following
        # line, the symmetry is the whole point.
        if op.startswith("+"):
            op = op[1:]
        if op.startswith("-"):
            name = op[1:]
            if name not in defines:
                raise SpecError("%s: '-%s' removes a define that is not "
                                "set -- either a typo, or the base board "
                                "changed underneath this spec" % (where, name))
            del defines[name]
        elif "=" in op:
            name, val = op.split("=", 1)
            defines[name] = val
        else:
            defines[op] = None
    return defines


class Pmod:
    """A PMOD plugged into a named port on the board."""

    def __init__(self, name, port, pins, io_type, description):
        self.name = name
        self.port = port              # which board port, e.g. "a"
        self.pins = pins              # {pin number: port name}
        self.io_type = io_type
        self.description = description


class Target:
    def __init__(self, name):
        self.name = name
        self.description = ""
        self.board = None           # BOARD= value for the top-level Makefile
        self.family = None
        self.lpf = None             # base .lpf filename under boards/
        self.pmod_ports = {}        # {port name: {pin: ball}} from the board
        self.pmods = []             # [Pmod] actually plugged in
        self.lpf_drop = []          # extra base-.lpf ports to release
        self.defines = {}           # ordered: name -> value or None
        self.flash_cmd = None       # template, {file} {offset}
        self.core_apps = []
        self.notes = []
        self.sources = []           # spec files that contributed

    # -- what the hardware has -----------------------------------------
    #
    # Descriptive only. It goes in the release notes and README.txt so
    # somebody can tell which download matches their board; NOTHING is
    # built differently because of it.
    #
    # This used to be derive_sw(), and it returned a NET_PHY value that
    # the build passed to sw/apps/net. That is gone: net now links both
    # drivers and picks one at runtime from the feature CSR (see
    # sw/apps/net/net_phy.h). One net.bin runs on every board, so the
    # whole software half of a release builds once rather than per
    # target, and the core app archive is identical everywhere.

    def nic(self):
        d = self.defines
        has_spi_eth = "SPI_ETH" in d
        has_rmii = "ETH_RMII" in d

        # Still worth rejecting, even though nothing downstream now
        # depends on the answer: rtl/sysctl.v would need two MACs to
        # honour it, so a spec asking for both is a mistake in the
        # spec rather than a configuration.
        if has_spi_eth and has_rmii:
            raise SpecError("%s: both SPI_ETH and ETH_RMII are defined. "
                            "rtl/sysctl.v builds one MAC or the other."
                            % self.name)
        if has_spi_eth:
            return "ENC28J60"
        if has_rmii:
            return "RMII"
        return None

    def core_app_list(self):
        """The core apps in the ZAR.

        Now the same list on every board. `net` is included even where
        there is no MAC: it detects that at startup from the feature
        CSR and exits cleanly saying so, which is better than being
        absent for reasons the user cannot see -- and it is what makes
        one archive correct everywhere.
        """
        return list(self.core_apps)


def load_target(root, name):
    """Load and compose targets/<name>.spec."""
    tdir = os.path.join(root, "release/targets")
    tpath = os.path.join(tdir, name + ".spec")
    tdata = parse_file(tpath)

    base = _one(tdata, "base", where=tpath)
    bpath = os.path.join(root, "release/hw/boards", base + ".spec")
    bdata = parse_file(bpath)

    t = Target(name)
    t.sources = [tpath, bpath]
    t.board = _one(bdata, "board", where=bpath)
    t.family = _one(bdata, "family", where=bpath)
    t.lpf = _one(bdata, "lpf", default="", where=bpath) or None
    t.flash_cmd = _one(bdata, "flash_cmd", default="", where=bpath) or None
    t.core_apps = _words(bdata, "core_apps") or ["wm", "net", "repl", "term"]
    t.description = _one(tdata, "description",
                         default=_one(bdata, "description", default=name,
                                      where=bpath),
                         where=tpath)

    _apply_defines(t.defines, _words(bdata, "defines"), bpath)

    # -- the board's PMOD ports ----------------------------------------
    for key in bdata:
        if not key.startswith("pmod."):
            continue
        port = key[len("pmod."):].lower()
        t.pmod_ports[port] = _pinmap(bdata, key, bpath)

    # -- PMODs, each plugged into a named port -------------------------
    #
    # "langkatze@a" rather than "langkatze": a board can have more than
    # one port, and which one a PMOD is in decides its pins entirely.
    # The port is only optional when the board has exactly one, since
    # then there is nothing to be ambiguous about.
    for entry in _words(tdata, "pmods"):
        if "@" in entry:
            pmod, port = entry.split("@", 1)
            port = port.lower()
        else:
            pmod, port = entry, None

        ppath = os.path.join(root, "release/hw/pmods", pmod + ".spec")
        pdata = parse_file(ppath)
        t.sources.append(ppath)
        _apply_defines(t.defines, _words(pdata, "defines"), ppath)

        pins = _pinmap(pdata, "pins", ppath)
        io_type = _one(pdata, "io_type", default="LVCMOS33", where=ppath)
        desc = _one(pdata, "description", default=pmod, where=ppath)

        if pins:
            if port is None:
                if len(t.pmod_ports) == 1:
                    port = next(iter(t.pmod_ports))
                else:
                    raise SpecError(
                        "%s: '%s' does not say which port. %s has %d "
                        "(%s), so write '%s@<port>'."
                        % (tpath, pmod, t.board, len(t.pmod_ports),
                           ", ".join(sorted(t.pmod_ports)) or "none", pmod))
            if port not in t.pmod_ports:
                raise SpecError(
                    "%s: '%s@%s' -- %s declares no PMOD port '%s'.\n"
                    "  Ports it does have: %s\n"
                    "  Add a 'pmod.%s = ...' ball map to "
                    "release/hw/boards/%s.spec if the board really has one."
                    % (tpath, pmod, port, t.board, port,
                       ", ".join(sorted(t.pmod_ports)) or "(none)",
                       port, t.board))

            balls = t.pmod_ports[port]
            unmapped = sorted(set(pins) - set(balls))
            if unmapped:
                raise SpecError(
                    "%s: '%s' uses PMOD pin(s) %s, which port '%s' of %s "
                    "does not map to a ball."
                    % (tpath, pmod, ", ".join(str(u) for u in unmapped),
                       port, t.board))

            t.pmods.append(Pmod(pmod, port, pins, io_type, desc))
        elif port is not None:
            raise SpecError("%s: '%s' declares no pins, so '@%s' means "
                            "nothing" % (tpath, pmod, port))

        for n in pdata.get("notes", []):
            t.notes.append(n)

    # -- target-level final say ----------------------------------------
    _apply_defines(t.defines, _words(tdata, "defines"), tpath)

    # Constraints in the board .lpf that this target releases, because
    # a PMOD has taken the pins. Separate from `-NAME` on purpose:
    # removing a define is a statement about the gateware, dropping a
    # constraint is a statement about the pins, and they are not always
    # the same edit -- rtl/sysctl.v declares some ports unconditionally,
    # so removing the define does not always remove the port.
    t.lpf_drop = _words(tdata, "lpf_drop")
    for n in tdata.get("notes", []):
        t.notes.append(n)

    return t


def list_targets(root):
    tdir = os.path.join(root, "release/targets")
    if not os.path.isdir(tdir):
        return []
    return sorted(f[:-5] for f in os.listdir(tdir) if f.endswith(".spec"))


# ---------------------------------------------------------------------
# The universal half of rtl/boards.vh
#
# `RTC, `TRNG, `GAME, `CPU_MUL, `CPU_DIV and friends are NOT per-board
# and stay outside the ZSPEC guard -- boards.vh explains at length why
# each of them is universal. Specs therefore do not, and cannot, set
# them.
#
# But sw/common/arch.mk's ARCH has to agree with `CPU_MUL/`CPU_DIV, and
# that file's header is blunt about the consequence of getting it
# backwards: rv32im software on an rv32i bitstream makes every mul and
# div an illegal instruction. So read the universal section and derive
# ARCH from it, rather than trusting arch.mk's rv32im default to still
# be right on the day somebody comments `CPU_MUL out.
# ---------------------------------------------------------------------

def universal_defines(root):
    path = os.path.join(root, "rtl/boards.vh")
    with open(path) as f:
        text = f.read()

    # Everything before the per-board chain. The marker is boards.vh's
    # own section header; if it is ever renamed this raises rather than
    # silently scanning the whole file.
    marker = "// BOARD CONFIG"
    if marker not in text:
        raise SpecError("rtl/boards.vh: could not find the '%s' section "
                        "header. This tool splits the file there." % marker)
    head = text.split(marker, 1)[0]

    out = {}
    for line in head.splitlines():
        line = line.strip()
        if not line.startswith("`define"):
            continue
        parts = line.split(None, 2)
        out[parts[1]] = parts[2].strip() if len(parts) > 2 else None
    return out


def board_block_defines(root, board):
    """The `ifdef BOARD_<X> block of rtl/boards.vh, as a dict.

    Used only to CHECK the board specs against the file they were
    copied from. The specs are the source of truth for a release build
    (they have to be -- they can subtract, and boards.vh cannot), but
    the two drifting apart silently would mean a release quietly
    building something other than what `make BOARD=x flash` builds,
    which is the worst kind of difference to discover from a bug
    report. Returns None if boards.vh has no block for this board.
    """
    path = os.path.join(root, "rtl/boards.vh")
    with open(path) as f:
        lines = f.read().splitlines()

    want = "BOARD_" + board.upper()
    inside = False
    out = {}
    for line in lines:
        s = line.strip()
        m = re.match(r"`(ifdef|elsif)\s+(\w+)", s)
        if m:
            inside = (m.group(2) == want)
            continue
        if s == "`endif" and inside:
            break
        if not inside or not s.startswith("`define"):
            continue
        parts = s.split(None, 2)
        val = parts[2].strip() if len(parts) > 2 else None
        if val is not None:
            # drop a trailing // comment
            val = val.split("//", 1)[0].strip() or None
        out[parts[1]] = val
    return out if out else None


def check_drift(root, target):
    """Compare a target's board-level defines against rtl/boards.vh.

    Only meaningful for a target that adds nothing -- once PMODs are
    involved the two are SUPPOSED to differ. So the comparison is made
    against the base board's spec, not the composed target.
    """
    base_path = None
    for s in target.sources:
        if "/hw/boards/" in s:
            base_path = s
    if base_path is None:
        return []

    bdata = parse_file(base_path)
    spec_defs = _apply_defines({}, _words(bdata, "defines"), base_path)
    vh_defs = board_block_defines(root, target.board)
    if vh_defs is None:
        return ["rtl/boards.vh has no BOARD_%s block; %s cannot be "
                "cross-checked" % (target.board.upper(), base_path)]

    problems = []
    for name in sorted(set(spec_defs) | set(vh_defs)):
        in_spec = name in spec_defs
        in_vh = name in vh_defs
        if in_spec and not in_vh:
            problems.append("%s: `%s is in the spec but not in "
                            "rtl/boards.vh's BOARD_%s block"
                            % (base_path, name, target.board.upper()))
        elif in_vh and not in_spec:
            problems.append("%s: `%s is in rtl/boards.vh's BOARD_%s block "
                            "but not in the spec"
                            % (base_path, name, target.board.upper()))
        elif spec_defs[name] != vh_defs[name]:
            problems.append("%s: `%s is %r in the spec and %r in "
                            "rtl/boards.vh" % (base_path, name,
                                               spec_defs[name],
                                               vh_defs[name]))
    return problems


def preprocess_defines(root, extra_defines, zspec_text=None, probe_names=None):
    """Ask a real Verilog preprocessor what rtl/boards.vh defines.

    The drift check above compares the spec text against boards.vh's
    text. This compares what a TOOL actually sees, which is a different
    guarantee: it is what catches a misplaced `ifdef/`endif in the ZSPEC
    guard, or an include that silently resolved to nothing.

    Returns None if no preprocessor is available -- iverilog is not a
    build dependency of this tree and this check is a bonus, not a gate.
    """
    import shutil as _shutil
    import subprocess as _sp
    import tempfile

    if _shutil.which("iverilog") is None:
        return None

    names = probe_names or []
    tmp = tempfile.mkdtemp(prefix="zrelease-probe-")
    try:
        incdir = os.path.join(tmp, "rtl")
        os.makedirs(incdir)
        # Copy boards.vh rather than adding rtl/ to the include path, so
        # a stale rtl/zspec.vh in the tree cannot influence the answer.
        _shutil.copy(os.path.join(root, "rtl/boards.vh"), incdir)
        if zspec_text is not None:
            with open(os.path.join(incdir, "zspec.vh"), "w") as f:
                f.write(zspec_text)

        body = ['`include "boards.vh"', "module probe;", "initial begin"]
        for n in names:
            body.append("`ifdef %s" % n)
            body.append('  $display("%s=%%0s", "on");' % n)
            body.append("`endif")
        body += ["end", "endmodule"]
        probe = os.path.join(tmp, "probe.v")
        with open(probe, "w") as f:
            f.write("\n".join(body) + "\n")

        cmd = ["iverilog", "-g2005", "-I", incdir,
               "-o", os.path.join(tmp, "a.out")]
        cmd += ["-D" + d for d in extra_defines]
        cmd.append(probe)
        r = _sp.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise SpecError("preprocessing rtl/boards.vh failed:\n%s%s"
                            % (r.stdout, r.stderr))
        r = _sp.run(["vvp", os.path.join(tmp, "a.out")],
                    capture_output=True, text=True)
        return {l.split("=")[0] for l in r.stdout.split() if "=" in l}
    finally:
        _shutil.rmtree(tmp, ignore_errors=True)


def check_equivalence(root, target, zspec_text):
    """Does the ZSPEC path define the same things as the BOARD_x path?

    Only meaningful for a target that adds and removes nothing -- for
    lakritz_langkatze the two are supposed to differ, and by exactly
    `SPI_ETH. So the expected difference is computed rather than
    assumed, and anything else is reported.
    """
    vh = board_block_defines(root, target.board)
    if vh is None:
        return []

    base_path = [s for s in target.sources if "/hw/boards/" in s]
    if not base_path:
        return []
    bdata = parse_file(base_path[0])
    base_defs = _apply_defines({}, _words(bdata, "defines"), base_path[0])

    probe = sorted(set(vh) | set(target.defines) | set(base_defs))

    via_board = preprocess_defines(root, ["BOARD_" + target.board.upper()],
                                   probe_names=probe)
    if via_board is None:
        return None                      # no preprocessor; skipped

    via_spec = preprocess_defines(root, ["ZSPEC"], zspec_text=zspec_text,
                                  probe_names=probe)

    expected_extra = set(target.defines) - set(base_defs)
    expected_gone = set(base_defs) - set(target.defines)

    got_extra = via_spec - via_board
    got_gone = via_board - via_spec

    problems = []
    for n in sorted(got_extra - expected_extra):
        problems.append("`%s appears via ZSPEC but not via BOARD_%s, and no "
                        "spec asked for it" % (n, target.board.upper()))
    for n in sorted(expected_extra - got_extra):
        problems.append("a spec adds `%s but it is not defined after "
                        "preprocessing" % n)
    for n in sorted(got_gone - expected_gone):
        problems.append("`%s is defined via BOARD_%s but vanished via ZSPEC, "
                        "and no spec removed it" % (n, target.board.upper()))
    for n in sorted(expected_gone - got_gone):
        problems.append("a spec removes `%s but it is still defined after "
                        "preprocessing" % n)
    return problems


def derive_arch(root):
    u = universal_defines(root)
    has_mul = "CPU_MUL" in u
    has_div = "CPU_DIV" in u
    if has_mul and has_div:
        return "rv32im"
    if has_mul and not has_div:
        # rv32i_zmmul needs binutils >= 12; arch.mk warns about this.
        raise SpecError("rtl/boards.vh defines `CPU_MUL without `CPU_DIV. "
                        "That is rv32i_zmmul, which sw/common/arch.mk says "
                        "the pinned toolchain cannot build. Refusing to "
                        "guess an ARCH for a release.")
    if has_div and not has_mul:
        raise SpecError("rtl/boards.vh defines `CPU_DIV without `CPU_MUL. "
                        "There is no RISC-V ISA string for that.")
    return "rv32i"


def declared_ports(root, defines):
    """Top-level ports rtl/sysctl.v declares for a given define set.

    Parses the module's port list, tracking `ifdef/`ifndef/`else so the
    answer reflects THIS target rather than the file in general.

    The reason to bother: nextpnr-ecp5 is run here without
    --lpf-allow-unconstrained, so a declared port with no LOCATE is a
    hard failure -- and the way to get one is exactly what the PMOD
    system does, namely release a base-.lpf constraint whose port is
    still built. Catching it here names the port and says which define
    would remove it; nextpnr says neither.
    """
    path = os.path.join(root, "rtl/sysctl.v")
    with open(path) as f:
        text = f.read()

    m = re.search(r"^module\s+sysctl\b", text, re.M)
    if not m:
        raise SpecError("rtl/sysctl.v: could not find the sysctl module "
                        "header")

    # Skip an optional parameter list -- "module sysctl #() (" is what
    # this file has, and a naive search for the first '(' finds the
    # #()'s rather than the port list's.
    pos = m.end()
    rest = text[pos:]
    mp = re.match(r"\s*#\s*\(", rest)
    if mp:
        depth = 0
        for i in range(pos + mp.end() - 1, len(text)):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    pos = i + 1
                    break

    start = text.index("(", pos)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                body = text[start + 1:i]
                break
    else:
        raise SpecError("rtl/sysctl.v: unterminated port list")

    active = [True]                # stack of enclosing conditions
    seen = []                      # (define that guards it, port name)
    guard = []                     # names of the enclosing `ifdefs

    for line in body.splitlines():
        s = line.split("//", 1)[0].strip()
        if not s:
            continue
        mi = re.match(r"`(ifdef|ifndef)\s+(\w+)", s)
        if mi:
            want = mi.group(2) in defines
            if mi.group(1) == "ifndef":
                want = not want
            active.append(active[-1] and want)
            guard.append(mi.group(2))
            continue
        if s.startswith("`else"):
            top = active.pop()
            parent = active[-1]
            active.append(parent and not (top if parent else True))
            # Recompute properly: flip only the innermost condition.
            active[-1] = parent and not top if parent else False
            continue
        if s.startswith("`endif"):
            if len(active) > 1:
                active.pop()
            if guard:
                guard.pop()
            continue
        if not active[-1]:
            continue
        # input/output/inout, optional wire/reg, optional [range]
        mp = re.match(r"(input|output|inout)\s+"
                      r"(?:wire\s+|reg\s+)?(?:\[[^\]]*\]\s*)?(.+)", s)
        if not mp:
            continue
        for name in mp.group(2).split(","):
            name = name.strip().rstrip(",").strip()
            if re.match(r"^\w+$", name):
                seen.append((guard[-1] if guard else None, name))
    return seen
