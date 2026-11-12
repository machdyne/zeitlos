# CI: what GitHub checks

Two workflows, in `.github/workflows/`. Neither needs a board, and
neither makes a release: `zrelease build` stays on a machine of one's
own ([../release/README.md](../release/README.md)).

## Every push and pull request (`ci.yml`)

Two jobs, side by side, about a quarter of an hour each.

**build**
- the xPack RISC-V GCC 15.2 the tree's budgets are set with, cached;
- the kernel, the BIOS and every app, for Lakritz;
- `tools/gen_abi.py --check`: the committed ABI fingerprint is the one
  the sources have;
- the core archive for both sizes of machine, Obst (the small tier)
  and Lakritz, each held to its budgets (`CORE_BUDGETS_SMALL`,
  `CORE_BUDGETS`); the sizes go in the job's summary, so a budget
  creeping up shows on the pull request;
- the emulator, headless (`make -C sim/sys test`, [zsys.md](zsys.md)):
  the devices against the RTL's behaviour, boot, the desktop, input,
  touch, and the screen size apps are told ([screen.md](screen.md));
- **a report, not a check**: what changed in the interfaces since
  `release/abi/snapshot.txt` (`gen_abi.py --compare`), in the summary.
  Nothing fails because of it while the interfaces are still being
  prepared (docs/freeze.md).

**host-tests**: `tools/run_tests.py --rtl`, every host test and the RTL
simulations with Icarus Verilog.

On failure the emulator's logs and screenshots, and the tests' logs,
are kept for a week as the run's artifacts.

## Weekly, and by hand (`weekly.yml`)

Mondays at 03:17 UTC, and whenever someone starts it (Actions, weekly,
Run workflow). What is too slow for every push:

- **the page**: emscripten and Playwright's Chromium, the docs and
  course packs, the emulator's card (`zrelease sdcard --variant emu`),
  then `web-test` without a card and with one;
- **Lakritz's gateware**: the OSS CAD Suite's Yosys and nextpnr,
  synthesis, place and route, and a failure if a clock misses timing
  after routing (`tools/pnr_timing.sh`). The utilisation goes in the
  summary.

Only Lakritz: every board's gateware takes about as long as a release,
and is checked before one, by hand.

## Limits

GitHub's standard runners are free for a public repository. A job may
run 6 hours; 20 run at once on the Free plan; caches hold 10 GB per
repository and artifacts 500 MB. Minutes are not the constraint here;
time per job is, which is why the gateware is weekly and one board.

## Running the same locally

```
make BOARD=lakritz os bios apps
python3 tools/gen_abi.py --check
make BOARD=obst output/obst/apps.zar && make BOARD=lakritz output/lakritz/apps.zar
make -C sim/sys test ZAR=$PWD/output/lakritz/apps.zar
python3 tools/run_tests.py --rtl
python3 tools/gen_abi.py --compare release/abi/snapshot.txt
```
