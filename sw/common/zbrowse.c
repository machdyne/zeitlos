/*
 * Zeitlos
 *
 * zbrowse.h: asking net for printers, and reading its answer.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zbrowse.h"
#include "zeitlos.h"
#include "zsoc.h"		// Z_TICK_HZ
#include "znet.h"

static void set_err(char *err, uint32_t n, const char *m) {
	if (!err || !n) return;
	uint32_t i = 0;
	for (; m[i] && i < n - 1; i++) err[i] = m[i];
	err[i] = 0;
}

static int cat(char *d, int k, int n, const char *s, int sl) {
	for (int i = 0; i < sl && s[i] && k < n - 1; i++) d[k++] = s[i];
	d[k] = 0;
	return k;
}

// the field after `key=` among a line's TXT fields, or NULL; *len its length
static const char *txt_field(const char *f, const char *end, const char *key, int *len) {
	int kl = (int)strlen(key);
	while (f < end) {
		const char *e = f;
		while (e < end && *e != '\t') e++;
		if (e - f > kl && !strncmp(f, key, (size_t)kl) && f[kl] == '=') {
			*len = (int)(e - f - kl - 1);
			return f + kl + 1;
		}
		f = e < end ? e + 1 : e;
	}
	return NULL;
}

int z_browse_parse(const char *found, z_browse_printer_t *out, int max) {
	int n = 0;
	for (const char *l = found ? found : ""; *l && n < max; ) {
		const char *end = strchr(l, '\n');
		if (!end) end = l + strlen(l);
		const char *f[4];
		int fl[4], nf = 0;
		const char *p = l;
		while (nf < 4 && p <= end) {
			const char *e = p;
			while (e < end && *e != '\t') e++;
			f[nf] = p;
			fl[nf++] = (int)(e - p);
			p = e + 1;
		}
		const char *txt = p <= end ? p : end;
		if (nf == 4) {
			bool ipp = fl[0] == 9 && !strncmp(f[0], "_ipp._tcp", 9);
			bool raw = fl[0] == 20 && !strncmp(f[0], "_pdl-datastream._tcp", 20);
			z_browse_printer_t *o = &out[n];
			int k, tl = 0;
			const char *ty = txt_field(txt, end, "ty", &tl);
			if (ty && tl) cat(o->name, 0, Z_BROWSE_NAME_MAX, ty, tl);
			else cat(o->name, 0, Z_BROWSE_NAME_MAX, f[1], fl[1]);
			k = cat(o->uri, 0, Z_BROWSE_URI_MAX, ipp ? "ipp://" : "socket://", 9);
			k = cat(o->uri, k, Z_BROWSE_URI_MAX, f[2], fl[2]);
			bool dflt = (ipp && fl[3] == 3 && !strncmp(f[3], "631", 3)) ||
				(raw && fl[3] == 4 && !strncmp(f[3], "9100", 4));
			if (!dflt) {
				k = cat(o->uri, k, Z_BROWSE_URI_MAX, ":", 1);
				k = cat(o->uri, k, Z_BROWSE_URI_MAX, f[3], fl[3]);
			}
			if (ipp) {
				const char *rp = txt_field(txt, end, "rp", &tl);
				k = cat(o->uri, k, Z_BROWSE_URI_MAX, "/", 1);
				if (rp) cat(o->uri, k, Z_BROWSE_URI_MAX, rp, tl);
				else cat(o->uri, k, Z_BROWSE_URI_MAX, "ipp/print", 9);
			}
			// the same printer by both: keep the IPP one (listed first by
			// net only by chance, so look both ways)
			bool dup = false;
			for (int i = 0; (ipp || raw) && i < n; i++) {
				const char *a = out[i].uri, *c = o->uri;
				a += a[0] == 'i' ? 6 : 9;
				c += c[0] == 'i' ? 6 : 9;
				int al = 0, cl = 0;
				while (a[al] && a[al] != ':' && a[al] != '/') al++;
				while (c[cl] && c[cl] != ':' && c[cl] != '/') cl++;
				if (al != cl || strncmp(a, c, (size_t)al)) continue;
				dup = true;
				if (ipp && out[i].uri[0] == 's') out[i] = *o;
				break;
			}
			if ((ipp || raw) && !dup) n++;
		}
		l = *end ? end + 1 : end;
	}
	return n;
}

static uint32_t next_tag(void) {
	static uint32_t t;
	return 0xB0B00000u | (++t & 0xFFFFF);
}

uint32_t z_browse_ask(const char *services, char *err, uint32_t errlen) {
	uint32_t pid;
	if (!z_pid_lookup("net0", &pid)) {
		set_err(err, errlen, "net is not running");
		return 0;
	}
	uint32_t tag = next_tag();
	if (z_msg_new_send(pid, Z_NET_BROWSE, tag, z_obj_str(services)) != Z_OK) {
		set_err(err, errlen, "net did not take the request");
		return 0;
	}
	return tag;
}

int z_browse_reply(const z_msg_t *msg, uint32_t tag, z_browse_printer_t *out, int max,
	char *err, uint32_t errlen) {
	if (!tag || msg->subject != Z_NET_BROWSE_REPLY || msg->tag != tag) return -1;
	z_obj_t *ok = z_map_find((z_obj_t *)&msg->obj, "ok");
	if (!ok || ok->type != Z_UINT32 || !ok->val.uint32) {
		z_obj_t *e = z_map_find((z_obj_t *)&msg->obj, "error");
		set_err(err, errlen, e && e->type == Z_STR && e->val.str ? e->val.str : "the search failed");
		return 0;
	}
	z_obj_t *f = z_map_find((z_obj_t *)&msg->obj, "found");
	set_err(err, errlen, "");
	return f && f->type == Z_STR ? z_browse_parse(f->val.str, out, max) : 0;
}

int z_browse_printers(z_browse_printer_t *out, int max, char *err, uint32_t errlen) {
	uint32_t tag = z_browse_ask(Z_BROWSE_PRINTERS, err, errlen);
	if (!tag) return 0;
	z_msg_t m;
	if (z_msg_recv_match(&m, Z_NET_BROWSE_REPLY, tag, 5u * Z_TICK_HZ) != Z_OK) {
		set_err(err, errlen, "no answer from net");
		return 0;
	}
	return z_browse_reply(&m, tag, out, max, err, errlen);
}
