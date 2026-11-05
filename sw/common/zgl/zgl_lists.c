/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_lists.c -- GL's display lists (docs/zgl.md, "Display lists").
 *
 * A list is the calls made between glNewList() and glEndList(), kept
 * as they were made: each a small record of the operation and its
 * arguments, already in zgl's own numbers (a vertex as 24.8, a normal
 * as 16.16), so the f entry points' conversion is not repeated when the
 * list is called. glCallList() makes the calls again.
 *
 * The calls that are recorded are listed in GL/gl.h; anything else is
 * carried out at once, as if outside the list. Linked only by a
 * program that calls glNewList() (zglr_rec() is weak in zgl.c).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zgl.h"
#include "zgl_int.h"

/* List names 1..ZGLR_LISTS-1; the table grows as glGenLists() needs it
 * (a world in cells, mausrun3d, has thousands) */
#define ZGLR_LISTS 65536
#define ZGLR_DEPTH 8          /* glCallList nesting */

typedef struct {
	zglr_op_t *ops;
	uint32_t n, cap;
	bool used;               /* named by glGenLists, or made */
	/* T2's compiled form (zgl_t2.c): 0 not tried, 1 made, 2 can't be */
	uint8_t t2_state;
	void *t2;
} zglr_list_t;

static zglr_list_t *lists;
static GLuint nlists;         /* the table's size: names 1..nlists-1 */

/* room for names up to `top` - 1 */
static bool grow_to(GLuint top) {
	zglr_list_t *n;
	GLuint cap = nlists ? nlists : 256;
	if (top <= nlists) return true;
	if (top > ZGLR_LISTS) return false;
	while (cap < top) cap *= 2;
	if (cap > ZGLR_LISTS) cap = ZGLR_LISTS;
	n = realloc(lists, cap * sizeof(*n));
	if (!n) return false;
	memset(&n[nlists], 0, (cap - nlists) * sizeof(*n));
	lists = n;
	nlists = cap;
	return true;
}
static GLuint cur_list;       /* being made */
static bool cur_exec;         /* GL_COMPILE_AND_EXECUTE */
static int depth;

static void error(GLenum e) {
	if (zgl_s.error == GL_NO_ERROR) zgl_s.error = e;
}

static void free_list(zglr_list_t *l) {
	if (l->t2 && zgl_t2_free) zgl_t2_free(l->t2);
	l->t2 = NULL;
	l->t2_state = 0;
	free(l->ops);
	l->ops = NULL;
	l->n = l->cap = 0;
}

bool zglr_rec(int op, int32_t a, int32_t b, int32_t c, int32_t d) {
	zglr_list_t *l = &lists[cur_list];
	if (l->n == l->cap) {
		uint32_t cap = l->cap ? l->cap * 2 : 64;
		zglr_op_t *n = realloc(l->ops, cap * sizeof(zglr_op_t));
		if (!n) {
			/* out of memory: the list stays as it is, and the call
			 * is carried out rather than lost */
			error(GL_OUT_OF_MEMORY);
			return true;
		}
		l->ops = n;
		l->cap = cap;
	}
	l->ops[l->n].op = (uint8_t)op;
	l->ops[l->n].a = a;
	l->ops[l->n].b = b;
	l->ops[l->n].c = c;
	l->ops[l->n].d = d;
	l->n++;
	return cur_exec;
}

GLuint glGenLists(GLsizei range) {
	GLuint i, k;
	if (range <= 0) { if (range < 0) error(GL_INVALID_VALUE); return 0; }
	if (!grow_to(256)) { error(GL_OUT_OF_MEMORY); return 0; }
	for (i = 1; i + (GLuint)range <= ZGLR_LISTS; i++) {
		if (i + (GLuint)range > nlists && !grow_to(i + (GLuint)range)) {
			error(GL_OUT_OF_MEMORY);
			return 0;
		}
		for (k = 0; k < (GLuint)range; k++)
			if (lists[i + k].used) break;
		if (k == (GLuint)range) {
			for (k = 0; k < (GLuint)range; k++) lists[i + k].used = true;
			return i;
		}
	}
	return 0;
}

