# Sergei ML1 -- the complete system, with optical audio.
#
# No PMOD. PMOD_A1 is ball A13, which is also the S/PDIF transmitter
# (see boards/sergei_ml1.lpf), so this target keeps A13 for audio and
# has no GPIO. sergei_ml1_gpio makes the other choice.

description = Sergei ML1

base = sergei_ml1

# The DMA controller (rtl/dma.v, docs/dma.md): two channels, the SD
# card's SPI and the RMII MAC's buffers, so a film streams off the card
# and a frame is copied in or out without the CPU moving the bytes.
defines =
	DMA
