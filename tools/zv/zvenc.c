// zvenc -- convert a video (anything ffmpeg reads) into a ZV file.
//
//   zvenc [options] INPUT OUTPUT.zv
//
// ffmpeg and ffprobe do the reading: zvenc asks them for the source's
// size, shape and frame rate, then for grey frames at the picture's
// size and for 8-bit PCM, through pipes. zvenc itself contains no
// decoder for any other format. See docs/zv.md, "zvenc".

#include "zvenc_core.h"
#include "zvcol.h"

// what game mode shows at full size (cinema: game_small())
#define Z_CHUNKY_FIT_W 320
#define Z_CHUNKY_FIT_H 240
// below this RMS spread of the cells' chroma around the film's mean, the
// film is one tint over black and white (zvcol.h, zvc_tint())
#define TINT_SPREAD 4.0
// The board the decode budget is for: a 48 MHz picorv32 with caches,
// which spends about BOARD_CPI cycles an instruction in the decoder
// (measured: Sergei ML2, docs/zv.md "A frame's cost").
#define BOARD_HZ 48e6
#define BOARD_CPI 12.0
#include "zv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
	fprintf(stderr,
		"usage: zvenc [options] INPUT OUTPUT.zv\n"
		"\n"
		"picture\n"
		"  --max WxH        fit inside this (default 640x464: a maximized window)\n"
		"  --size WxH       exactly this (multiples of 8)\n"
		"  --upscale        allow a picture larger than the source (default: not --\n"
		"                   a 320x240 source stays 320x240: a quarter of the work)\n"
		"  --start T        start at T (seconds or hh:mm:ss)\n"
		"  --duration T     only this much\n"
		"  --fps N[/D]      frame rate (default: the source's)\n"
		"  --ivtc           undo 3:2 pulldown (29.97 telecined film -> 23.976)\n"
		"  --deint          deinterlace\n"
		"  --denoise S      grain removal, 0 off (default 3)\n"
		"  --sharpen A      unsharp amount, 0 off (default 0.4)\n"
		"  --eq STR         ffmpeg eq filter, e.g. contrast=1.1:brightness=0.02\n"
		"coding\n"
		"  --rate KB        video kilobytes a second (default 80 for 640x464, less for\n"
		"                   smaller pictures: 40 for 320x240)\n"
		"  --lambda L       fixed quality instead of a rate (smaller = better)\n"
		"  --key S          a seek point every S seconds (default 2: a seek decodes\n"
		"                   up to key + refresh seconds of frames first)\n"
		"  --refresh S      the picture refreshed over S seconds after one (default 1)\n"
		"audio\n"
		"  --audio-rate R   Hz (default 22050)\n"
		"  --stereo         two channels (default one)\n"
		"  --loudness L     normalise to L LUFS before 8 bits (default -16; 0 off)\n"
		"  --no-audio\n"
		"  --budget PCT     the share of a frame's time the board may spend decoding\n"
		"                   (default 35; 0: no limit): the picture is coded coarser\n"
		"                   where it would cost more, so the film keeps its frame rate\n"
		"  --no-colour      no colour layer (a black and white film gets none anyway)\n"
		"  --colour         a colour layer even for one tint over everything (a\n"
		"                   sepia or yellowed black and white print), which is\n"
		"                   otherwise shown in greys\n"
		"  --colour-cell N  colour cells N x N pixels, 4 or 8 (default 4 for a film\n"
		"                   of 320x240 or less, 8 otherwise)\n"
		"  --threads N      default: one a processor\n"
		"  -v               a line a frame\n");
	exit(2);
}

// a shell word
static void sq(char *out, size_t cap, const char *s) {
	size_t n = 0;
	if (cap < 3) return;
	out[n++] = '\'';
	for (; *s && n + 5 < cap; s++) {
		if (*s == '\'') { memcpy(out + n, "'\\''", 4); n += 4; }
		else out[n++] = *s;
	}
	out[n++] = '\'';
	out[n] = 0;
}

