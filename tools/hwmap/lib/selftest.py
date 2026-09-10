#
# Zeitlos hwmap -- self-test.
#
# A tiny synthetic SoC with known answers exercises every stage: the
# condition algebra, the condition-tracking lexer, decode and mux
# recognition, alternatives, bypass folding, each check, and rendering.
# It does not depend on the real RTL, so it only fails when hwmap
# itself is broken. (The golden comparison against the real RTL lives
# in the CLI, see `hwmap --selftest`.)
#

import os
import shutil
import tempfile

from . import cond as C

FILES = {
    "Makefile": "RTL_PICO = rtl/sysctl.v rtl/cpu.v rtl/slv.v rtl/brg.v\n",
    "rtl/test.vh": ("`ifndef TEST_VH\n`define TEST_VH\n"
                    "`ifndef SIZE_DEFAULT\n`define SIZE_DEFAULT 4\n`endif\n`endif\n"),
    "rtl/cpu.v": """
module cpu (
    input clk_i,
    output [31:0] wbm_adr_o, output [31:0] wbm_dat_o, input [31:0] wbm_dat_i,
    output wbm_we_o, output wbm_stb_o, output wbm_cyc_o, input wbm_ack_i,
    input [31:0] irq
);
endmodule
""",
    "rtl/slv.v": """
module slv #(parameter SIZE = 1) (
    input wb_clk_i, input [31:0] wb_adr_i, input [31:0] wb_dat_i,
    output [31:0] wb_dat_o, input wb_we_i, input wb_stb_i, input wb_cyc_i,
    output wb_ack_o, output int_o
);
endmodule
module slv_b (
    input wb_clk_i, input [31:0] wb_adr_i, output [31:0] wb_dat_o,
    input wb_stb_i, input wb_cyc_i, output wb_ack_o
);
endmodule
""",
    "rtl/brg.v": """
module brg (
    input [31:0] c_adr_i, input c_cyc_i, output c_ack_o, output [31:0] c_dat_o,
    output [31:0] m_adr_o, output m_cyc_o, input m_ack_i, input [31:0] m_dat_i
);
endmodule
""",
    "rtl/sysctl.v": """
`include "test.vh"
module sysctl (input CLK_48, output LED);
    wire clk48mhz = CLK_48;
    wire [31:0] cpu_adr, cpu_dat_o, cpu_dat_i;
    wire cpu_cyc, cpu_stb, cpu_we, cpu_ack;
    reg [31:0] cpu_irq;
    cpu cpu0 (.clk_i(clk48mhz), .wbm_adr_o(cpu_adr), .wbm_dat_o(cpu_dat_o),
              .wbm_dat_i(cpu_dat_i), .wbm_we_o(cpu_we), .wbm_stb_o(cpu_stb),
              .wbm_cyc_o(cpu_cyc), .wbm_ack_i(cpu_ack), .irq(cpu_irq));
    wire [31:0] wbm_adr, wbm_dat_i;
    wire wbm_cyc, wbm_ack;
`ifdef BRIDGE
    brg br0 (.c_adr_i(cpu_adr), .c_cyc_i(cpu_cyc), .c_ack_o(cpu_ack),
             .c_dat_o(cpu_dat_i), .m_adr_o(wbm_adr), .m_cyc_o(wbm_cyc),
             .m_ack_i(wbm_ack), .m_dat_i(wbm_dat_i));
`else
    assign wbm_adr = cpu_adr;
    assign wbm_cyc = cpu_cyc;
    assign cpu_ack = wbm_ack;
    assign cpu_dat_i = wbm_dat_i;
`endif
    wire cs_a = ((wbm_adr & 32'hf000_0000) == 32'h1000_0000);
`ifdef FAST
    wire cs_b = ((wbm_adr & 32'hf000_0000) == 32'h2000_0000);
`elsif SLOW
    wire cs_c = ((wbm_adr & 32'hf000_0000) == 32'h2000_0000);
`endif
`ifdef EXTRA
    wire cs_d = ((wbm_adr & 32'hf000_0000) == 32'h1000_0000);
`endif
`ifdef HANG
    wire cs_e = ((wbm_adr & 32'hf000_0000) == 32'h3000_0000);
`endif
    wire [31:0] a_dat, b_dat, c_dat, d_dat;
    wire a_ack, b_ack, c_ack, d_ack, a_int;
`ifdef FAST
    wire fast_int;
`endif
    assign wbm_dat_i =
        ({32{cs_a}} & a_dat) |
`ifdef FAST
        ({32{cs_b}} & b_dat) |
`elsif SLOW
        ({32{cs_c}} & c_dat) |
`endif
`ifdef EXTRA
        ({32{cs_d}} & d_dat) |
`endif
        32'd0;
    assign wbm_ack =
        (cs_a & a_ack) |
`ifdef FAST
        (cs_b & b_ack) |
`elsif SLOW
        (cs_c & c_ack) |
`endif
`ifdef EXTRA
        (cs_d & d_ack) |
`endif
        1'b0;
    slv #(.SIZE(`SIZE_DEFAULT)) sa (.wb_clk_i(clk48mhz), .wb_adr_i(wbm_adr),
        .wb_cyc_i(cs_a && wbm_cyc), .wb_dat_o(a_dat), .wb_ack_o(a_ack),
        .int_o(a_int));
`ifdef FAST
    slv sb (.wb_clk_i(clk48mhz), .wb_adr_i(wbm_adr), .wb_cyc_i(cs_b && wbm_cyc),
        .wb_dat_o(b_dat), .wb_ack_o(b_ack), .int_o(fast_int));
`elsif SLOW
    slv_b sc (.wb_clk_i(clk48mhz), .wb_adr_i(wbm_adr), .wb_cyc_i(cs_c && wbm_cyc),
        .wb_dat_o(c_dat), .wb_ack_o(c_ack));
`endif
`ifdef EXTRA
    slv sd (.wb_clk_i(clk48mhz), .wb_adr_i(wbm_adr), .wb_cyc_i(cs_d && wbm_cyc),
        .wb_dat_o(d_dat), .wb_ack_o(d_ack),);
`endif
    always @* begin
        cpu_irq = 0;
        cpu_irq[3] = a_int;
        cpu_irq[5] = fast_int;
    end
`ifdef FAST
    assign LED = 1'b1;
`endif
`ifdef SLOW
    assign LED = 1'b0;
`endif
endmodule
""",
}


