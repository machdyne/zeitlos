# zeitlos32ps test suite
#
#   make              directed tests at fixed and random latencies, random
#                     programs, lockstep against zeitlos32
#   make run          directed tests only
#   make random       random programs against their golden model
#   make lockstep     zeitlos32ps against zeitlos32, write by write
#   make one T=t_alu  one directed test, zero latency
#
# Options: FAST_MUL=1, SEEDS="1 2 3", COUNT=400
#
# The programs are zeitlos32's (../../zeitlos32/tests/prog), built by that
# directory's Makefile, so all three cores are held to the same tests.

.SUFFIXES:

IVERILOG ?= iverilog
VVP      ?= vvp
FAST_MUL ?= 0
SEEDS    ?= 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
COUNT    ?= 400

Z32  = ../../zeitlos32
PROG = $(Z32)/tests/prog
UNITS = $(Z32)/zeitlos32_decode.v $(Z32)/zeitlos32_alu.v \
	$(Z32)/zeitlos32_lsu.v $(Z32)/zeitlos32_muldiv_lean.v
RTL  = ../zeitlos32ps.v ../zeitlos32ps_alu.v $(UNITS)

TESTS = t_alu t_branch t_mem t_muldiv t_csr t_irq t_illegal
LATS  = 0 1 3 12

all: run random lockstep

HEX = $(addprefix $(PROG)/,$(addsuffix .hex,$(TESTS)))

progs: $(HEX)

$(PROG)/%.hex:
	$(MAKE) -s -C $(Z32)/tests prog/$*.hex

tb_l%.vvp: tb_zeitlos32ps.v $(RTL)
	$(IVERILOG) -g2005 -o $@ -Ptb_zeitlos32ps.LAT=$* -Ptb_zeitlos32ps.FAST_MUL=$(FAST_MUL) tb_zeitlos32ps.v $(RTL)

tb_rl%.vvp: tb_zeitlos32ps.v $(RTL)
	$(IVERILOG) -g2005 -o $@ -Ptb_zeitlos32ps.LAT=12 -Ptb_zeitlos32ps.RANDLAT=1 -Ptb_zeitlos32ps.BUSY=1 \
		-Ptb_zeitlos32ps.SEED=$* -Ptb_zeitlos32ps.FAST_MUL=$(FAST_MUL) tb_zeitlos32ps.v $(RTL)

run: progs
	@fail=0; \
	for lat in $(LATS); do \
		rm -f tb_l$$lat.vvp; $(MAKE) -s tb_l$$lat.vvp FAST_MUL=$(FAST_MUL); \
		echo "== directed, latency $$lat, FAST_MUL=$(FAST_MUL) =="; \
		for t in $(TESTS); do \
			r=`$(VVP) -n tb_l$$lat.vvp +hex=$(PROG)/$$t.hex | grep -E "PASS|FAIL"`; \
			echo "  $$t  $$r"; echo "$$r" | grep -q PASS || fail=1; \
		done; \
	done; \
	for sd in 1 2 3; do \
		rm -f tb_rl$$sd.vvp; $(MAKE) -s tb_rl$$sd.vvp FAST_MUL=$(FAST_MUL); \
		echo "== directed, random latency and busy, seed $$sd =="; \
		for t in $(TESTS); do \
			r=`$(VVP) -n tb_rl$$sd.vvp +hex=$(PROG)/$$t.hex | grep -E "PASS|FAIL"`; \
			echo "  $$t  $$r"; echo "$$r" | grep -q PASS || fail=1; \
		done; \
	done; \
	if [ $$fail = 0 ]; then echo "DIRECTED PASSED"; else echo "DIRECTED FAILED"; exit 1; fi

random: progs
	@fail=0; \
	$(MAKE) -s tb_l0.vvp tb_rl7.vvp FAST_MUL=$(FAST_MUL); \
	echo "== random, $(COUNT) instructions/seed, FAST_MUL=$(FAST_MUL) =="; \
	for sd in $(SEEDS); do \
		$(Z32)/tests/gen_random.py --seed $$sd --count $(COUNT) > $(PROG)/t_rand_$$sd.S; \
		$(MAKE) -s -C $(Z32)/tests prog/t_rand_$$sd.hex || exit 1; \
		for v in tb_l0 tb_rl7; do \
			r=`$(VVP) -n $$v.vvp +hex=$(PROG)/t_rand_$$sd.hex | grep -E "PASS|FAIL"`; \
			echo "  seed $$sd $$v  $$r"; echo "$$r" | grep -q PASS || fail=1; \
		done; \
	done; \
	if [ $$fail = 0 ]; then echo "RANDOM PASSED"; else echo "RANDOM FAILED"; exit 1; fi

LOCK_RTL = tb_lockstep.v $(RTL) $(Z32)/zeitlos32.v $(Z32)/zeitlos32_muldiv.v

tb_lock%.vvp: $(LOCK_RTL)
	$(IVERILOG) -g2005 -o $@ -Ptb_lockstep_ps.SEED=$* -Ptb_lockstep_ps.FAST_MUL=$(FAST_MUL) $(LOCK_RTL)

