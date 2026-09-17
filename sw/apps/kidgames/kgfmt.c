/*
 * kidgames -- number formatting, without a formatter.
 *
 * Its own file, and not part of kgui.c, for a layering reason that
 * turned into a practical one: kgsave.c needs to write a score into a
 * line of text and needs nothing else from the UI at all. With these
 * living in kgui.c, linking the save code pulled in the window, the
 * message pump, the drawing primitives and every Zeitlos symbol
 * underneath them -- which made a pure parser test need a framebuffer.
 *
 * -- WHY NOT snprintf --
 *
 * One conversion specifier links picolibc's formatter: on the order of
 * 100KB, on an app that crashes on start if it outgrows the space the
 * loader has for it. An app whose printf calls are all plain strings
 * links none of it, because the compiler rewrites printf("literal")
 * into puts() and --gc-sections drops vfprintf. fputs is no escape
 * either -- it needs a FILE, which drags in ~40KB of stdio on its own.
 *
 * sw/apps/read hit this twice in one week, and both times the binary
 * tripling looked entirely unrelated to the one-line debug print that
 * caused it. See docs/app_runtime.md, and the Makefile's `noprintf`
 * target, which fails the build if one comes back.
 */

#include <string.h>

#include "kgui.h"

/* -- numbers, without a formatter ----------------------------------- */

const char *kg_utoa(char *buf, int buflen, unsigned long v)
{
	char tmp[12];
	int n = 0, i = 0;

	if (buflen <= 0) return buf;

	do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 12);

	while (n > 0 && i < buflen - 1) buf[i++] = tmp[--n];
	buf[i] = '\0';

	return buf;
}

const char *kg_itoa(char *buf, int buflen, long v)
{
	if (buflen <= 1) { if (buflen == 1) buf[0] = '\0'; return buf; }

	if (v < 0) {
		buf[0] = '-';
		/* -v on LONG_MIN is undefined; this app never gets near it,
		 * but casting through unsigned costs nothing and removes the
		 * question. */
		kg_utoa(buf + 1, buflen - 1, (unsigned long)(-(v + 1)) + 1u);
		return buf;
	}

	return kg_utoa(buf, buflen, (unsigned long)v);
}

void kg_append(char *buf, int buflen, const char *s)
{
	int n = (int)strlen(buf);

	while (*s && n < buflen - 1) buf[n++] = *s++;
	buf[n] = '\0';
}

void kg_append_num(char *buf, int buflen, long v)
{
	char tmp[14];

	kg_append(buf, buflen, kg_itoa(tmp, sizeof(tmp), v));
}