static int parse_wh(const char *s, int *w, int *h) {
	return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

static int parse_rat(const char *s, uint32_t *n, uint32_t *d) {
	unsigned a = 0, b = 1;
	double f;
	if (sscanf(s, "%u/%u", &a, &b) == 2 && a && b) { *n = a; *d = b; return 1; }
	f = atof(s);
	if (f <= 0) return 0;
	if (fabs(f - 23.976) < 0.01) { *n = 24000; *d = 1001; return 1; }
	if (fabs(f - 29.97) < 0.01) { *n = 30000; *d = 1001; return 1; }
	if (fabs(f - 59.94) < 0.01) { *n = 60000; *d = 1001; return 1; }
	*n = (uint32_t)(f * 1000 + 0.5); *d = 1000;
	return 1;
}

static int round8(double v) {
	int r = (int)(v / 8.0 + 0.5) * 8;
	return r < 8 ? 8 : r;
}

int main(int argc, char **argv) {
	const char *in = NULL, *outp = NULL, *start = NULL, *dur = NULL, *eq = NULL;
	int maxw = 640, maxh = 464, W = 0, H = 0, ivtc = 0, deint = 0, verbose = 0, upscale = 0;
	int arate = 22050, achan = 1, audio = 1, threads = 0, motion_probe = 0, colour = 1, col_cell = 0;
	zvc_t *zc = NULL;
	uint8_t *yuv = NULL;
	zve_stats_t last_st;
	double loudness = -16, denoise = 3, sharpen = 0.4, rate_kb = 0, lambda = 0, key_s = 2, refresh_s = 1;
	uint32_t fn = 0, fd = 0;
	char qin[4096], cmd[8192], chain[2048], tmp[512], ss[600] = "", tt[600] = "";
	FILE *pp, *of;
	int sw = 0, sh = 0, sarn = 1, sard = 1;
	uint32_t rn = 0, rd = 0;
	uint8_t *abuf = NULL, *grey, *pay, *rec;
	size_t alen = 0, acap = 0;
	zv_header_t hd;
	zve_opts_t eo;
	zve_t *e;
	uint32_t n = 0, keyint, *kidx = NULL, kn = 0, kcap = 0, maxrec = 0;
	uint64_t vbytes = 0, abytes = 0;
	double sse_all = 0, cost_all = 0, cost_max = 0, budget = 35;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), ""))
		if (!strcmp(a, "--max")) { if (!parse_wh(NEXT(), &maxw, &maxh)) usage(); }
		else if (!strcmp(a, "--size")) { if (!parse_wh(NEXT(), &W, &H)) usage(); }
		else if (!strcmp(a, "--start")) start = NEXT();
		else if (!strcmp(a, "--duration")) dur = NEXT();
		else if (!strcmp(a, "--fps")) { if (!parse_rat(NEXT(), &fn, &fd)) usage(); }
		else if (!strcmp(a, "--ivtc")) ivtc = 1;
		else if (!strcmp(a, "--upscale")) upscale = 1;
		else if (!strcmp(a, "--deint")) deint = 1;
		else if (!strcmp(a, "--denoise")) denoise = atof(NEXT());
		else if (!strcmp(a, "--sharpen")) sharpen = atof(NEXT());
		else if (!strcmp(a, "--eq")) eq = NEXT();
		else if (!strcmp(a, "--rate")) rate_kb = atof(NEXT());
		else if (!strcmp(a, "--lambda")) lambda = atof(NEXT());
		else if (!strcmp(a, "--key")) key_s = atof(NEXT());
		else if (!strcmp(a, "--refresh")) refresh_s = atof(NEXT());
		else if (!strcmp(a, "--audio-rate")) arate = atoi(NEXT());
		else if (!strcmp(a, "--stereo")) achan = 2;
		else if (!strcmp(a, "--no-audio")) audio = 0;
		else if (!strcmp(a, "--loudness")) loudness = atof(NEXT());
		else if (!strcmp(a, "--threads")) threads = atoi(NEXT());
		else if (!strcmp(a, "--motion-probe")) motion_probe = 1;
		else if (!strcmp(a, "--no-colour") || !strcmp(a, "--no-color")) colour = 0;
		else if (!strcmp(a, "--colour") || !strcmp(a, "--color")) colour = 2;
		else if (!strcmp(a, "--budget")) budget = atof(NEXT());
		else if (!strcmp(a, "--colour-cell") || !strcmp(a, "--color-cell")) { col_cell = atoi(NEXT()); if (col_cell != 4 && col_cell != 8) usage(); }
		else if (!strcmp(a, "-v")) verbose = 1;
		else if (a[0] == '-' && a[1]) usage();
		else if (!in) in = a;
		else if (!outp) outp = a;
		else usage();
	}
	if (!in || !outp) usage();
	if (W && ((W & 7) || (H & 7) || W > ZV_MAX_W || H > ZV_MAX_H)) {
		fprintf(stderr, "zvenc: --size must be multiples of 8, at most %dx%d\n", ZV_MAX_W, ZV_MAX_H);
		return 2;
	}
	if (maxw > ZV_MAX_W) maxw = ZV_MAX_W;
	if (maxh > ZV_MAX_H) maxh = ZV_MAX_H;
	sq(qin, sizeof(qin), in);
	if (start) { sq(tmp, sizeof(tmp), start); snprintf(ss, sizeof(ss), "-ss %s", tmp); }
	if (dur) { sq(tmp, sizeof(tmp), dur); snprintf(tt, sizeof(tt), "-t %s", tmp); }

	// the source: size, pixel shape, rate
	snprintf(cmd, sizeof(cmd),
		"ffprobe -v error -select_streams v:0 -show_entries "
		"stream=width,height,sample_aspect_ratio,r_frame_rate,avg_frame_rate "
		"-of default=noprint_wrappers=1 %s", qin);
	pp = popen(cmd, "r");
	if (!pp) { perror("ffprobe"); return 1; }
	while (fgets(tmp, sizeof(tmp), pp)) {
		unsigned a, b;
		if (sscanf(tmp, "width=%d", &sw) == 1) continue;
		if (sscanf(tmp, "height=%d", &sh) == 1) continue;
		if (sscanf(tmp, "sample_aspect_ratio=%u:%u", &a, &b) == 2 && a && b) { sarn = (int)a; sard = (int)b; continue; }
		if (sscanf(tmp, "r_frame_rate=%u/%u", &a, &b) == 2 && a && b) { rn = a; rd = b; continue; }
		if (!rn && sscanf(tmp, "avg_frame_rate=%u/%u", &a, &b) == 2 && a && b) { rn = a; rd = b; continue; }
	}
	pclose(pp);
	if (!sw || !sh || !rn) { fprintf(stderr, "zvenc: %s: no video stream\n", in); return 1; }
	if (!fn) {
		fn = rn; fd = rd;
		if (ivtc) { uint64_t a = (uint64_t)fn * 4, b = (uint64_t)fd * 5;
			while (a > 0xffffffffu || b > 0xffffffffu) { a /= 2; b /= 2; }
			fn = (uint32_t)a; fd = (uint32_t)b; }
	}
	if (!W) {
		double dar = (double)sw * sarn / ((double)sh * sard);
		// no bigger than the source shows: more pixels than it has cost
		// the board four times the blocks for no detail
		if (!upscale) {
			int dw = (int)((double)sw * sarn / sard + 0.5), dh = sh;
			if (dw < maxw) maxw = dw >= 8 ? dw & ~7 : 8;
			if (dh < maxh) maxh = dh >= 8 ? dh & ~7 : 8;
		}
		W = round8(maxw > 0 ? maxw : 640);
		if (W > maxw) W -= 8;
		H = round8(W / dar);
		if (H > maxh) { H = maxh & ~7; W = round8(H * dar); if (W > maxw) W = maxw & ~7; }
	}
	if (rate_kb <= 0) rate_kb = 80.0 * sqrt((double)W * H / (640.0 * 464.0));
	fprintf(stderr, "zvenc: %s: %dx%d (pixels %d:%d), %u/%u fps -> %dx%d at %u/%u fps\n",
		in, sw, sh, sarn, sard, rn, rd, W, H, fn, fd);

	// audio first, all of it (8 bits a sample: a 90-minute film is 120 MB)
	if (audio) {
		// 8 bits keep about 48 dB: a quiet soundtrack (old films often
		// are) would be a few steps of noise. Loudness normalised first,
		// and triangular dither into 8 bits.
		char af[256];
		if (loudness < 0)
			snprintf(af, sizeof(af), "-af 'loudnorm=I=%g:TP=-1.5:LRA=11,aresample=%d:osf=u8:dither_method=triangular'",
				loudness, arate);
		else
			snprintf(af, sizeof(af), "-af 'aresample=%d:osf=u8:dither_method=triangular'", arate);
		snprintf(cmd, sizeof(cmd), "ffmpeg -nostdin -v error %s -i %s %s -vn %s -ac %d -ar %d -f u8 - 2>/dev/null",
			ss, qin, tt, af, achan, arate);
		pp = popen(cmd, "r");
		if (pp) {
			size_t r;
			for (;;) {
				if (alen + 65536 > acap) { acap = acap ? acap * 2 : 1 << 20; abuf = realloc(abuf, acap); }
				r = fread(abuf + alen, 1, 65536, pp);
				if (!r) break;
				alen += r;
			}
			pclose(pp);
		}
		if (alen == 0) { fprintf(stderr, "zvenc: no audio\n"); audio = 0; }
	}

	// grey frames
	chain[0] = 0;
	if (ivtc) strcat(chain, "fieldmatch,yadif=deint=interlaced,decimate,");
	else if (deint) strcat(chain, "yadif,");
	if (denoise > 0) {
		snprintf(tmp, sizeof(tmp), "hqdn3d=%g:%g:%g:%g,", denoise, denoise * 0.75, denoise * 1.5, denoise * 1.1);
		strcat(chain, tmp);
	}
	snprintf(tmp, sizeof(tmp), "fps=%u/%u,scale=%d:%d:flags=lanczos:out_range=full,setsar=1,", fn, fd, W, H);
	strcat(chain, tmp);
	if (sharpen > 0) { snprintf(tmp, sizeof(tmp), "unsharp=5:5:%g:5:5:0,", sharpen); strcat(chain, tmp); }
	if (eq) { snprintf(tmp, sizeof(tmp), "eq=%s,", eq); strcat(chain, tmp); }
	// grey, or with the colour layer all three planes (Y is the same grey)
	strcat(chain, colour ? "format=yuv444p" : "format=gray");
	snprintf(cmd, sizeof(cmd), "ffmpeg -nostdin -v error %s -i %s %s -an -vf '%s' -f rawvideo -pix_fmt %s -",
		ss, qin, tt, chain, colour ? "yuv444p" : "gray");
	if (verbose) fprintf(stderr, "zvenc: %s\n", cmd);
	pp = popen(cmd, "r");
	if (!pp) { perror("ffmpeg"); return 1; }

	of = fopen(outp, "wb");
	if (!of) { perror(outp); return 1; }
	memset(&hd, 0, sizeof(hd));
	hd.w = (uint16_t)W; hd.h = (uint16_t)H;
	hd.fps_num = fn; hd.fps_den = fd;
	if (audio) { hd.flags |= ZV_F_AUDIO; hd.arate = (uint32_t)arate; hd.achan = (uint8_t)achan; hd.afmt = ZV_A_PCM8; }
	{
		uint8_t hb[ZV_HDR_SIZE];
		zv_write_header(hb, &hd);
		fwrite(hb, 1, ZV_HDR_SIZE, of);
	}

	memset(&eo, 0, sizeof(eo));
	eo.w = W; eo.h = H;
	eo.lambda = lambda;
	eo.target_bytes = lambda > 0 ? 0 : rate_kb * 1024.0 * fd / fn;
	// the board's time: `budget` percent of a frame's cycles for decoding
	eo.target_cost = budget > 0 ? budget / 100.0 * BOARD_HZ * fd / fn / BOARD_CPI : 0;
	eo.verbose = verbose;
	eo.threads = threads;
	eo.motion_probe = motion_probe;
	eo.refresh = (int)(refresh_s * fn / fd + 0.5);
	if (eo.refresh < 1) eo.refresh = 1;
	hd.refresh = (uint16_t)eo.refresh;
	e = zve_new(&eo);
	grey = malloc((size_t)W * H * 3);
	yuv = grey;
	// 4x4 colour cells for a film game mode shows at full size (320x240
	// or less), where an 8x8 cell's edge is 8 pixels on the screen; 8x8
	// for the rest, where four times the bytes would cost too much
	if (!col_cell) col_cell = W <= Z_CHUNKY_FIT_W && H <= Z_CHUNKY_FIT_H ? 4 : 8;
	if (colour) zc = zvc_new(W, H, col_cell);
	pay = malloc(zve_max_payload(W, H));
	rec = malloc(zve_max_payload(W, H) + 65536 * 2 + 16);
	keyint = (uint32_t)(key_s * fn / fd + 0.5);
	if (keyint < 1) keyint = 1;
	if ((uint32_t)eo.refresh > keyint) { eo.refresh = (int)keyint; hd.refresh = (uint16_t)keyint; }

	while (fread(yuv, 1, (size_t)W * H * (colour ? 3 : 1), pp) == (size_t)W * H * (colour ? 3 : 1)) {
		zve_stats_t st;
		int key = (n % keyint) == 0;
		if (zc) zvc_frame(zc, yuv, yuv + (size_t)W * H, yuv + (size_t)W * H * 2, key);
		size_t vl = zve_frame(e, grey, key, pay, zve_max_payload(W, H), &st);
		uint32_t a0 = zv_audio_before(&hd, n), a1 = zv_audio_before(&hd, n + 1), na = audio ? a1 - a0 : 0;
		size_t ab = (size_t)na * achan, off, rl;
		long pos = ftell(of);
		if (!vl) { fprintf(stderr, "zvenc: encoder failed at frame %u\n", n); return 1; }
		if (na > 65535) { fprintf(stderr, "zvenc: too many audio samples a frame\n"); return 1; }
		memset(rec, 0, ZV_REC_HDR);
		rec[4] = ZV_REC_FRAME;
		rec[5] = key ? ZV_RF_KEY : 0;
		rec[6] = (uint8_t)na; rec[7] = (uint8_t)(na >> 8);
		off = ZV_REC_HDR;
		if (ab) {
			size_t from = (size_t)a0 * achan, k;
			for (k = 0; k < ab; k++) rec[off + k] = from + k < alen ? abuf[from + k] : 128;
			off += ab;
		}
		while (off & 3) rec[off++] = 0;
		memcpy(rec + off, pay, vl);
		rl = off + vl;
		rec[0] = (uint8_t)rl; rec[1] = (uint8_t)(rl >> 8); rec[2] = (uint8_t)(rl >> 16); rec[3] = (uint8_t)(rl >> 24);
		fwrite(rec, 1, rl, of);
		if (rl > maxrec) maxrec = (uint32_t)rl;
		if (key) {
			if (kn == kcap) { kcap = kcap ? kcap * 2 : 256; kidx = realloc(kidx, kcap * 2 * sizeof(uint32_t)); }
			kidx[kn * 2] = n; kidx[kn * 2 + 1] = (uint32_t)pos; kn++;
		}
		vbytes += vl;
		last_st = st;
		abytes += ab;
		sse_all += st.sse;
		cost_all += st.cost;
		if (st.cost > cost_max) cost_max = st.cost;
		if (verbose)
			fprintf(stderr, "%6u %c %6u B  skip %4u v1 %4u (rep %4u) v4 %4u v16 %4u  upd %3u/%3u/%3u  psnr %5.2f  lambda %.1f\n",
				n, key ? 'K' : ' ', st.bytes, st.skip, st.v1, st.rep_blocks, st.v4, st.v16, st.upd1, st.upd4, st.upd16,
				st.sse > 0 ? 10 * log10(255.0 * 255 * W * H / st.sse) : 99.0, st.lambda);
		else if (n % (fn / fd ? fn / fd : 1) == 0)
			fprintf(stderr, "\rzvenc: %u frames, %.0f s, video %.1f KB/s (target %.0f)  ", n,
				(double)n * fd / fn, n ? vbytes / 1024.0 * fn / fd / n : 0.0, rate_kb);
		n++;
	}
	pclose(pp);
	fprintf(stderr, "\n");
	if (!n) { fprintf(stderr, "zvenc: no frames\n"); return 1; }

	// the index, then the header for real
	hd.index_off = (uint32_t)ftell(of);
	hd.index_n = kn;
	for (i = 0; i < (int)kn * 2; i++) {
		uint8_t w4[4];
		uint32_t v = kidx[i];
		w4[0] = (uint8_t)v; w4[1] = (uint8_t)(v >> 8); w4[2] = (uint8_t)(v >> 16); w4[3] = (uint8_t)(v >> 24);
		fwrite(w4, 1, 4, of);
	}
	// the colour layer, after the index: a mono player never reads it.
	// A black and white film (hardly a cell not grey) gets none.
	if (zc) {
		uint8_t *cs;
		double share;
		size_t cl = zvc_finish(zc, &cs, &share);
		double tu, tv, spread = zvc_tint(zc, &tu, &tv);
		if (getenv("ZVCOL_STATS"))
			fprintf(stderr, "zvcol: chroma %.1f, %.1f, spread %.2f\n", tu, tv, spread);
		if (share < 0.01)
			fprintf(stderr, "zvenc: black and white (%.2f%% of cells coloured): no colour layer\n", share * 100);
		else if (spread < TINT_SPREAD && colour != 2)
			fprintf(stderr, "zvenc: black and white, tinted (one tint, %.0f, %.0f, over everything; "
				"spread %.1f): no colour layer (--colour keeps it)\n", tu, tv, spread);
		else {
			hd.colour_off = (uint32_t)ftell(of);
			hd.flags |= ZV_F_COLOUR;
			fwrite(cs, 1, cl, of);
			fprintf(stderr, "zvenc: colour layer: %dx%d cells, %.1f KB/s, %.0f%% of cells coloured\n",
				col_cell, col_cell, cl / 1024.0 * fn / fd / n, share * 100);
		}
		free(cs);
		zvc_free(zc);
	}
	hd.frames = n;
	hd.max_rec = maxrec;
	hd.video_bytes = (uint32_t)(vbytes > 0xffffffffu ? 0xffffffffu : vbytes);
	hd.audio_bytes = (uint32_t)(abytes > 0xffffffffu ? 0xffffffffu : abytes);
	{
		uint8_t hb[ZV_HDR_SIZE];
		long total = ftell(of);
		double secs = (double)n * fd / fn;
		zv_write_header(hb, &hd);
		fseek(of, 0, SEEK_SET);
		fwrite(hb, 1, ZV_HDR_SIZE, of);
		fclose(of);
		fprintf(stderr,
			"zvenc: %s: %u frames, %.1f s, %dx%d\n"
			"       video %.1f KB/s, audio %.1f KB/s, file %.1f KB/s (%.0f MB for 90 minutes)\n"
			"       psnr %.2f dB, largest record %u bytes, %u seek points\n",
			outp, n, secs, W, H, vbytes / 1024.0 / secs, abytes / 1024.0 / secs, total / 1024.0 / secs,
			total / secs * 5400 / 1e6,
			sse_all > 0 ? 10 * log10(255.0 * 255 * W * H * n / sse_all) : 99.0, maxrec, kn);
		if (n)
			fprintf(stderr, "       decoding on the board: about %.0f%% of a frame's time (the worst frame %.0f%%)\n",
				100.0 * cost_all / n * BOARD_CPI / (BOARD_HZ * fd / fn),
				100.0 * cost_max * BOARD_CPI / (BOARD_HZ * fd / fn));
	}
	if (motion_probe)
		fprintf(stderr, "zvenc: motion probe: %u blocks would be copies, saving about %.0f KB (%.1f%% of the video)\n",
			last_st.probe_blocks, last_st.probe_bits / 8 / 1024, 100.0 * last_st.probe_bits / 8 / (double)vbytes);
	zve_free(e);
	return 0;
}