lockstep: progs
	@fail=0; \
	for sd in 1 2 3; do \
		rm -f tb_lock$$sd.vvp; $(MAKE) -s tb_lock$$sd.vvp FAST_MUL=$(FAST_MUL); \
		echo "== lockstep with zeitlos32, random latencies (seed $$sd) =="; \
		for t in t_alu t_branch t_mem t_muldiv t_illegal; do \
			r=`$(VVP) -n tb_lock$$sd.vvp +hex=$(PROG)/$$t.hex | grep -E "MATCH|FAIL" | head -3`; \
			echo "  $$t  $$r"; echo "$$r" | grep -q MATCH || fail=1; \
		done; \
		for r in 1 2 3 4 5; do \
			s=$$((r * 100 + sd)); \
			$(Z32)/tests/gen_random.py --seed $$s --count $(COUNT) > $(PROG)/t_rand_$$s.S; \
			$(MAKE) -s -C $(Z32)/tests prog/t_rand_$$s.hex || exit 1; \
			o=`$(VVP) -n tb_lock$$sd.vvp +hex=$(PROG)/t_rand_$$s.hex | grep -E "MATCH|FAIL" | head -3`; \
			echo "  random $$s  $$o"; echo "$$o" | grep -q MATCH || fail=1; \
		done; \
	done; \
	if [ $$fail = 0 ]; then echo "LOCKSTEP PASSED"; else echo "LOCKSTEP FAILED"; exit 1; fi

one: progs
	@rm -f tb_l0.vvp; $(MAKE) -s tb_l0.vvp FAST_MUL=$(FAST_MUL)
	$(VVP) -n tb_l0.vvp +hex=$(PROG)/$(T).hex

clean:
	rm -f *.vvp *.vcd rnd_*

# -- the MPU: tb_mpu_lockstep.v, zeitlos32ps + rtl/mpu_one.v +
# rtl/cache_one.v against zeitlos32 + rtl/mpu.v, on zeitlos32p's MPU
# programs (prog/t_mpu.S, prog/t_mtu.S there), four latencies x three
# seeds

Z32P = ../../zeitlos32p/tests
MPU_RTL = tb_mpu_lockstep.v $(RTL) $(Z32)/zeitlos32.v $(Z32)/zeitlos32_muldiv.v \
	../../../mpu.v ../../../mpu_one.v ../../../cache_one.v

$(Z32P)/prog/%.hex:
	$(MAKE) -C $(Z32P) prog/$*.hex

mpu: $(Z32P)/prog/t_mpu.hex $(Z32P)/prog/t_mtu.hex
	@fail=0; \
	for lat in 0 3 8 12; do \
		for sd in 1 2 3; do \
			$(IVERILOG) -g2005 -Ptb_mpu_lockstep_ps.SEED=$$sd -Ptb_mpu_lockstep_ps.LAT=$$lat \
				-o tb_mpu.vvp $(MPU_RTL) || exit 1; \
			for t in t_mpu t_mtu; do \
				out=`$(VVP) -N tb_mpu.vvp +hex=$(Z32P)/prog/$$t.hex 2>&1`; \
				case "$$out" in \
					*MATCH*) printf "  %s lat %-3s seed %s  %s\n" "$$t" "$$lat" "$$sd" "`echo \"$$out\" | grep MATCH`" ;; \
					*)       printf "  %s lat %-3s seed %s  FAIL\n%s\n" "$$t" "$$lat" "$$sd" "$$out"; fail=1 ;; \
				esac; \
			done; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "MPU FAILED"; exit 1; fi; \
	echo "MPU PASSED"

.PHONY: mpu

# -- rotates (ENABLE_ROT): prog/t_rot.S, this directory's own program,
# plus every directed test, with the rotates on

CROSS ?= riscv64-unknown-elf-

prog/%.hex: prog/%.S $(PROG)/test.h $(PROG)/test.lds
	cpp -I$(PROG) -P $< -o prog/$*.i
	$(CROSS)as -march=rv32im_zbb -mabi=ilp32 -o prog/$*.o prog/$*.i
	$(CROSS)ld -m elf32lriscv -T $(PROG)/test.lds -o prog/$*.elf prog/$*.o
	$(CROSS)objcopy -O binary prog/$*.elf prog/$*.bin
	python3 $(Z32)/tests/bin2hex.py prog/$*.bin 16384 > prog/$*.hex

rot: prog/t_rot.hex progs
	@fail=0; \
	for lat in 0 3; do \
		$(IVERILOG) -g2005 -o tb_rot.vvp -Ptb_zeitlos32ps.LAT=$$lat -Ptb_zeitlos32ps.ROT=1 \
			-Ptb_zeitlos32ps.FAST_MUL=$(FAST_MUL) tb_zeitlos32ps.v $(RTL) || exit 1; \
		for t in prog/t_rot $(addprefix $(PROG)/,$(TESTS)); do \
			r=`$(VVP) -n tb_rot.vvp +hex=$$t.hex | grep -E "PASS|FAIL"`; \
			echo "  rot lat $$lat `basename $$t`  $$r"; echo "$$r" | grep -q PASS || fail=1; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "ROT FAILED"; exit 1; fi; \
	echo "ROT PASSED"

.PHONY: rot
