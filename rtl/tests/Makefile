# Zeitlos -- simulation tests for rtl/montmul.v, rtl/sha256.v and others
#
#   make          everything below except `cycles`
#   make classic  montmul's classic interface: 51 products against Python
#   make rf       montmul's register file: random MUL/ADD/SUB programs
#   make sha256   sha256.v: whole messages against hashlib
#   make cycles   report engine cycles per multiply
#   make vram_dirty  socctl.v's DIRTY: the stripe of every VRAM word, and the
#                    clear racing a write
#
# Needs iverilog (the OSS CAD Suite has it). The vectors are checked in;
# regenerate them with the gen_*_vectors.py scripts.

IVERILOG ?= iverilog
VVP      ?= vvp

.PHONY: all classic rf sha256 cycles spiflash vram_dirty clean

all: classic rf sha256 keccak vram_dirty

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
	$(VVP) /tmp/tb_spiflash /tmp/tb_vram_dirty

# rtl/socctl.v's DIRTY register (word 7); its header has the contract.
vram_dirty:
	$(IVERILOG) -g2012 -o /tmp/tb_vram_dirty tb_vram_dirty.v ../socctl.v
	$(VVP) /tmp/tb_vram_dirty

clean:
	rm -f /tmp/tb_montmul /tmp/tb_montmul_rf /tmp/tb_sha256 /tmp/tb_keccak /tmp/tb_montmul_cycles /tmp/tb_spiflash /tmp/tb_vram_dirty