void glNewList(GLuint list, GLenum mode) {
	if (list == 0 || list >= ZGLR_LISTS || !grow_to(list + 1)) { error(GL_INVALID_VALUE); return; }
	if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) { error(GL_INVALID_ENUM); return; }
	if (zgl_s.listing || zgl_s.in_begin) { error(GL_INVALID_OPERATION); return; }
	free_list(&lists[list]);
	lists[list].used = true;
	cur_list = list;
	cur_exec = (mode == GL_COMPILE_AND_EXECUTE);
	zgl_s.listing = true;
}

void glEndList(void) {
	if (!zgl_s.listing) { error(GL_INVALID_OPERATION); return; }
	zgl_s.listing = false;
	cur_list = 0;
}

GLboolean glIsList(GLuint list) {
	return list > 0 && list < nlists && lists[list].used ? GL_TRUE : GL_FALSE;
}

void glDeleteLists(GLuint list, GLsizei range) {
	GLuint i;
	if (range < 0) { error(GL_INVALID_VALUE); return; }
	for (i = list; i < list + (GLuint)range && i < nlists; i++) {
		if (i == 0 || (zgl_s.listing && i == cur_list)) continue;
		free_list(&lists[i]);
		lists[i].used = false;
	}
}

ZGL_HOT void glCallList(GLuint list) {
	const zglr_list_t *l;
	bool listing = zgl_s.listing;
	uint32_t i;
	if (listing) {
		/* recorded as a call, not copied in: GL's rule, so a list
		 * changed later is called as it is then */
		if (!zglr_rec(ZGLR_CALL, (int32_t)list, 0, 0, 0)) return;
	}
	if (list == 0 || list >= nlists || depth >= ZGLR_DEPTH) return;
	l = &lists[list];
	/* T2: compiled once, and sent as it is */
	if (zgl_t2_call && zgl_t2_call(l->ops, l->n, &lists[list].t2, &lists[list].t2_state)) return;
	depth++;
	zgl_s.listing = false;          /* carrying out: nothing recorded again */
	for (i = 0; i < l->n; i++) {
		const zglr_op_t *o = &l->ops[i];
		switch (o->op) {
		case ZGLR_BEGIN:     glBegin((GLenum)o->a); break;
		case ZGLR_END:       glEnd(); break;
		case ZGLR_VERTEX:    zgl_vertex_pos(o->a, o->b, o->c); break;
		case ZGLR_NORMAL:    glNormal3x(o->a, o->b, o->c); break;
		case ZGLR_INDEX:     glIndexi(o->a); break;
		case ZGLR_MATERIAL:  zgl_material(o->a, o->b); break;
		case ZGLR_TRANSLATE: zgl_translate_pos(o->a, o->b, o->c); break;
		case ZGLR_SCALE:     glScalex(o->a, o->b, o->c); break;
		case ZGLR_ROTATE:    glRotatex(o->a, o->b, o->c, o->d); break;
		case ZGLR_PUSH:      glPushMatrix(); break;
		case ZGLR_POP:       glPopMatrix(); break;
		case ZGLR_IDENTITY:  glLoadIdentity(); break;
		case ZGLR_SHADE:     glShadeModel((GLenum)o->a); break;
		case ZGLR_ENABLE:    glEnable((GLenum)o->a); break;
		case ZGLR_DISABLE:   glDisable((GLenum)o->a); break;
		case ZGLR_CALL:      glCallList((GLuint)o->a); break;
		case ZGLR_TEXCOORD:  zgl_s.tex_s = o->a; zgl_s.tex_t = o->b; break;
		case ZGLR_BIND:      zgl_s.tex_bound = (uint32_t)o->a; break;
		default: break;
		}
	}
	zgl_s.listing = listing;
	depth--;
}
