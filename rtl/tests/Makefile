# Zeitlos -- simulation tests for rtl/montmul.v
#
#   make          run the vector suite
#   make cycles   report engine cycles per multiply
#
# Needs iverilog. The vectors are checked in; regenerate them with
# gen_montmul_vectors.py if the moduli ever change.

IVERILOG ?= iverilog
VVP      ?= vvp

.PHONY: all cycles clean

all:
	$(IVERILOG) -g2012 -o /tmp/tb_montmul tb_montmul.v ../montmul.v
	$(VVP) /tmp/tb_montmul

cycles:
	$(IVERILOG) -g2012 -o /tmp/tb_montmul_cycles tb_montmul_cycles.v ../montmul.v
	$(VVP) /tmp/tb_montmul_cycles

clean:
	rm -f /tmp/tb_montmul /tmp/tb_montmul_cycles
