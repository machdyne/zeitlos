# Zeitlos -- simulation tests for rtl/montmul.v, rtl/sha256.v and others
#
#   make          everything below except `cycles`
#   make classic  montmul's classic interface: 51 products against Python
#   make rf       montmul's register file: random MUL/ADD/SUB programs
#   make sha256   sha256.v: whole messages against hashlib
#   make cycles   report engine cycles per multiply
#   make vram_dirty  socctl.v's DIRTY: the stripe of every VRAM word, and the
#                    clear racing a write
#   make 8b10b    gpio_stream.v's 8b/10b against every symbol (zlink)
#   make spiflash_netlist  tb_spiflash against spiflash.v as yosys builds
#                 it for ECP5 -- the page buffer is a block RAM (needs yosys)
#
# Needs iverilog (the OSS CAD Suite has it). The vectors are checked in;
# regenerate them with the gen_*_vectors.py scripts.

IVERILOG ?= iverilog
VVP      ?= vvp

.PHONY: all classic rf sha256 cycles spiflash spiflash_netlist vram_dirty 8b10b clean

all: classic rf sha256 keccak vram_dirty 8b10b

classic:
	$(IVERILOG) -g2012 -o /tmp/tb_montmul tb_montmul.v ../montmul.v
	$(VVP) /tmp/tb_montmul

rf:
	$(IVERILOG) -g2012 -o /tmp/tb_montmul_rf tb_montmul_rf.v ../montmul.v
	$(VVP) /tmp/tb_montmul_rf

keccak:
	$(IVERILOG) -o /tmp/tb_keccak tb_keccak.v ../keccak.v
	$(VVP) /tmp/tb_keccak

sha256:
	$(IVERILOG) -g2012 -o /tmp/tb_sha256 tb_sha256.v ../sha256.v
	$(VVP) /tmp/tb_sha256

cycles:
	$(IVERILOG) -g2012 -o /tmp/tb_montmul_cycles tb_montmul_cycles.v ../montmul.v
	$(VVP) /tmp/tb_montmul_cycles

# rtl/spiflash.v against a behavioural W25Q16 (w25q16_model.v):
# reads, erase, program, and every refusal. docs/spiflash.md.
spiflash:
	$(IVERILOG) -g2012 -o /tmp/tb_spiflash tb_spiflash.v w25q16_model.v ../spiflash.v
	$(VVP) /tmp/tb_spiflash /tmp/tb_vram_dirty /tmp/tb_8b10b

# rtl/socctl.v's DIRTY register (word 7); its header has the contract.
vram_dirty:
	$(IVERILOG) -g2012 -o /tmp/tb_vram_dirty tb_vram_dirty.v ../socctl.v
	$(VVP) /tmp/tb_vram_dirty

# The same test against the ECP5 netlist. yosys's cells_sim_ecp5.v
# declares DP16KD as an empty black box -- outputs undriven, so the RAM
# reads X -- which is replaced here by dp16kd_model.v; tbuf_model.v
# supplies the $$_TBUF_ cells write_verilog emits. YOSYS_LATTICE is the
# directory holding cells_sim_ecp5.v (`yosys-config --datdir`/lattice).
# The netlist is written here rather than to /tmp so that YoWASP's
# sandboxed yosys, which only sees the current directory, works too.
YOSYS ?= yosys
YOSYS_LATTICE ?= $(shell yosys-config --datdir 2>/dev/null)/lattice
spiflash_netlist:
	$(YOSYS) -q -p "read_verilog ../spiflash.v; synth_ecp5 -top spiflash_wb; write_verilog -noattr spiflash_net.tmp.v"
	python3 -c "import re,sys; s=open(sys.argv[1]).read(); \
		sys.stdout.write(re.sub(r'\(\*\s*blackbox\s*\*\)\s*module DP16KD\b.*?endmodule\n', '', s, flags=re.S))" \
		$(YOSYS_LATTICE)/cells_sim_ecp5.v > /tmp/cells_sim_ecp5_nodp.v
	$(IVERILOG) -g2012 -DNO_INCLUDES -I $(YOSYS_LATTICE) -o /tmp/tb_spiflash_net tb_spiflash.v \
		w25q16_model.v spiflash_net.tmp.v tbuf_model.v /tmp/cells_sim_ecp5_nodp.v dp16kd_model.v
	rm -f spiflash_net.tmp.v
	$(VVP) -n /tmp/tb_spiflash_net

# rtl/gpio_stream.v's 8b/10b tables against 8b10b_vectors.txt, which
# tools/gen_8b10b.py writes after checking the code's properties.
8b10b:
	$(IVERILOG) -g2012 -o /tmp/tb_8b10b tb_8b10b.v ../gpio_stream.v
	$(VVP) -n /tmp/tb_8b10b

clean:
	rm -f /tmp/tb_montmul /tmp/tb_montmul_rf /tmp/tb_sha256 /tmp/tb_keccak /tmp/tb_montmul_cycles /tmp/tb_spiflash /tmp/tb_vram_dirty /tmp/tb_8b10b
