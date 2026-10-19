/*
 * slide's model on the host: the pool, the deck, the .zsl reader and
 * writer. `make -C sw/apps/slide test` (ASan/UBSan).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../deck.h"
#include "../deckio.h"
#include "../printjob.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static uint8_t mem[4 << 20];

static deck_t *sample(void) {
	deck_t *d = deck_new();
	card_t *bg = card_new("plain"), *c2;
	obj_t *o;
	zpage_set(&d->page, "letter", true);
	strcpy(d->title, "A \"quoted\" title");
	o = obj_new(OB_RECT);
	strcpy(o->name, "bar");
	o->fill = FILL_SHADE;
	o->fill_v = 4;
	o->line_w = 0;
	card_add(bg, -1, o);
	deck_insert_bg(d, -1, bg);
	d->card[0]->bg = 0;
	strcpy(d->card[0]->name, "intro");
	d->card[0]->trans = TR_WIPE;
	d->card[0]->trans_ms = 300;
	obj_set_str(&d->card[0]->notes, "Say hello.\nThen wave.");

	o = obj_new(OB_TEXT);
	strcpy(o->name, "title");
	obj_set_box(o, 20, 40, 470, 60);
	o->font = (uint8_t)deck_font_index(d, "serif");
	o->size = 36;
	o->style = ST_BOLD | ST_ITALIC;
	o->align = AL_CENTER;
	o->valign = VA_MIDDLE;
	{
		static const char *t = "Gr\xc3\xbc\xc3\x9f" "e, \\ \"Zeitlos\"\n\tsecond line";
		obj_set_text(o, t, (uint32_t)strlen(t));
	}
	card_add(d->card[0], -1, o);

	o = obj_new(OB_POLY);
	strcpy(o->name, "tri");
	o->closed = 1;
	o->npts = 3;
	o->pts = pool_alloc(12);
	o->pts[0] = 10; o->pts[1] = 10; o->pts[2] = 60; o->pts[3] = 10; o->pts[4] = 35; o->pts[5] = -40;
	o->x = 10; o->y = -40; o->w = 50; o->h = 50;	// the points' own box
	o->fill = FILL_PATTERN;
	o->fill_v = 13;
	card_add(d->card[0], -1, o);

	o = obj_new(OB_LINE);
	strcpy(o->name, "arrow");
	obj_set_box(o, 300, 200, -100, 50);
	o->arrows = AR_END;
	o->line_w = 3;
	o->line_tone = 8;
	o->flags = OBF_LOCKED;
	obj_set_str(&o->action, "(go 'next)");
	card_add(d->card[0], -1, o);

	c2 = card_new("second");
	c2->advance_ms = 5000;
	c2->trans = TR_PUSH;
	o = obj_new(OB_IMAGE);
	strcpy(o->name, "logo");
	obj_set_str(&o->src, "img/logo.png");
	o->contrast = -20;
	o->bright = 10;
	card_add(c2, -1, o);
	o = obj_new(OB_RRECT);
	strcpy(o->name, "box");
	o->radius = 7;
	o->flags = OBF_HIDDEN;
	card_add(c2, -1, o);
	deck_insert_card(d, -1, c2);
	return d;
}

int main(void) {
	pool_init(mem, sizeof(mem));
	size_t free0 = pool_free_bytes();

	// -- the pool --
	{
		void *a = pool_alloc(100), *b = pool_alloc(200), *c = pool_alloc(300);
		void *p[500];
		int i;
		ck(a && b && c && pool_check(), "three blocks");
		ck(((uintptr_t)a & 7) == 0, "8-byte aligned");
		pool_free(b);
		ck(pool_check(), "free in the middle");
		b = pool_alloc(150);
		ck(b && pool_check(), "reused");
		a = pool_realloc(a, 5000);
		ck(a && pool_check(), "realloc grows");
		pool_free(a); pool_free(b); pool_free(c);
		ck(pool_check() && pool_free_bytes() == free0, "everything merges back");
		srand(3);
		memset(p, 0, sizeof(p));
		for (i = 0; i < 20000; i++) {
			int k = rand() % 500;
			if (p[k]) { pool_free(p[k]); p[k] = NULL; }
			else p[k] = pool_alloc((size_t)(rand() % 3000));
			if (i % 997 == 0 && !pool_check()) break;
		}
		ck(pool_check(), "20000 random allocs and frees keep the tags straight");
		for (i = 0; i < 500; i++) pool_free(p[i]);
		ck(pool_free_bytes() == free0 && pool_largest() + 12 >= free0 - 8,
			"and it all comes back as one block");
		ck(pool_alloc(sizeof(mem)) == NULL, "too big is NULL");
	}

	// -- round trip --
	{
		deck_t *d = sample(), *e;
		sbuf_t a, b;
		char err[80];
		sb_init(&a);
		sb_init(&b);
		ck(deckio_write(&a, d), "write");
		e = deckio_read(a.buf, a.len, err, sizeof(err));
		ck(e != NULL, "read back");
		if (!e) printf("  %s\n", err);
		if (e) {
			ck(deckio_write(&b, e) && b.len == a.len && !memcmp(a.buf, b.buf, a.len),
				"written again, byte for byte the same");
			ck(e->ncard == 2 && e->nbg == 1 && e->card[0]->bg == 0, "cards and background");
			ck(!strcmp(e->title, d->title), "title with quotes");
			ck(e->page.gw == 512 && e->page.gh == 396 && !strcmp(e->page.size.name, "letter"),
				"page");
			{
				obj_t *t = e->card[0]->obj[0];
				ck(t->kind == OB_TEXT && !strcmp(t->text, d->card[0]->obj[0]->text),
					"text with UTF-8, escapes, newlines and tabs");
				ck(!strcmp(e->font[t->font], "serif") && t->size == 36 &&
					t->style == (ST_BOLD | ST_ITALIC), "font, size, style");
				ck(t->align == AL_CENTER && t->valign == VA_MIDDLE, "alignment");
			}
			{
				obj_t *p = e->card[0]->obj[1];
				ck(p->npts == 3 && p->pts[5] == -40 && p->closed && p->fill == FILL_PATTERN &&
					p->fill_v == 13, "polygon");
				obj_t *l = e->card[0]->obj[2];
				ck(l->w == -100 && l->arrows == AR_END && l->line_tone == 8 &&
					(l->flags & OBF_LOCKED) && !strcmp(l->action, "(go 'next)"), "line");
			}
			ck(e->card[1]->advance_ms == 5000 && e->card[1]->trans == TR_PUSH, "timing");
			ck(!strcmp(e->card[1]->obj[0]->src, "img/logo.png") &&
				e->card[1]->obj[0]->contrast == -20, "image");
			ck(!strcmp(e->card[0]->notes, "Say hello.\nThen wave."), "notes");
			deck_free(e);
		}
		// a card on its own (undo) and objects (clipboard)
		{
			sbuf_t c;
			card_t *cc;
			int n;
			sb_init(&c);
			deckio_write_card(&c, d, d->card[0], false);
			cc = deckio_read_card(d, c.buf, c.len);
			ck(cc && cc->nobj == 3 && cc->bg == 0 && !strcmp(cc->name, "intro"), "a card alone");
			card_free(cc);
			sb_free(&c);
			sb_init(&c);
			deckio_write_objs(&c, d, d->card[0]->obj, 2);
			n = deckio_read_objs(d, d->card[1], c.buf, c.len);
			ck(n == 2 && d->card[1]->nobj == 4, "objects through the clipboard");
			ck(deckio_read_objs(d, d->card[1], "hello", 5) == -1, "plain text is not objects");
			sb_free(&c);
		}
		// unknown forms are skipped, newer versions refused
		{
			static const char *t =
				"(deck (version 1) (future (nested (things \"x\")) 3)"
				" (card \"a\" (sparkle 1 2) (rect \"r\" (box 1 2 3 4) (glow 5)) (wobble))"
				" (card \"b\"))";
			deck_t *f = deckio_read(t, (uint32_t)strlen(t), err, sizeof(err));
			ck(f && f->ncard == 2 && f->card[0]->nobj == 1 && f->card[0]->obj[0]->w == 3,
				"forms from a later slide are skipped");
			deck_free(f);
			f = deckio_read("(deck (version 2))", 18, err, sizeof(err));
			ck(!f && strstr(err, "newer"), "a newer version is refused, and says why");
			f = deckio_read("(deck (card \"a\"", 15, err, sizeof(err));
			ck(!f && strstr(err, "line 1"), "an unclosed deck is refused with a line");
			f = deckio_read("(deck)", 6, err, sizeof(err));
			ck(f && f->ncard == 1, "an empty deck gets one card");
			deck_free(f);
		}
		// damaged files: every truncation and many corruptions, none crash
		{
			int i, ok = 0;
			char *c = malloc(a.len);
			for (i = 0; i < (int)a.len; i += 3) {
				deck_t *f = deckio_read(a.buf, (uint32_t)i, err, sizeof(err));
				if (f) { ok++; deck_free(f); }
			}
			for (i = 0; i < 3000; i++) {
				deck_t *f;
				int k;
				memcpy(c, a.buf, a.len);
				for (k = 0; k < 1 + i % 4; k++)
					c[rand() % (int)a.len] = "()\" \\x9-\n;"[rand() % 10];
				f = deckio_read(c, a.len, err, sizeof(err));
				if (f) { ok++; deck_free(f); }
			}
			free(c);
			ck(pool_check(), "after thousands of damaged files the pool is intact");
			(void)ok;
		}
		sb_free(&a);
		sb_free(&b);
		deck_free(d);
	}

	// -- model operations --
	{
		deck_t *d = deck_new();
		char nm[32];
		card_t *c;
		obj_t *o;
		int i;
		for (i = 0; i < 5; i++) {
			deck_unique_card_name(d, nm, sizeof(nm));
			deck_insert_card(d, -1, card_new(nm));
		}
		ck(d->ncard == 6 && deck_find_card(d, "card 6") == 5, "unique card names");
		deck_move_card(d, 5, 0);
		ck(!strcmp(d->card[0]->name, "card 6") && !strcmp(d->card[1]->name, "card 1"), "move a card up");
		deck_move_card(d, 0, 5);
		ck(!strcmp(d->card[5]->name, "card 6"), "and back down");
		c = d->card[0];
		for (i = 0; i < 3; i++) {
			o = obj_new(OB_OVAL);
			card_unique_obj_name(c, OB_OVAL, o->name, OB_NAME_MAX);
			card_add(c, -1, o);
		}
		ck(!strcmp(c->obj[2]->name, "oval 3"), "unique object names");
		o = card_take(c, 0);
		card_add(c, -1, o);
		ck(c->obj[2] == o && !strcmp(o->name, "oval 1"), "to the front");
		{
			card_t *dup = card_dup(c);
			ck(dup && dup->nobj == 3 && dup->obj[0] != c->obj[0] &&
				!strcmp(dup->obj[0]->name, c->obj[0]->name), "a duplicated card owns new objects");
			card_free(dup);
		}
		deck_free(d);
	}

	ck(pool_check() && pool_free_bytes() == free0, "no leaks: the pool is as it started");
	// -- printing: the /print words, and the deck keeping its settings --
	{
		pj_settings_t ps;
		char keep[64], err[64];
		ck(printjob_parse(&ps, "4up cut", "/print pdf 2-3 paper a4 copies 2", 0, 7, keep, sizeof(keep), err, sizeof(err)),
			"print words parse");
		ck(ps.nup == 4 && ps.cut && ps.dest == PJ_PDF && ps.first == 1 && ps.count == 2 &&
			!strcmp(ps.paper, "a4") && ps.copies == 2, "settings and one-off words");
		ck(!strcmp(keep, "4up cut paper a4"), "kept: settings, not copies, cards or pdf");
		ck(printjob_parse(&ps, keep, "print this nocut", 5, 7, keep, sizeof(keep), err, sizeof(err)) &&
			ps.first == 5 && ps.count == 1 && !ps.cut && !strcmp(keep, "4up paper a4"), "this card, and a setting undone");
		ck(!printjob_parse(&ps, "", "print 9", 0, 7, NULL, 0, err, sizeof(err)) && err[0], "no card 9");
		ck(!printjob_parse(&ps, "", "print paper tabloid", 0, 7, NULL, 0, err, sizeof(err)), "unknown paper");
		ck(!printjob_parse(&ps, "", "print 17up", 0, 7, NULL, 0, err, sizeof(err)), "17up");
		ck(!printjob_parse(&ps, "pdf", NULL, 0, 7, NULL, 0, err, sizeof(err)), "a deck does not keep pdf");
	}
	{
		static const char t[] = "(deck (version 1) (page \"classic\" landscape) (print \"2up cut\") (card \"a\"))";
		char err[80];
		deck_t *d = deckio_read(t, sizeof(t) - 1, err, sizeof(err));
		sbuf_t b;
		ck(d && !strcmp(d->print, "2up cut"), "(print ...) read");
		sb_init(&b);
		ck(d && deckio_write(&b, d) && strstr(b.buf, "(print \"2up cut\")"), "(print ...) written");
		sb_free(&b);
		if (d) deck_free(d);
	}

	printf("deck: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}

// zimg's file reading, linked with the renderer (printjob.c); unused here
