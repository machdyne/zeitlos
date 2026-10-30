/*
 * The print service's work on the build machine: a job (zpaper) in,
 * PWG raster, PDF or PostScript out -- to a file, to an IPP printer
 * (ippeveprinter in the tests), or to a socket printer. The same code
 * as on the board (run.c); only the files and the socket are POSIX.
 *
 *   print_host [options] JOB.zpp
 *     --pwg FILE | --pdf FILE | --ps FILE    write that
 *     --ipp URL [--prefer pdf|ps]            print there
 *     --socket URL                           PostScript to port 9100
 *     --probe URL                            what the printer says
 *     --dpi N  --srgb  --paper NAME  --root DIR
 *
 * Exit 0 when it worked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../../common/zpool.h"
#include "../run.h"
#include "../res.h"
#include "../textdoc.h"
#include "host_net.h"

static const char *root = "../../..";

static uint8_t *load(const char *path, uint32_t *len) {
	char p[600];
	FILE *f;
	long n;
	uint8_t *b;
	if (!strncmp(path, "/sys/fonts/", 11)) {
		const char *e = strrchr(path, '.');
		snprintf(p, sizeof(p), "%s/sw/data/font/%s/%s", root, e ? e + 1 : "", path + 11);
	} else if (!strncmp(path, "/media/images/", 14)) {
		snprintf(p, sizeof(p), "%s/sw/data/images/%s", root, path + 14);
	} else snprintf(p, sizeof(p), "%s", path);
	f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = pool_alloc((size_t)n + 1);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { pool_free(b); b = NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return b;
}

static bool file_write(out_t *o, const void *d, uint32_t n) {
	return fwrite(d, 1, n, (FILE *)o->ctx) == n;
}

static bool host_wait(void *ctx, int ms, const char *why) {
	(void)ctx;
	printf("print_host: %s, waiting\n", why);
	fflush(stdout);
	usleep((useconds_t)ms * 1000);
	return true;
}

static void show_info(const ipp_info_t *i) {
	int k;
	printf("printer: %s\n", i->make[0] ? i->make : "(no make and model)");
	printf("  state %d %s\n", i->state, i->state_reasons);
	printf("  formats:%s%s%s%s%s\n", i->pwg ? " pwg-raster" : "", i->pdf ? " pdf" : "",
		i->ps ? " postscript" : "", i->urf ? " urf" : "", i->jpeg ? " jpeg" : "");
	printf("  raster:%s%s at", i->sgray8 ? " sgray_8" : "", i->srgb8 ? " srgb_8" : "");
	for (k = 0; k < i->ndpi; k++) printf(" %d", i->dpi[k]);
	printf(" dpi\n  media %s, ready:", i->media_default);
	for (k = 0; k < i->nready; k++) printf(" %s", i->media_ready[k]);
	printf("\n  margins (1/100 mm) %d %d %d %d\n", i->margin[0], i->margin[1], i->margin[2], i->margin[3]);
	printf("  trays:");
	for (k = 0; k < i->nsources; k++) printf(" %s", i->sources[k]);
	printf("\n  loaded:");
	for (k = 0; k < i->ntray; k++) printf(" %s=%dx%d", i->tray[k].source, i->tray[k].w, i->tray[k].h);
	printf("\n");
}

int main(int argc, char **argv) {
	static uint8_t mem[64 << 20];
	static uint32_t start[JOB_MAX_PAGES], bytes[JOB_MAX_PAGES];
	const char *pwg = NULL, *pdf = NULL, *ps = NULL, *ipp = NULL, *probe = NULL, *jobf = NULL;
	const char *paper = NULL, *prefer = NULL, *sock = NULL;
	int dpi = 300, space = PWG_SGRAY8, i;
	uint8_t *data;
	uint32_t len;
	job_t j;
	run_t r;
	transport_t t;
	ipp_url_t u;
	clock_t t0;
	bool ok = false;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pwg") && i + 1 < argc) pwg = argv[++i];
		else if (!strcmp(argv[i], "--pdf") && i + 1 < argc) pdf = argv[++i];
		else if (!strcmp(argv[i], "--ps") && i + 1 < argc) ps = argv[++i];
		else if (!strcmp(argv[i], "--ipp") && i + 1 < argc) ipp = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock = argv[++i];
		else if (!strcmp(argv[i], "--probe") && i + 1 < argc) probe = argv[++i];
		else if (!strcmp(argv[i], "--prefer") && i + 1 < argc) prefer = argv[++i];
		else if (!strcmp(argv[i], "--dpi") && i + 1 < argc) dpi = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--paper") && i + 1 < argc) paper = argv[++i];
		else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
		else if (!strcmp(argv[i], "--srgb")) space = PWG_SRGB8;
		else jobf = argv[i];
	}
	pool_init(mem, sizeof(mem));
	{
		static uint8_t scratch[256 * 1024], cache[256 * 1024];
		zc_scratch(scratch, sizeof(scratch));
		zc_text_cache(cache, sizeof(cache));
	}
	res_init(load, 8u << 20);
	memset(&r, 0, sizeof(r));
	r.wait = host_wait;
	host_transport(&t);

	if (probe) {
		ipp_info_t info;
		if (!ipp_url(probe, &u)) { printf("print_host: not an address: %s\n", probe); return 2; }
		if (!run_probe(&r, &t, &u, &info)) { printf("print_host: %s\n", r.err); return 1; }
		show_info(&info);
		return 0;
	}
	if (!jobf) { fprintf(stderr, "print_host: no job\n"); return 2; }
	data = load(jobf, &len);
	{
		// a text job (textdoc.h), as print.c's text_job() makes one
		char tpath[ZP_PATH_MAX], err[64];
		if (data && zp_job_get(data, len, "text", tpath, sizeof(tpath)) && tpath[0] &&
			zp_pages(data, len, start, bytes, JOB_MAX_PAGES) <= 0) {
			uint32_t tlen = 0, olen;
			uint8_t *text = load(tpath, &tlen), *out;
			if (!text) { fprintf(stderr, "print_host: cannot read %s\n", tpath); return 1; }
			if (!textdoc_build(data, len, (const char *)text, tlen, paper ? paper : "",
				&out, &olen, err, sizeof(err))) {
				fprintf(stderr, "print_host: %s\n", err);
				return 1;
			}
			data = out;
			len = olen;
		}
	}
	if (!data || !job_open(&j, data, len, start, bytes)) {
		fprintf(stderr, "print_host: %s is not a print job\n", jobf);
		return 1;
	}
	if (paper) snprintf(j.paper, sizeof(j.paper), "%s", paper);
	t0 = clock();
	if (pwg || pdf || ps) {
		FILE *f = fopen(pwg ? pwg : pdf ? pdf : ps, "wb");
		out_t o = { file_write, f, 0, false };
		if (!f) return 1;
		if (pwg) ok = run_raster(&r, &j, &o, dpi, space, NULL);
		else ok = run_vec(&r, &j, &o, pdf ? VEC_PDF : VEC_PS);
		fclose(f);
	} else if (ipp || sock) {
		if (!ipp_url(ipp ? ipp : sock, &u)) { printf("print_host: not an address\n"); return 2; }
		if (sock) u.kind = URL_SOCKET;
		ok = sock ? run_socket(&r, &t, &u, &j) : run_ipp(&r, &t, &u, &j, prefer);
	} else {
		return 2;
	}
	if (r.format) printf("print_host: sent as %s%s\n", r.format, r.dpi ? (r.dpi == 300 ? " at 300 dpi" : " (not 300 dpi)") : "");
	printf("print_host: %d sheets, %u bytes, %.2fs on this machine%s%s\n", job_sheets(&j),
		r.bytes, (double)(clock() - t0) / CLOCKS_PER_SEC, ok ? "" : " -- FAILED: ", ok ? "" : r.err);
	if (r.skipped) printf("print_host: %u things not drawn\n", r.skipped);
	return ok ? 0 : 1;
}

// zimg's file reading, unused here (pictures are read whole, res.c)
int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int a, void *b, int c) { (void)a; (void)b; (void)c; return 0; }
int fs_seek(int a, uint32_t b) { (void)a; (void)b; return -1; }
int fs_close_handle(int a) { (void)a; return 0; }