class Failed(Exception):
    pass


def run(verbose=False):
    from . import design as D, model as M, hints as H, check as K, layout as L
    from . import pdf, svg
    results = []

    def ok(name, cond_, detail=""):
        results.append((name, bool(cond_), detail))

    A, B = ("A", True), ("B", True)
    nA = ("A", False)
    ok("cond: X | !X is always", C.absorb([C.cube(A), C.cube(nA)]) == [C.TRUE])
    ok("cond: X | (Y & !X) reduces to X | Y",
       set(C.absorb([C.cube(A), C.cube(B, nA)])) == {C.cube(A), C.cube(B)})
    ok("cond: always minus (A | !A) is empty",
       C.uncovered(C.TRUE, [C.cube(A), C.cube(nA)]) == [])
    ok("cond: A minus B is A & !B",
       C.uncovered(C.cube(A), [C.cube(B)]) == [C.cube(A, ("B", False))])

    tmp = tempfile.mkdtemp(prefix="hwmap-selftest-")
    try:
        for rel, text in FILES.items():
            p = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f:
                f.write(text)
        d = D.Design(tmp, warn=lambda s: None)
        h = H.Hints()
        m = M.Model(d, h)
        inst = {i.name: i for i in d.top.instances}
        ok("lexer: include guard adds no condition", C.fmt(inst["sa"].cond) == "always")
        ok("lexer: `ifdef", C.fmt(inst["sb"].cond) == "FAST")
        ok("lexer: `elsif carries !earlier", C.fmt(inst["sc"].cond) == "SLOW & !FAST",
           C.fmt(inst["sc"].cond))
        ok("lexer: default value from `ifndef/`define",
           m.defaults.get("SIZE_DEFAULT", [None])[0] == "4")
        ok("parser: trailing comma in a port list", "sd" in inst)
        ok("model: decode window",
           m.decodes["cs_a"]["windows"][0]["value"] == 0x10000000 and
           m.decodes["cs_a"]["windows"][0]["mask"] == 0xF0000000)
        ok("model: tenant from the ack mux",
           [t["block"].id for t in m.tenants["cs_a"]] == ["sa"])
        ok("model: main bus net", m.main_bus == "wbm_adr")
        tree = m.main_tree
        ok("model: optional bridge folds to bypassable",
           tree and tree["kind"] == "bridge" and tree["bypass"] and
           tree["up"]["kind"] == "master", str(tree and tree["kind"]))
        ok("model: interrupt source", any(i["n"] == 3 and i["sources"] and
                                          i["sources"][0].get("block") is m.by_inst[("sa", inst["sa"].line)]
                                          for i in m.irqs))
        ok("model: clock", any(e["net"] == "clk48mhz" and e["mhz"] == 48.0 for e in m.clocks))
        findings = K.run(m)
        groups = K.group(findings)

        def has(kind, when):
            return any(f["kind"] == kind and C.fmt(f["when"]) == when for f in findings)
        ok("check: overlapping windows", has("overlap", "EXTRA"))
        ok("check: undeclared outside its `ifdef", has("undeclared", "!FAST"))
        ok("check: two drivers", has("drivers", "FAST & SLOW"))
        ok("check: decoded but never muxed", has("no-mux", "HANG"))
        ok("check: nothing spurious",
           not any(f["kind"] in ("undeclared", "overlap") and C.fmt(f["when"]) not in
                   ("EXTRA", "!FAST") for f in findings),
           "; ".join("%s %s" % (f["kind"], C.fmt(f["when"])) for f in findings))
        info = dict(rev="selftest", stats="", files="selftest")
        v = L.View(m, h, findings, groups, info)
        ok("layout: slot 0x2 is a one-of", v.columns[2] and v.columns[2][0]["kind"] == "alts")
        page = L.MapPage(v)
        cv = page.draw()
        pages = [cv] + L.reference_pages(v)
        out = os.path.join(tmp, "t.pdf")
        pdf.write(out, pages, "t", "t")
        with open(out, "rb") as f:
            head = f.read(8)
        ok("render: PDF written", head.startswith(b"%PDF-1.4"))
        svg.write(os.path.join(tmp, "t.svg"), cv)
        ok("render: every block placed", not page.unplaced,
           ", ".join(b.id for b in page.unplaced))
    except Exception as e:                      # report, do not hide
        import traceback
        results.append(("exception", False, traceback.format_exc()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return results
