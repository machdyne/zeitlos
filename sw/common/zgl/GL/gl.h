/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * GL/gl.h -- zgl, a GL 1.0-style 3D API (docs/zgl.md).
 *
 * The subset of GL 1.0 that zgl provides, under GL's own names and
 * values, so that small GL 1.0 programs port. Written for Zeitlos from
 * the GL 1.0 specification's names and values; it is not OpenGL (a
 * Khronos trademark) and claims no conformance.
 *
 * Numbers: fixed point inside. The `x` entry points take 16.16
 * (GLfixed, as in OpenGL ES 1.0 Common-Lite) and the `i` ones
 * integers, with no conversion cost; the `f` and `d` ones convert on
 * entry, and pull in soft-float only for a program that calls them.
 *
 * The window-system part (what GLX or WGL is elsewhere) is zgl.h.
 */

#ifndef ZGL_GL_H
#define ZGL_GL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;
typedef void           GLvoid;
typedef int32_t        GLfixed;     /* 16.16 */
typedef int32_t        GLclampx;

#define GL_FALSE                0
#define GL_TRUE                 1

/* primitives */
#define GL_POINTS               0x0000
#define GL_LINES                0x0001
#define GL_LINE_LOOP            0x0002
#define GL_LINE_STRIP           0x0003
#define GL_TRIANGLES            0x0004
#define GL_TRIANGLE_STRIP       0x0005
#define GL_TRIANGLE_FAN         0x0006
#define GL_QUADS                0x0007
#define GL_QUAD_STRIP           0x0008
#define GL_POLYGON              0x0009

/* errors */
#define GL_NO_ERROR             0
#define GL_INVALID_ENUM         0x0500
#define GL_INVALID_VALUE        0x0501
#define GL_INVALID_OPERATION    0x0502
#define GL_STACK_OVERFLOW       0x0503
#define GL_STACK_UNDERFLOW      0x0504
#define GL_OUT_OF_MEMORY        0x0505

/* faces, polygon mode */
#define GL_FRONT                0x0404
#define GL_BACK                 0x0405
#define GL_FRONT_AND_BACK       0x0408
#define GL_CW                   0x0900
#define GL_CCW                  0x0901
#define GL_POINT                0x1B00
#define GL_LINE                 0x1B01
#define GL_FILL                 0x1B02

/* capabilities */
#define GL_CULL_FACE            0x0B44
#define GL_LIGHTING             0x0B50
#define GL_DEPTH_TEST           0x0B71
#define GL_NORMALIZE            0x0BA1
#define GL_TEXTURE_2D           0x0DE1
#define GL_SCISSOR_TEST         0x0C11
#define GL_LIGHT0               0x4000
#define GL_LIGHT1               0x4001

/* shading */
#define GL_FLAT                 0x1D00
#define GL_SMOOTH               0x1D01

/* matrices */
#define GL_MODELVIEW            0x1700
#define GL_PROJECTION           0x1701

/* clear */
#define GL_DEPTH_BUFFER_BIT     0x00000100
#define GL_COLOR_BUFFER_BIT     0x00004000

/* lights and materials */
#define GL_AMBIENT              0x1200
#define GL_DIFFUSE              0x1201
#define GL_SPECULAR             0x1202
#define GL_POSITION             0x1203
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION   0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_AMBIENT_AND_DIFFUSE  0x1602
#define GL_COLOR_INDEXES        0x1603

/* display lists */
#define GL_COMPILE              0x1300
#define GL_COMPILE_AND_EXECUTE  0x1301

/* queries */
#define GL_VENDOR               0x1F00
#define GL_RENDERER             0x1F01
#define GL_VERSION              0x1F02
#define GL_EXTENSIONS           0x1F03

/* -- vertices ---------------------------------------------------------- */

void glBegin(GLenum mode);
void glEnd(void);

void glVertex2i(GLint x, GLint y);
void glVertex3i(GLint x, GLint y, GLint z);
void glVertex2x(GLfixed x, GLfixed y);
void glVertex3x(GLfixed x, GLfixed y, GLfixed z);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat *v);
void glVertex3iv(const GLint *v);

/* colour index mode (the palette's index), and RGB, which zgl maps to
 * the nearest index of the palette it was given (zgl.h) */
void glIndexi(GLint c);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor3x(GLfixed r, GLfixed g, GLfixed b);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);

void glNormal3x(GLfixed nx, GLfixed ny, GLfixed nz);
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
void glNormal3i(GLint nx, GLint ny, GLint nz);   /* any length: GL_NORMALIZE */

/* -- lighting (colour index mode, as GL 1.0 lights an index) ---------- *
 * A lit vertex's index is the material's ambient index plus its
 * diffuse brightness (0..1) times the way to the diffuse index:
 * glMaterial(GL_COLOR_INDEXES) gives the two ends of a ramp (zglRamp()).
 * A light is directional (GL_POSITION's w 0) or positional (w not 0:
 * where it is, through the modelview of the moment, each vertex lit from
 * there), with GL's attenuation 1 / (kc + kl d + kq d^2) for distance d
 * in eye units (GL_CONSTANT/LINEAR/QUADRATIC_ATTENUATION; 1, 0, 0).
 * GL_DIFFUSE's brightness is the luminance of its colour. No specular yet. */
void glLightxv(GLenum light, GLenum pname, const GLfixed *params);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightx(GLenum light, GLenum pname, GLfixed param);
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glMaterialiv(GLenum face, GLenum pname, const GLint *params);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);

/* -- matrices ------------------------------------------------------------ */

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glPushMatrix(void);
void glPopMatrix(void);
void glLoadMatrixx(const GLfixed *m);       /* column-major, as GL */
void glMultMatrixx(const GLfixed *m);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);

void glTranslatex(GLfixed x, GLfixed y, GLfixed z);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glScalex(GLfixed x, GLfixed y, GLfixed z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glRotatex(GLfixed degrees, GLfixed x, GLfixed y, GLfixed z);
void glRotatef(GLfloat degrees, GLfloat x, GLfloat y, GLfloat z);

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f);

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);

/* -- textures (docs/zgl.md, "Textures") ------------------------------------ *
 * On T2 only, in a display list: glEnable(GL_TEXTURE_2D) at the time a
 * list is called draws its textured triangles with their textures; the
 * software and immediate mode draw them in their colours. A texture is
 * made with zglTexImage() (zgl.h); its name is bound while the list is
 * made. Coordinates in texels. */
void glTexCoord2i(GLint s, GLint t);
void glTexCoord2x(GLfixed s, GLfixed t);
void glBindTexture(GLenum target, GLuint name);

/* -- display lists (zgl_lists.c; docs/zgl.md, "Display lists") ----------- *
 * Recorded: glBegin, glEnd, glVertex*, glNormal*, glIndexi, glColor*,
 * glMaterial*, glTranslate*, glRotate*, glScale*, glPushMatrix,
 * glPopMatrix, glLoadIdentity, glShadeModel, glEnable, glDisable,
 * glCallList, glTexCoord*, glBindTexture. Anything else is carried out at once, also while a list
 * is being made. */
GLuint glGenLists(GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glDeleteLists(GLuint list, GLsizei range);
GLboolean glIsList(GLuint list);

/* -- state -------------------------------------------------------------- */

void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void glShadeModel(GLenum mode);
void glPolygonMode(GLenum face, GLenum mode);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glPointSize(GLfloat size);             /* 1 only */
void glLineWidth(GLfloat width);            /* 1 only */

void glClearIndex(GLfloat c);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClear(GLbitfield mask);
void glFlush(void);
void glFinish(void);

GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);

#ifdef __cplusplus
}
#endif

#endif
