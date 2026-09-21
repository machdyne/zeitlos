# Vendored: nextpnr-ecp5 base configuration

`lfe5u-25f.config` is the configuration nextpnr-ecp5 starts every 25F
(and 12F) design from, before placing anything: 179 settings: four global-clock mux arcs, and mostly
`CIB.J??MUX 0` tie-offs in the DCU, PLL and EFB interface tiles, and
nine `unknown:` bits. `zfpga pnr` starts from the same baseline, or its
output could not match nextpnr's.

| | |
|---|---|
| Upstream | https://github.com/YosysHQ/nextpnr, tag `nextpnr-0.6` |
| Source | `ecp5/baseconfigs.cc`, function `config_empty_lfe5u_25f()` |
| Licence | ISC -- `COPYING`, copied verbatim; the notice travels with the data |

It is DATA extracted from code: each
`cc.tiles["T"].add_enum("K", "V")` became `enum: K V` under `.tile T`,
and each `add_unknown(F, B)` became `unknown: FxBy` and each `add_arc(S, D)` `arc: S D`, in source order.
No code was copied. `tools/mkzdb.py --base` embeds it in the `.zdb`, so
nothing extra ships to the card. 45F and 85F have their own functions
in the same file and follow the same way when those dies are vendored.
