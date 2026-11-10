# Zeitlos
#
# welcome's list of keyboard layouts, from sw/common/zkbd_layouts.c's
# z_kbd_layouts[] table: the config name and the description of every
# ordinary layout (input method 0), in the table's order.
#
#   { "de", "DE", "German", 1, ',', 49, kbd_de_keys, 0 },  ->  { "de", "German" },
#
# So a layout added there appears in welcome without welcome linking the
# key tables themselves (about 17KB it would carry for two strings a
# layout). Run by the Makefile (kbd_names.h); the output is not
# committed. Plain POSIX awk.

/^const z_kbd_layout_t z_kbd_layouts\[\] = \{/ { table = 1; next }
table && /^\};/ { table = 0 }
table && /^\t\{ "/ {
	# the three strings, then the five fields after them
	n = split($0, q, "\"")
	ime = $0
	sub(/[ \t]*\},[ \t]*$/, "", ime)
	sub(/.*,[ \t]*/, "", ime)
	if (ime == "0") printf("\t{ \"%s\", \"%s\" },\n", q[2], q[6])
}
