# zeitlos32 test suite
#
#   make                 everything: directed tests at three memory
#                        latencies, then 20 random programs
#   make run             directed tests only (fixed and random latency)
#   make random          random differential tests only
#   make one T=t_alu     one directed test, zero wait states
#   make wave T=t_alu    same, dumping tb_zeitlos32.vcd
#   make dis  T=t_alu    disassemble one test
#   make clean
#
# Options:
#   FAST_MUL=1           build the core with the DSP multiplier
#                        (rtl/boards.vh's `CPU_MUL_FAST)
#   SEEDS="1 2 3"        which random seeds to run
#   COUNT=400            instructions per random program
#
# Needs iverilog, cpp, and RISC-V binutils. Only the assembler and
# linker are used -- no C compiler, no libc, no libgcc -- so a distro
# package is enough (Debian/Ubuntu: binutils-riscv64-unknown-elf).
# This deliberately does NOT depend on the toolchain sw/common/arch.mk
# pins for building the OS, so the CPU can be tested on a machine that
# cannot build Zeitlos itself.
#
# Override the prefix if yours is named differently:
#   make CROSS=riscv32-unknown-elf-

# Cancel make's built-in rules. Its %.o: %.S rule would otherwise beat
# the cpp/as pipeline below and try to build these with the HOST
# compiler, which fails in a thoroughly confusing way.
.SUFFIXES:
%.o: %.S
%.o: %.c

CROSS   ?= riscv64-unknown-elf-
AS       = $(CROSS)as
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump
CPP     ?= cpp

IVERILOG ?= iverilog
VVP      ?= vvp

FAST_MUL ?= 0
SEEDS    ?= 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
COUNT    ?= 400

RTL = ../zeitlos32.v ../zeitlos32_muldiv.v

ASFLAGS  = -march=rv32im -mabi=ilp32
LDFLAGS  = -m elf32lriscv
CPPFLAGS = -Iprog -P

TESTS = t_alu t_branch t_mem t_muldiv t_csr t_irq t_illegal

HEX = $(addprefix prog/,$(addsuffix .hex,$(TESTS)))

# Wait-state profiles the directed suite is run against. 0 models
# rtl/mem/bram.v, 3 is roughly rtl/mem/sram.v, 12 is in the region of
# rtl/mem/sdram.v. Running all three is the point: a bus handshake bug
# that only shows up when ack is slow is exactly the kind of thing
# that otherwise reaches the board.
LATENCIES = 0 3 12

# ...and once more with the latency varying per access. Since
# rtl/arbiter_main.v arrived the CPU shares the main bus with the
# blitter's source reads, so a real access takes however long the
# arbiter takes to get round to it. Constant latency can hide an
# off-by-one that only bites when two consecutive accesses differ.
RANDLAT = 8

all: run random

# ------------------------------------------------------------------
# build
# ------------------------------------------------------------------

prog/%.i: prog/%.S prog/test.h
	$(CPP) $(CPPFLAGS) $< -o $@

prog/%.o: prog/%.i
	$(AS) $(ASFLAGS) -o $@ $<

prog/%.elf: prog/%.o prog/test.lds
	$(LD) $(LDFLAGS) -T prog/test.lds -o $@ $<

prog/%.bin: prog/%.elf
	$(OBJCOPY) -O binary $< $@

prog/%.hex: prog/%.bin bin2hex.py
	python3 bin2hex.py $< 16384 > $@

prog/%.asm: prog/%.elf
	$(OBJDUMP) -d $< > $@

tb_lat%.vvp: tb_zeitlos32.v $(RTL)
	$(IVERILOG) -g2005 -Ptb_zeitlos32.MEM_LAT=$* \
		-Ptb_zeitlos32.FAST_MUL=$(FAST_MUL) -o $@ tb_zeitlos32.v $(RTL)

