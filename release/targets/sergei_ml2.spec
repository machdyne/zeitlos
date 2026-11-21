# Sergei ML2 -- the complete system, with optical audio.
#
# No PMOD. PMOD_A1 is ball A13, which is also the S/PDIF transmitter
# (see boards/sergei_ml2.lpf), so this target keeps A13 for audio and
# has no GPIO.

description = Sergei ML2

base = sergei_ml2

# The DMA controller (rtl/dma.v) is off for now: it saved 8% of the
# card's time on Sergei ML2 for about 500 LUTs, and freed no CPU.
# docs/dma.md, "Off for now", says why and what would make it worth
# turning on (`DMA`, with `DMA_CHANNELS=1` on a board short of LUTs).

defines =
	+CPU_ZEITLOS32PS
	GPU_DDMI_AUDIO
