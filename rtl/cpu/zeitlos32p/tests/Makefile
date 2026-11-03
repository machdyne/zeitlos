# zeitlos32p test suite
#
#   make                 everything: directed tests at three latencies,
#                        then random latency with random not-ready
#                        cycles, then 20 random programs, then the
#                        lockstep comparison with zeitlos32
#   make run             directed tests only
#   make random          random programs only
#   make lockstep        zeitlos32p against zeitlos32, write by write
#   make local           this directory's own single-core programs
#                        (prog/: t_timer -- picorv32's IRQ timer, which
#                        zeitlos32 does not have -- and t_hazard), at
#                        every latency
#   make mpu             zeitlos32p + rtl/mpu_split.v against zeitlos32 +
#                        rtl/mpu.v, on prog/t_mpu.S and prog/t_mtu.S
#                        (switching the MTU base; tb_mpu_lockstep.v)
#   make stress          gen_stress.py programs (STRESS_SEEDS), zeitlos32p
#                        on the real split cache against zeitlos32, at
#                        three latencies (tb_mpu_lockstep.v)
#   make one T=t_alu     one directed test, zero latency
#   make clean
#
# Options:
#   FAST_MUL=1           the DSP multiplier (`CPU_MUL_FAST)
#   SEEDS="1 2 3"        random-program seeds
#   COUNT=400            instructions per random program
#
# The programs are zeitlos32's (../../zeitlos32/tests/prog), built by
# that directory's Makefile: the same binaries, so the two cores are
# held to exactly the same tests. Needs iverilog, cpp and RISC-V
# binutils, as zeitlos32's suite.

.SUFFIXES:

IVERILOG ?= iverilog
VVP      ?= vvp
FAST_MUL ?= 0
SEEDS    ?= 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
COUNT    ?= 400

Z32  = ../../zeitlos32
PROG = $(Z32)/tests/prog
UNITS = $(Z32)/zeitlos32_decode.v $(Z32)/zeitlos32_alu.v \
	$(Z32)/zeitlos32_lsu.v $(Z32)/zeitlos32_muldiv.v
RTL  = ../zeitlos32p.v $(UNITS)

TESTS = t_alu t_branch t_mem t_muldiv t_csr t_irq t_illegal
HEX   = $(addprefix $(PROG)/,$(addsuffix .hex,$(TESTS)))

# Latency on each port: 0 is a cache hit every time, 3 and 12 slow
# memory. The two ports get them crossed as well, so a slow I side
# against a fast D side (and the reverse) is exercised.
LATENCIES = 0_0 3_3 12_12 0_12 12_0
RANDLAT = 12

# BTB_BITS=0: the core without branch prediction (`make nobtb`)
BTB_BITS ?= 6

all: run local random lockstep mpu stress irqstress nobtb

nobtb:
	@$(MAKE) -s BTB_BITS=0 run local lockstep
	@rm -f tb_l*.vvp tb_r*.vvp tb_lock*.vvp

$(PROG)/%.hex:
	$(MAKE) -s -C $(Z32)/tests prog/$*.hex

tb_l%.vvp: tb_zeitlos32p.v $(RTL)
	$(IVERILOG) -g2005 \
		-Ptb_zeitlos32p.I_LAT=$(word 1,$(subst _, ,$*)) \
		-Ptb_zeitlos32p.D_LAT=$(word 2,$(subst _, ,$*)) \
		-Ptb_zeitlos32p.FAST_MUL=$(FAST_MUL) -Ptb_zeitlos32p.BTB_BITS=$(BTB_BITS) \
		-o $@ tb_zeitlos32p.v $(RTL)

tb_r%.vvp: tb_zeitlos32p.v $(RTL)
	$(IVERILOG) -g2005 -Ptb_zeitlos32p.I_LAT=$(RANDLAT) \
		-Ptb_zeitlos32p.D_LAT=$(RANDLAT) -Ptb_zeitlos32p.RANDLAT=1 \
		-Ptb_zeitlos32p.BUSY=1 -Ptb_zeitlos32p.SEED=$* \
		-Ptb_zeitlos32p.FAST_MUL=$(FAST_MUL) -Ptb_zeitlos32p.BTB_BITS=$(BTB_BITS) \
		-o $@ tb_zeitlos32p.v $(RTL)

