#
# Zeitlos hwmap -- loading the RTL.
#
# The file list is the one synthesis uses: the Makefile's RTL_PICO
# variable. Reading "every .v under rtl/" instead would pick up modules
# that are not in the build -- rtl/mem/sdram.v and rtl/mem/sdram_kianv.v
# both define sdram_wb, and only the second is compiled -- and testbench
# models besides. If the variable cannot be found the tool falls back to
# scanning rtl/ and says so.
#

import os
import re

from . import vparse as V


class LoadError(Exception):
    pass


def rtl_files(root, warn):
    mk = os.path.join(root, "Makefile")
    files = []
    if os.path.exists(mk):
        with open(mk) as f:
            text = f.read()
        m = re.search(r"^RTL_PICO\s*[:?]?=\s*((?:.*\\\n)*.*)$", text, re.M)
        if m:
            files = [w for w in m.group(1).replace("\\\n", " ").split()
                     if w.endswith(".v") or w.endswith(".sv")]
    if files:
        return files, "Makefile RTL_PICO"
    warn("could not read RTL_PICO from the Makefile; scanning rtl/ instead")
    out = []
    for d, _, fs in os.walk(os.path.join(root, "rtl")):
        rel = os.path.relpath(d, root)
        if re.search(r"(^|/)(tb|tests|bench|boards)(/|$)", rel):
            continue
        out += [os.path.join(rel, f) for f in sorted(fs) if f.endswith(".v")]
    return sorted(out), "rtl/ scan"


class Design:
    def __init__(self, root, top_file="rtl/sysctl.v", top_module="sysctl",
                 warn=print):
        self.root = root
        self.warn = warn
        self.files, self.file_source = rtl_files(root, warn)
        if top_file not in self.files:
            self.files.insert(0, top_file)
        incdirs = [os.path.join(root, "rtl")]
        self.units = {}
        self.modules = {}
        self.dup_modules = []
        for rel in self.files:
            path = os.path.join(root, rel)
            if not os.path.exists(path):
                warn("%s (from %s) does not exist" % (rel, self.file_source))
                continue
            try:
                unit = V.read_unit(root, path, incdirs)
            except V.ParseError as e:
                if rel == top_file:
                    raise
                warn("skipping %s: %s" % (rel, e))
                continue
            self.units[rel] = unit
            for m in unit.modules:
                if m.name in self.modules:
                    self.dup_modules.append((m.name, self.modules[m.name].file,
                                             m.file))
                    continue
                self.modules[m.name] = m
        if top_module not in self.modules:
            raise LoadError("module %s not found in %s" % (top_module, top_file))
        self.top = self.modules[top_module]
        self.top_unit = self.units[top_file]

    def port_dir(self, module, port):
        """Direction of a port of a submodule. Modules with no RTL here
        (vendor primitives) fall back to naming conventions."""
        m = self.modules.get(module)
        if m is not None:
            p = m.port(port)
            if p is not None and p.dir:
                return p.dir
            if isinstance(port, int) and port < len(m.ports):
                return m.ports[port].dir
            return None
        name = str(port)
        low = name.lower()
        if (re.search(r"(_o|_out|out\d*|_oe)$", low) or
                re.match(r"^(clk\d|clkout|clkop|clkos)", low) or
                "locked" in low):
            return "output"
        return "input"

    def is_primitive(self, module):
        return module not in self.modules
