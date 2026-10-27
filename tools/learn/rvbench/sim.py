#!/usr/bin/env python3
# tools/learn/rvbench: runs simbench.elf (rv32im, build.sh) in unicorn and
# says, for each phase simbench marks, the instructions it took, the
# functions they were in, and a model of the board's cycles: about 6 a
# instruction (picorv32), plus 30 for each miss of an 8 KB instruction
# cache and 22 for each load missing a 4 KB data cache (docs/icache.md,
# docs/dcache.md). The model is a guide, not the board: compare phases.
#
#   python3 sim.py out/simbench.elf [card]     TOP=n: functions listed
import sys, os, bisect, collections
from unicorn import *
from unicorn.riscv_const import *
from elftools.elf.elffile import ELFFile

CARD = sys.argv[2] if len(sys.argv) > 2 else '/tmp/learn_sim_card'
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
FONT = os.path.join(ROOT, 'sw/data/font')
CPI = float(os.environ.get('CPI', '7'))
MHZ = 48.0
TOP = int(os.environ.get('TOP', '18'))

elf = ELFFile(open(sys.argv[1], 'rb'))
mu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
mu.mem_map(0, 16 << 20)
for seg in elf.iter_segments():
	if seg['p_type'] == 'PT_LOAD':
		mu.mem_write(seg['p_paddr'], seg.data())
syms = []
for s in elf.get_section_by_name('.symtab').iter_symbols():
	if s['st_info']['type'] == 'STT_FUNC' and s['st_value']:
		syms.append((s['st_value'], s.name))
syms.sort()
addrs = [a for a, _ in syms]

icount = 0
phase = None
per = collections.Counter()
calls_sf = collections.Counter()
blockfn = {}

def fn_of(a):
	i = bisect.bisect_right(addrs, a) - 1
	return syms[i][1] if i >= 0 else '?'

ICL = [None] * (8192 // 16)
DCL = [None] * (4096 // 16)
imiss = dmiss = 0
def hook_mem(uc, access, addr, size, value, ud):
	global dmiss
	if access != UC_MEM_READ: return
	l = addr >> 4
	i = l & 255
	if DCL[i] != l:
		DCL[i] = l; dmiss += 1
def hook_block(uc, addr, size, ud):
	global icount, imiss
	n = size >> 2
	icount += n
	l = addr >> 4
	e = (addr + size - 1) >> 4
	while l <= e:
		i = l & 511
		if ICL[i] != l:
			ICL[i] = l; imiss += 1
		l += 1
	if phase is not None:
		f = blockfn.get(addr)
		if f is None:
			f = blockfn[addr] = fn_of(addr)
		per[f] += n

def cstr(a):
	b = bytearray()
	while True:
		c = mu.mem_read(a, 64)
		i = c.find(0)
		if i >= 0:
			b += c[:i]; return b.decode('utf-8', 'replace')
		b += c; a += 64

def cardpath(p): return CARD + p if p.startswith('/') else p

SF = ('__adddf3','__subdf3','__muldf3','__divdf3','__ltdf2','__gtdf2','__ledf2','__gedf2','__eqdf2','__nedf2','__unorddf2',
	'__floatsidf','__fixdfsi','__extendsfdf2','__truncdfsf2','__addsf3','__subsf3','__mulsf3','__divsf3','__floatsisf','__fixsfsi',
	'__ltsf2','__gtsf2','__lesf2','__gesf2','__eqsf2','__nesf2','__floatunsidf','__fixunsdfsi','__floatunsisf','__fixunssfsi')
t0 = 0
i0 = d0 = 0
def report(name, n):
	im = imiss - i0; dm = dmiss - d0
	cyc = 6 * n + 30 * im + 22 * dm
	print('== %-28s %9d instr  %9d cycles (model, CPI %.1f: ~%.0f ms at 48 MHz)  i-misses %d  d-misses %d' % (name, n, cyc, cyc / max(n, 1), cyc / 48000.0, im, dm))
	if not TOP: return
	sf = sum(v for k, v in per.items() if k in SF or k.startswith('__') and ('df' in k or 'sf' in k))
	print('     (soft-float %.1f%%)' % (100.0 * sf / max(n, 1)))
	for f, v in per.most_common(TOP):
		print('     %-30s %9d  %5.1f%%' % (f, v, 100.0 * v / max(n, 1)))

def hook_intr(uc, intno, ud):
	global phase, t0, per
	a7 = uc.reg_read(UC_RISCV_REG_A7)
	a0, a1, a2, a3 = [uc.reg_read(r) for r in (UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2, UC_RISCV_REG_A3)]
	r = 0
	if a7 == 64:
		sys.stdout.write(bytes(uc.mem_read(a1, a2)).decode('utf-8', 'replace')); r = a2
	elif a7 == 93:
		uc.emu_stop(); return
	elif a7 == 500:
		d = cardpath(cstr(a0))
		if not os.path.isdir(d): r = -1
		else:
			b = b''
			for e in sorted(os.listdir(d)):
				b += (b'd' if os.path.isdir(os.path.join(d, e)) else b'f') + e.encode() + b'\0'
			b = b[:a2]; uc.mem_write(a1, b); r = len(b)
	elif a7 == 501:
		p = cardpath(cstr(a0))
		if not os.path.isfile(p): r = -1
		else:
			with open(p, 'rb') as f: f.seek(a1); b = f.read(a3)
			uc.mem_write(a2, b); r = len(b)
	elif a7 in (502, 503):
		p = cardpath(cstr(a0))
		with open(p, 'wb' if a7 == 502 else 'ab') as f: f.write(bytes(uc.mem_read(a1, a2)))
		r = 1
	elif a7 == 504:
		p = cardpath(cstr(a0)); os.makedirs(p, exist_ok=True); r = 1
	elif a7 == 505:
		r = 1 if os.path.exists(cardpath(cstr(a0))) else 0
	elif a7 == 506:
		r = int((6 * icount + 30 * imiss + 22 * dmiss) / (MHZ * 1000))
	elif a7 == 507:
		global i0, d0
		phase = cstr(a0); per = collections.Counter(); t0 = icount; i0 = imiss; d0 = dmiss
	elif a7 == 508:
		report(phase, icount - t0); phase = None
	elif a7 == 509:
		p = FONT + cstr(a0)[5:]
		if not os.path.isfile(p): r = -1
		else:
			b = open(p, 'rb').read()[:a2]; uc.mem_write(a1, b); r = len(b)
	uc.reg_write(UC_RISCV_REG_A0, r & 0xffffffff)

mu.hook_add(UC_HOOK_BLOCK, hook_block)
mu.hook_add(UC_HOOK_INTR, hook_intr)
mu.hook_add(UC_HOOK_MEM_READ, hook_mem)
os.system("rm -rf %s && mkdir -p %s/data %s/opt/ark/edu-zeitlos && cp -r %s/sw/data/learn/edu-zeitlos/* %s/opt/ark/edu-zeitlos/" % (CARD, CARD, CARD, ROOT, CARD))
mu.emu_start(elf.header['e_entry'], 0xffffffff)
