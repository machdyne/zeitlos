# Zeitlos
#
# Turns sw/common/zsoc.h's feature bits into info's name table, so a new
# bit shows up in info without anyone remembering to add it there:
#
#     #define Z_FEATURE_GPU_BLIT    (1u << 8)    ->   { 0, 8, "BLIT" },
#     #define Z_FEATURE2_VMOUSE     (1u << 11)   ->   { 1, 11, "VMOUSE" },
#
# Run by the Makefile (feat_names.h); the output is not committed.
# Plain POSIX awk. The subject prefixes MEM_, GPU_, CPU_ and SPI_ are
# dropped, which is how the tags always read in info (SDRAM, BLIT,
# SDCARD); nothing becomes ambiguous by it.

/^#define[ \t]+Z_FEATURE2?_[A-Z0-9_]+[ \t]+\(1u << [0-9]+\)/ {
	name = $2
	word = (name ~ /^Z_FEATURE2_/) ? 1 : 0
	sub(/^Z_FEATURE2?_/, "", name)
	sub(/^(MEM|GPU|CPU|SPI)_/, "", name)
	match($0, /<< [0-9]+/)
	bit = substr($0, RSTART + 3, RLENGTH - 3)
	printf "\t{ %d, %d, \"%s\" },\n", word, bit, name
}