run: $(HEX)
	@fail=0; \
	for lat in $(LATENCIES); do \
		rm -f tb_l$$lat.vvp; $(MAKE) -s tb_l$$lat.vvp FAST_MUL=$(FAST_MUL); \
		echo "== directed, I/D latency $$lat, FAST_MUL=$(FAST_MUL) =="; \
		for t in $(TESTS); do \
			out=`$(VVP) -N tb_l$$lat.vvp +hex=$(PROG)/$$t.hex 2>&1`; \
			case "$$out" in \
				*PASS*) printf "  %-12s %s\n" "$$t" "`echo \"$$out\" | grep PASS`" ;; \
				*)      printf "  %-12s FAIL\n%s\n" "$$t" "$$out"; fail=1 ;; \
			esac; \
		done; \
	done; \
	for sd in 1 2 3; do \
		rm -f tb_r$$sd.vvp; $(MAKE) -s tb_r$$sd.vvp FAST_MUL=$(FAST_MUL); \
		echo "== directed, latency 0-$(RANDLAT) + random not-ready (seed $$sd) =="; \
		for t in $(TESTS); do \
			out=`$(VVP) -N tb_r$$sd.vvp +hex=$(PROG)/$$t.hex 2>&1`; \
			case "$$out" in \
				*PASS*) printf "  %-12s %s\n" "$$t" "`echo \"$$out\" | grep PASS`" ;; \
				*)      printf "  %-12s FAIL\n%s\n" "$$t" "$$out"; fail=1 ;; \
			esac; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "DIRECTED FAILED"; exit 1; fi; \
	echo "DIRECTED PASSED"

# zeitlos32's random straight-line programs, each checked against
# gen_random.py's golden model, here at zero latency and at random
# latency with random not-ready cycles.
random:
	@fail=0; \
	rm -f tb_l0_0.vvp tb_r7.vvp; \
	$(MAKE) -s tb_l0_0.vvp tb_r7.vvp FAST_MUL=$(FAST_MUL); \
	echo "== random, $(COUNT) instructions/seed, FAST_MUL=$(FAST_MUL) =="; \
	for s in $(SEEDS); do \
		$(Z32)/tests/gen_random.py --seed $$s --count $(COUNT) > $(PROG)/t_rand_$$s.S; \
		$(MAKE) -s -C $(Z32)/tests prog/t_rand_$$s.hex || exit 1; \
		for tb in tb_l0_0 tb_r7; do \
			out=`$(VVP) -N $$tb.vvp +hex=$(PROG)/t_rand_$$s.hex 2>&1`; \
			case "$$out" in \
				*PASS*) printf "  seed %-4s %-8s %s\n" "$$s" "$$tb" "`echo \"$$out\" | grep PASS`" ;; \
				*)      printf "  seed %-4s %-8s FAIL\n%s\n" "$$s" "$$tb" "$$out"; fail=1 ;; \
			esac; \
		done; \
		[ $$fail -eq 0 ] && rm -f $(PROG)/t_rand_$$s.*; \
	done; \
	if [ $$fail -ne 0 ]; then echo "RANDOM FAILED"; exit 1; fi; \
	echo "RANDOM PASSED"

one: $(PROG)/$(T).hex tb_l0_0.vvp
	$(VVP) -N tb_l0_0.vvp +hex=$(PROG)/$(T).hex

# -- lockstep: see tb_lockstep.v --------------------------------------

LOCK_RTL = tb_lockstep.v $(RTL) $(Z32)/zeitlos32.v
LOCK_PROGS = t_alu t_branch t_mem t_muldiv t_illegal