tb_rand%.vvp: tb_zeitlos32.v $(RTL)
	$(IVERILOG) -g2005 -Ptb_zeitlos32.MEM_LAT=$(RANDLAT) \
		-Ptb_zeitlos32.MEM_RANDLAT=1 -Ptb_zeitlos32.SEED=$* \
		-Ptb_zeitlos32.FAST_MUL=$(FAST_MUL) -o $@ tb_zeitlos32.v $(RTL)

# ------------------------------------------------------------------
# directed tests
# ------------------------------------------------------------------

run: $(HEX)
	@fail=0; \
	for lat in $(LATENCIES); do \
		$(MAKE) -s tb_lat$$lat.vvp FAST_MUL=$(FAST_MUL); \
		echo "== directed, $$lat wait states, FAST_MUL=$(FAST_MUL) =="; \
		for t in $(TESTS); do \
			out=`$(VVP) -N tb_lat$$lat.vvp +hex=prog/$$t.hex 2>&1`; \
			case "$$out" in \
				*PASS*) printf "  %-12s %s\n" "$$t" "`echo \"$$out\" | grep PASS`" ;; \
				*)      printf "  %-12s FAIL\n%s\n" "$$t" "$$out"; fail=1 ;; \
			esac; \
		done; \
	done; \
	for sd in 1 2 3; do \
		$(MAKE) -s tb_rand$$sd.vvp FAST_MUL=$(FAST_MUL); \
		echo "== directed, 0-$(RANDLAT) wait states (random, seed $$sd) =="; \
		for t in $(TESTS); do \
			out=`$(VVP) -N tb_rand$$sd.vvp +hex=prog/$$t.hex 2>&1`; \
			case "$$out" in \
				*PASS*) printf "  %-12s %s\n" "$$t" "`echo \"$$out\" | grep PASS`" ;; \
				*)      printf "  %-12s FAIL\n%s\n" "$$t" "$$out"; fail=1 ;; \
			esac; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "DIRECTED FAILED"; exit 1; fi; \
	echo "DIRECTED PASSED"

# ------------------------------------------------------------------
# random differential tests
#
# Each seed is a fresh straight-line program checked against the
# golden model in gen_random.py. A failure leaves prog/t_rand_<seed>.S
# in place so it can be disassembled and re-run on its own.
# ------------------------------------------------------------------

random: tb_lat0.vvp
	@fail=0; \
	echo "== random, $(COUNT) instructions/seed, FAST_MUL=$(FAST_MUL) =="; \
	for s in $(SEEDS); do \
		./gen_random.py --seed $$s --count $(COUNT) > prog/t_rand_$$s.S; \
		$(MAKE) -s prog/t_rand_$$s.hex || exit 1; \
		out=`$(VVP) -N tb_lat0.vvp +hex=prog/t_rand_$$s.hex 2>&1`; \
		case "$$out" in \
			*PASS*) printf "  seed %-5s %s\n" "$$s" "`echo \"$$out\" | grep PASS`"; \
			        rm -f prog/t_rand_$$s.* ;; \
			*)      printf "  seed %-5s FAIL\n%s\n" "$$s" "$$out"; \
			        echo "    kept prog/t_rand_$$s.S"; fail=1 ;; \
		esac; \
	done; \
	if [ $$fail -ne 0 ]; then echo "RANDOM FAILED"; exit 1; fi; \
	echo "RANDOM PASSED"

# ------------------------------------------------------------------
# single test
# ------------------------------------------------------------------

T ?= t_alu

one: prog/$(T).hex tb_lat0.vvp
	$(VVP) -N tb_lat0.vvp +hex=prog/$(T).hex

wave: prog/$(T).hex tb_lat0.vvp
	$(VVP) -N tb_lat0.vvp +hex=prog/$(T).hex +vcd

dis: prog/$(T).asm
	@cat prog/$(T).asm

clean:
	rm -f prog/*.i prog/*.o prog/*.elf prog/*.bin prog/*.hex prog/*.asm
	rm -f prog/t_rand_*.S *.vvp *.vcd

.PHONY: all run random one wave dis clean
.PRECIOUS: prog/%.i prog/%.o prog/%.elf prog/%.bin prog/%.hex
