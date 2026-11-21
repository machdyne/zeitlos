# Mozart ML1 -- a complete system. No PMOD layer, because there are no
# PMOD sockets: the ethernet PHY, the audio DAC and the video output
# are all on the board, so there is exactly one Mozart configuration
# and it is the board spec.

description = Mozart ML1

base = mozart_ml1

# The DMA controller (rtl/dma.v) is off for now: it saved 8% of the
# card's time on Sergei ML2 for about 500 LUTs, and freed no CPU.
# docs/dma.md, "Off for now", says why and what would make it worth
# turning on (`DMA`, with `DMA_CHANNELS=1` on a board short of LUTs).

defines = +CPU_ZEITLOS32PS