lockstep: $(addprefix $(PROG)/,$(addsuffix .hex,$(LOCK_PROGS)))
	@fail=0; \
	for sd in 1 2 3; do \
		$(IVERILOG) -g2005 -Ptb_lockstep.SEED=$$sd -Ptb_lockstep.FAST_MUL=$(FAST_MUL) \
			-Ptb_lockstep.BTB_BITS=$(BTB_BITS) \
			-o tb_lock$$sd.vvp $(LOCK_RTL) || exit 1; \
		echo "== lockstep with zeitlos32, random latencies (seed $$sd) =="; \
		for t in $(LOCK_PROGS); do \
			out=`$(VVP) -N tb_lock$$sd.vvp +hex=$(PROG)/$$t.hex 2>&1`; \
			case "$$out" in \
				*MATCH*) printf "  %-12s %s\n" "$$t" "`echo \"$$out\" | grep MATCH`" ;; \
				*)       printf "  %-12s FAIL\n%s\n" "$$t" "$$out"; fail=1 ;; \
			esac; \
		done; \
		for s in 1 2 3 4 5; do \
			$(Z32)/tests/gen_random.py --seed $$((s * 100 + sd)) --count $(COUNT) > $(PROG)/t_lk_$$s.S; \
			$(MAKE) -s -C $(Z32)/tests prog/t_lk_$$s.hex || exit 1; \
			out=`$(VVP) -N tb_lock$$sd.vvp +hex=$(PROG)/t_lk_$$s.hex 2>&1`; \
			case "$$out" in \
				*MATCH*) printf "  random %-5s %s\n" "$$((s * 100 + sd))" "`echo \"$$out\" | grep MATCH`"; rm -f $(PROG)/t_lk_$$s.* ;; \
				*)       printf "  random %-5s FAIL\n%s\n" "$$((s * 100 + sd))" "$$out"; fail=1 ;; \
			esac; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "LOCKSTEP FAILED"; exit 1; fi; \
	echo "LOCKSTEP PASSED"

# -- the MPU: see tb_mpu_lockstep.v ----------------------------------
#
# prog/t_mpu.S is this directory's own program, built like zeitlos32's
# (its test.h and test.lds).

CROSS ?= riscv64-unknown-elf-
MPU_RTL = tb_mpu_lockstep.v $(RTL) $(Z32)/zeitlos32.v \
	../../../mpu.v ../../../mpu_split.v ../../../cache_split.v

prog/%.hex: prog/%.S $(PROG)/test.h $(PROG)/test.lds
	cpp -I$(PROG) -P $< -o prog/$*.i
	$(CROSS)as -march=rv32im -mabi=ilp32 -o prog/$*.o prog/$*.i
	$(CROSS)ld -m elf32lriscv -T $(PROG)/test.lds -o prog/$*.elf prog/$*.o
	$(CROSS)objcopy -O binary prog/$*.elf prog/$*.bin
	python3 $(Z32)/tests/bin2hex.py prog/$*.bin 16384 > $@

LOCAL_TESTS = t_timer t_hazard t_btb

local: $(addprefix prog/,$(addsuffix .hex,$(LOCAL_TESTS)))
	@fail=0; \
	for lat in $(LATENCIES); do \
		rm -f tb_l$$lat.vvp; $(MAKE) -s tb_l$$lat.vvp FAST_MUL=$(FAST_MUL); \
		for t in $(LOCAL_TESTS); do \
			out=`$(VVP) -N tb_l$$lat.vvp +hex=prog/$$t.hex 2>&1`; \
			case "$$out" in \
				*PASS*) printf "  %-9s lat %-6s %s\n" "$$t" "$$lat" "`echo \"$$out\" | grep PASS`" ;; \
				*)      printf "  %-9s lat %-6s FAIL\n%s\n" "$$t" "$$lat" "$$out"; fail=1 ;; \
			esac; \
		done; \
	done; \
	for sd in 1 2 3; do \
		rm -f tb_r$$sd.vvp; $(MAKE) -s tb_r$$sd.vvp FAST_MUL=$(FAST_MUL); \
		for t in $(LOCAL_TESTS); do \
			out=`$(VVP) -N tb_r$$sd.vvp +hex=prog/$$t.hex 2>&1`; \
			case "$$out" in \
				*PASS*) printf "  %-9s seed %-5s %s\n" "$$t" "$$sd" "`echo \"$$out\" | grep PASS`" ;; \
				*)      printf "  %-9s seed %-5s FAIL\n%s\n" "$$t" "$$sd" "$$out"; fail=1 ;; \
			esac; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "LOCAL FAILED"; exit 1; fi; \
	echo "LOCAL PASSED"

timer: local

# -- stress: see gen_stress.py ---------------------------------------

STRESS_SEEDS ?= 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20

