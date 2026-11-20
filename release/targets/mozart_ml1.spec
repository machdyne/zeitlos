# Mozart ML1 -- a complete system. No PMOD layer, because there are no
# PMOD sockets: the ethernet PHY, the audio DAC and the video output
# are all on the board, so there is exactly one Mozart configuration
# and it is the board spec.

description = Mozart ML1

base = mozart_ml1

# The DMA controller (rtl/dma.v, docs/dma.md): two channels, the SD
# card's SPI and the RMII MAC's buffers, so a film streams off the card
# and a frame is copied in or out without the CPU moving the bytes.
defines =
	DMA
