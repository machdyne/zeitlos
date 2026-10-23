#!/usr/bin/env python3
# planedemo -- run film.elf on unicorn's RISC-V, count instructions
# between the film's marks (planedemo.c, PD_MEASURE), report per scene.
import os, sys, time
from unicorn import *
from unicorn.riscv_const import *
from elftools.elf.elffile import ELFFile

f = ELFFile(open(sys.argv[1], 'rb'))
mu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
mu.mem_map(0, 64 << 20)
mu.mem_map(0x3f00000 & ~0xfff, 0x1000) if False else None
for seg in f.iter_segments():
    if seg['p_type'] == 'PT_LOAD':
        mu.mem_write(seg['p_vaddr'], seg.data())
sym = {s.name: s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols()}
n = [0]
# where the instructions go, by function: what to make cheaper
funcs = sorted((s['st_value'], s['st_size'], s.name) for s in f.get_section_by_name('.symtab').iter_symbols()
               if s['st_info']['type'] == 'STT_FUNC' and s['st_size'])
import bisect
starts = [a for a, _, _ in funcs]
prof = {}
BOUNDARY = os.environ.get('PROFILE') == 'boundary'
phase = [0]
def blk(uc, addr, size, ud):
    n[0] += size // 4
    if phase[0] == 1:
        i = bisect.bisect_right(starts, addr) - 1
        name = funcs[i][2] if i >= 0 and addr < funcs[i][0] + funcs[i][1] else '?'
        prof[name] = prof.get(name, 0) + size // 4
mu.hook_add(UC_HOOK_BLOCK, blk)
marks = []
def mark(uc, addr, size, ud):
    k = uc.reg_read(UC_RISCV_REG_A0)
    marks.append((k, n[0]))
    # the scene's frame() profiled, or with PROFILE=boundary what is
    # run at the frame boundary (the palette, the queued drawing)
    if k == (3 if BOUNDARY else 1): phase[0] = 1
    elif k == (4 if BOUNDARY else 2): phase[0] = 0
mu.hook_add(UC_HOOK_CODE, mark, begin=sym['pd_mark'], end=sym['pd_mark'])
t = time.time()
try:
    mu.emu_start(sym['_start'], 0xffffffff)
except UcError as e:
    print("stopped:", e, hex(mu.reg_read(UC_RISCV_REG_PC)))
print("%.0f M instructions in all, %.0f s" % (n[0] / 1e6, time.time() - t))

# frames: 1 -> 2 the scene's frame(), 2 -> 3 the jobs, 3 -> 4 the
# runtime's frame (the palette, then the queued drawing run: ar_frame_hook); 5 -> 6 a scene's begin()
FPS_BUDGET = None
scene = -1
rows = []
cur = None
last = {}
for k, c in marks:
    last[k] = c
    if k == 6:
        scene += 1
        cur = {'begin': c - last.get(5, c), 'frames': [], 'jobs': [], 'rt': []}
        rows.append(cur)
    elif k == 2 and cur is not None:
        cur['frames'].append(c - last[1])
    elif k == 3 and cur is not None:
        cur['jobs'].append(c - last[2])
    elif k == 4 and cur is not None:
        cur['rt'].append(c - last[3])
names = ['clock', 'plasma', 'storm', 'mouse', 'donut', 'tunnel', 'vectors', 'sunrise', 'end']
print("%-8s %9s %9s %9s %9s %10s" % ("scene", "begin", "frame avg", "frame max", "boundary", "jobs"))
for i, r in enumerate(rows):
    fr = r['frames'] or [0]
    print("%-8s %9d %9d %9d %9d %10d" % (names[i] if i < len(names) else i, r['begin'],
        sum(fr) // len(fr), max(fr), sum(r['rt']) // max(1, len(r['rt'])), sum(r['jobs'])))

print()
print("%s, by function (instructions, all frames):" % ("at the frame boundary" if BOUNDARY else "the scenes' frames"))
tot = sum(prof.values())
for name, c in sorted(prof.items(), key=lambda x: -x[1])[:25]:
    print("  %-28s %6.1f%%  %8d a frame" % (name, 100.0 * c / tot, c // 3750))
