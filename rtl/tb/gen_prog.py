def I(imm,rs1,f3,rd,op): return ((imm&0xfff)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|op
def R(f7,rs2,rs1,f3,rd,op): return (f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|op
def B(off,rs2,rs1,f3,op):
    o=off&0x1fff
    return ((( o>>12)&1)<<31)|(((o>>5)&0x3f)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|((((o>>1)&0xf))<<8)|(((o>>11)&1)<<7)|op
def J(off,rd,op):
    o=off&0x1fffff
    return ((((o>>20)&1)<<31)|(((o>>1)&0x3ff)<<21)|(((o>>11)&1)<<20)|(((o>>12)&0xff)<<12)|(rd<<7)|op)

addi=lambda rd,rs1,i: I(i,rs1,0,rd,0x13)
slli=lambda rd,rs1,s: I(s,rs1,1,rd,0x13)
srli=lambda rd,rs1,s: I(s,rs1,5,rd,0x13)
add =lambda rd,rs1,rs2: R(0,rs2,rs1,0,rd,0x33)
xor =lambda rd,rs1,rs2: R(0,rs2,rs1,4,rd,0x33)
blt =lambda rs1,rs2,off: B(off,rs2,rs1,4,0x63)
jal =lambda rd,off: J(off,rd,0x6f)

# mirrors k_cpu_report()'s loop: pure register work, no memory, no mul
p=[]
p.append(addi(5,0,0))      # 0x00 i=0
p.append(addi(6,0,100))    # 0x04 x=100
p.append(addi(8,0,64))     # 0x08 limit=64
LOOP=len(p)*4              # 0x0c
p.append(add (6,6,5))      # x += i
p.append(srli(7,6,7))
p.append(xor (6,6,7))      # x ^= x>>7
p.append(slli(7,6,3))
p.append(add (6,6,7))      # x += x<<3
p.append(addi(5,5,1))      # i++
p.append(blt (5,8,LOOP-(len(p)*4)))
p.append(addi(5,0,0))      # i=0
p.append(jal (0,LOOP-(len(p)*4)))
open("prog.hex","w").write("\n".join("%08x"%w for w in p)+"\n")
print("prog.hex      : %d insns, register-only loop"%len(p))

# --- second program: realistic mix with loads and stores ---
#
# prog.hex above mirrors k_cpu_report() exactly, which means it does no
# loads or stores at all. That makes it a near-pure measure of FETCH
# latency -- useful for isolating the cache, but it is not what real
# code looks like, and it cannot show the difference between a board
# with fast data memory and one without. This one is ~29% memory ops,
# which is much closer to typical compiled RV32I.
def U(imm,rd,op): return (imm&0xfffff)<<12 | (rd<<7) | op
lui =lambda rd,i: U(i,rd,0x37)
lw  =lambda rd,rs1,off: I(off,rs1,2,rd,0x03)
def sw(rs2,rs1,off):
    o=off&0xfff
    return ((o>>5)<<25)|(rs2<<20)|(rs1<<15)|(2<<12)|((o&0x1f)<<7)|0x23

q=[]
q.append(addi(5,0,0))
q.append(addi(6,0,100))
q.append(addi(8,0,64))
q.append(lui (9,0x40002))   # data base, well clear of the code
L2=len(q)*4
q.append(lw  (10,9,0))
q.append(add (6,6,10))
q.append(srli(7,6,7))
q.append(xor (6,6,7))
q.append(sw  (6,9,4))
q.append(addi(5,5,1))
q.append(blt (5,8,L2-(len(q)*4)))
q.append(addi(5,0,0))
q.append(jal (0,L2-(len(q)*4)))
open("prog_mem.hex","w").write("\n".join("%08x"%w for w in q)+"\n")
print("prog_mem.hex  : %d insns, 7-insn loop with 2 memory ops (29%%)"%len(q))

# --- multiply benchmark, two ways ---
#
# The point of rv32im. prog_mulhw.hex uses a single MUL instruction;
# prog_mulsw.hex uses a shift-add loop, which is what libgcc's
# __mulsi3 does on rv32i and therefore what this codebase runs today
# every time C code contains a `*`.
mul =lambda rd,rs1,rs2: R(1,rs2,rs1,0,rd,0x33)
srli_=srli
beq =lambda rs1,rs2,off: B(off,rs2,rs1,0,0x63)
bne =lambda rs1,rs2,off: B(off,rs2,rs1,1,0x63)
andi=lambda rd,rs1,i: I(i,rs1,7,rd,0x13)

# hardware: x12 = x10 * x11, in the loop
h=[]
h.append(addi(10,0,1234))
h.append(addi(11,0,567))
h.append(addi(5,0,0))
h.append(addi(8,0,64))
LH=len(h)*4
h.append(mul (12,10,11))
h.append(add (6,6,12))
h.append(addi(5,5,1))
h.append(blt (5,8,LH-(len(h)*4)))
h.append(addi(5,0,0))
h.append(jal (0,LH-(len(h)*4)))
open("prog_mulhw.hex","w").write("\n".join("%08x"%w for w in h)+"\n")

# software: the same product via 32 iterations of shift-add
w=[]
w.append(addi(10,0,1234))
w.append(addi(11,0,567))
w.append(addi(5,0,0))
w.append(addi(8,0,64))
LW=len(w)*4
w.append(addi(12,0,0))      # acc = 0
w.append(add (13,0,10))     # a
w.append(add (14,0,11))     # b
INNER=len(w)*4
w.append(andi(15,14,1))
w.append(beq (15,0,8))      # skip add if bit clear
w.append(add (12,12,13))
w.append(slli(13,13,1))
w.append(srli(14,14,1))
w.append(bne (14,0,INNER-(len(w)*4)))
w.append(add (6,6,12))
w.append(addi(5,5,1))
w.append(blt (5,8,LW-(len(w)*4)))
w.append(addi(5,0,0))
w.append(jal (0,LW-(len(w)*4)))
open("prog_mulsw.hex","w").write("\n".join("%08x"%w for w in w)+"\n")
print("prog_mulhw.hex: 1 MUL insn per iteration")
print("prog_mulsw.hex: shift-add loop (what libgcc __mulsi3 does)")
