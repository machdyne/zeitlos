/*
 * zbrowse.c on the host: net's "found" text (znet.h, Z_NET_BROWSE_REPLY)
 * into printers -- the address made from the service, port and rp=,
 * the name from ty=, one printer offered two ways listed once -- and
 * the reply's map read, with net's messaging stubbed.
 */
#include <stdio.h>
#include <string.h>
#include "../zbrowse.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* -- stubs: what zbrowse.c asks of the system -- */
bool z_pid_lookup(const char *name, uint32_t *pid) { (void)name; *pid = 2; return true; }
static uint32_t sent_subject, sent_tag;
static char sent_str[64];
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	(void)to; sent_subject = subject; sent_tag = tag;
	snprintf(sent_str, sizeof(sent_str), "%s", obj.val.str);
	return Z_OK;
}
z_obj_t z_obj_str(const char *s) { z_obj_t o; o.type = Z_STR; o.val.str = (char *)s; return o; }
z_obj_t *z_map_find(z_obj_t *map, const char *key) {
	z_obj_table_t *t = map->val.ptr;
	for (uint32_t i = 0; i < t->len; i++) if (!strcmp(t->a[i].val.str, key)) return &t->b[i];
	return NULL;
}
z_rv z_msg_recv_match(z_msg_t *m, uint32_t s, uint32_t t, uint32_t to) { (void)m; (void)s; (void)t; (void)to; return Z_FAIL; }

static z_msg_t reply_msg(uint32_t tag, bool ok, const char *text) {
	static z_obj_table_t t;
	static z_obj_t k[2], v[2];
	z_msg_t m;
	memset(&m, 0, sizeof(m));
	t.len = 2; t.a = k; t.b = v;
	k[0] = z_obj_str("ok"); v[0].type = Z_UINT32; v[0].val.uint32 = ok;
	k[1] = z_obj_str(ok ? "found" : "error"); v[1] = z_obj_str(text);
	m.subject = Z_NET_BROWSE_REPLY; m.tag = tag;
	m.obj.type = Z_MAP; m.obj.val.ptr = &t;
	return m;
}

int main(void) {
	z_browse_printer_t p[8];
	const char *f =
		"_ipp._tcp\tCanon MG3600 series\t192.0.2.2\t631\ttxtvers=1\trp=ipp/print\tty=Canon MG3600 series\n"
		"_pdl-datastream._tcp\tHP LaserJet 400 (1.2.3)\t192.0.2.9\t9100\tty=HP LaserJet 400\n"
		"_pdl-datastream._tcp\tCanon raw\t192.0.2.2\t9100\n"
		"_ipp._tcp\tOdd port\t10.0.0.7\t8631\n"
		"_ipp._tcp\tEmpty rp\t10.0.0.8\t631\trp=\tty=\n"
		"_printer._tcp\tLPD\t10.0.0.9\t515\n"
		"_pdl-datastream._tcp\tBrother raw\t10.0.0.10\t9100\n"
		"_ipp._tcp\tBrother\t10.0.0.10\t631\trp=ipp/print\tty=Brother HL-L2350DW series\n"
		"broken line\n"
		"_ipp._tcp\tno newline\t10.0.0.12\t631";
	int n = z_browse_parse(f, p, 8);
	CHECK(n == 6);
	CHECK(!strcmp(p[0].name, "Canon MG3600 series") && !strcmp(p[0].uri, "ipp://192.0.2.2/ipp/print"));
	CHECK(!strcmp(p[1].name, "HP LaserJet 400") && !strcmp(p[1].uri, "socket://192.0.2.9"));
	CHECK(!strcmp(p[2].name, "Odd port") && !strcmp(p[2].uri, "ipp://10.0.0.7:8631/ipp/print"));
	CHECK(!strcmp(p[3].name, "Empty rp") && !strcmp(p[3].uri, "ipp://10.0.0.8/"));
	// the raw one first, then IPP for the same host: IPP replaces it in place
	CHECK(!strcmp(p[4].name, "Brother HL-L2350DW series") && !strcmp(p[4].uri, "ipp://10.0.0.10/ipp/print"));
	CHECK(!strcmp(p[5].uri, "ipp://10.0.0.12/ipp/print"));
	CHECK(z_browse_parse(f, p, 2) == 2);
	CHECK(z_browse_parse("", p, 8) == 0 && z_browse_parse(NULL, p, 8) == 0);

	// a name too long is cut, not overrun
	{
		char l[400];
		snprintf(l, sizeof(l), "_ipp._tcp\t%0100d\t1.2.3.4\t631\trp=%0100d\n", 0, 0);
		CHECK(z_browse_parse(l, p, 8) == 1);
		CHECK(strlen(p[0].name) == Z_BROWSE_NAME_MAX - 1 && strlen(p[0].uri) == Z_BROWSE_URI_MAX - 1);
	}

	// asking and the answer
	char err[64];
	uint32_t tag = z_browse_ask(Z_BROWSE_PRINTERS, err, sizeof(err));
	CHECK(tag && sent_subject == Z_NET_BROWSE && sent_tag == tag && !strcmp(sent_str, Z_BROWSE_PRINTERS));
	z_msg_t m = reply_msg(tag + 1, true, f);
	CHECK(z_browse_reply(&m, tag, p, 8, err, sizeof(err)) == -1);
	m = reply_msg(tag, true, f);
	CHECK(z_browse_reply(&m, tag, p, 8, err, sizeof(err)) == 6 && !err[0]);
	m = reply_msg(tag, false, "busy: another search is running");
	CHECK(z_browse_reply(&m, tag, p, 8, err, sizeof(err)) == 0 && !strcmp(err, "busy: another search is running"));
	CHECK(z_browse_reply(&m, 0, p, 8, err, sizeof(err)) == -1);
	CHECK(z_browse_printers(p, 8, err, sizeof(err)) == 0 && !strcmp(err, "no answer from net"));

	printf("test_zbrowse: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