stress:
	@fail=0; \
	for lat in 0 4 12; do \
		$(IVERILOG) -g2005 -Ptb_mpu_lockstep.LAT=$$lat -Ptb_mpu_lockstep.SEED=$$lat \
			-o tb_st$$lat.vvp $(MPU_RTL) || exit 1; \
	done; \
	for s in $(STRESS_SEEDS); do \
		./gen_stress.py --seed $$s --blocks 150 > prog/t_st_$$s.S; \
		$(MAKE) -s prog/t_st_$$s.hex || exit 1; \
		for lat in 0 4 12; do \
			out=`$(VVP) -N tb_st$$lat.vvp +hex=prog/t_st_$$s.hex 2>&1`; \
			case "$$out" in \
				*MATCH*) ;; \
				*) printf "  stress seed %s lat %s FAIL\n%s\n" "$$s" "$$lat" "$$out"; fail=1 ;; \
			esac; \
		done; \
		[ $$fail -eq 0 ] && rm -f prog/t_st_$$s.*; \
	done; \
	if [ $$fail -ne 0 ]; then echo "STRESS FAILED"; exit 1; fi; \
	echo "STRESS PASSED ($(words $(STRESS_SEEDS)) programs x 3 latencies)"

# -- irqstress: gen_stress.py --irq programs on tb_irq_stress.v -------
# IRQ 3 pulses at random on zeitlos32p only; the final memories must
# match zeitlos32's (except the handler's counter)

IRQ_SEEDS ?= 1 2 3 4 5 6 7 8 9 10
IRQ_RTL = tb_irq_stress.v $(RTL) $(Z32)/zeitlos32.v \
	../../../mpu.v ../../../mpu_split.v ../../../cache_split.v

irqstress:
	@fail=0; \
	for cfg in 4:40 0:15 12:90 4:7; do \
		lat=$${cfg%:*}; gap=$${cfg#*:}; \
		$(IVERILOG) -g2005 -Ptb_irq_stress.LAT=$$lat -Ptb_irq_stress.IRQ_GAP=$$gap \
			-Ptb_irq_stress.SEED=$$gap -o tb_irq_$$lat\_$$gap.vvp $(IRQ_RTL) || exit 1; \
	done; \
	for s in $(IRQ_SEEDS); do \
		./gen_stress.py --seed $$s --blocks 120 --irq > prog/t_si_$$s.S; \
		$(MAKE) -s prog/t_si_$$s.hex || exit 1; \
		for v in tb_irq_*.vvp; do \
			out=`$(VVP) -N $$v +hex=prog/t_si_$$s.hex 2>&1`; \
			case "$$out" in \
				*SAME*) ;; \
				*) printf "  irq seed %s %s FAIL\n%s\n" "$$s" "$$v" "$$out"; fail=1 ;; \
			esac; \
		done; \
		[ $$fail -eq 0 ] && rm -f prog/t_si_$$s.*; \
	done; \
	if [ $$fail -ne 0 ]; then echo "IRQ STRESS FAILED"; exit 1; fi; \
	echo "IRQ STRESS PASSED ($(words $(IRQ_SEEDS)) programs x 4 configurations)"

mpu: prog/t_mpu.hex prog/t_mtu.hex
	@fail=0; \
	for lat in 0 3 8 12; do \
		for sd in 1 2 3; do \
			$(IVERILOG) -g2005 -Ptb_mpu_lockstep.SEED=$$sd -Ptb_mpu_lockstep.LAT=$$lat \
				-o tb_mpu.vvp $(MPU_RTL) || exit 1; \
			for t in t_mpu t_mtu; do \
				out=`$(VVP) -N tb_mpu.vvp +hex=prog/$$t.hex 2>&1`; \
				case "$$out" in \
					*MATCH*) printf "  %s lat %-3s seed %s  %s\n" "$$t" "$$lat" "$$sd" "`echo \"$$out\" | grep MATCH`" ;; \
					*)       printf "  %s lat %-3s seed %s  FAIL\n%s\n" "$$t" "$$lat" "$$sd" "$$out"; fail=1 ;; \
				esac; \
			done; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "MPU FAILED"; exit 1; fi; \
	echo "MPU PASSED"

clean:
	rm -f *.vvp *.vcd prog/*.i prog/*.o prog/*.elf prog/*.bin prog/*.hex

.PHONY: all run local timer random lockstep mpu stress irqstress nobtb one clean
