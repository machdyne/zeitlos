/*
 * Render sw/apps/ask's panel to an image, on the build machine.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/askrender \
 *      sw/apps/ask/tests/render.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c
 *   /tmp/askrender /tmp/ask.pbm [win_w win_h]
 *
 * Needs -no-pie and vm.mmap_min_addr=0 -- see
 * sw/common/tests/ztramp.h.
 *
 * See sw/common/tests/zrender.h for what this does and cannot catch.
 * ask.c, zwin.c and zwidget.c are the REAL sources; only the pixel
 * plotting is software.
 *
 * -- WHY THIS EXISTS, WHICH IS NOT HYPOTHETICAL --
 *
 * The pack buttons shipped with their labels outside their boxes, and
 * the cause is the third bug zrender.h's own header lists:
 * z_win_hw_box() and z_win_hw_line() take ABSOLUTE SCREEN COORDINATES
 * while z_win_fill_rect() and z_win_draw_text() are window-relative.
 * So the boxes were drawn offset by the window's position on screen
 * and the text was not, and the two separated by exactly however far
 * down the screen the window happened to be.
 *
 * No arithmetic assertion I would have thought to write catches that.
 * Looking at it catches it immediately, which is the whole argument in
 * zrender.h.
 *
 * The synthetic state below matters as much as the harness. An empty
 * result list would hide most of what is worth looking at: a title
 * running past the window edge, a score colliding with the heading it
 * shares a line with, an inverted selection that swallows the row
 * above it, buttons too narrow for their labels. So there are results
 * of several widths, a selected row, a long untruncated title, and
 * more packs than fit comfortably.
 */

#include "../../../common/tests/zrender.h"
#include "../../../common/tests/ztramp.h"

/* ask.c owns main(); this file needs its own. */
#define main ask_main_unused
#include "../ask.c"
#undef main

/* -- everything that would need a filesystem, an index or a message --
 *
 * Do-nothing on purpose: this renders geometry, and anything that
 * takes a round trip to the card or to wm is not geometry.
 */

static ai_pack_t fake_packs[3];
static int fake_npacks;

int ai_pack_count(void) { return fake_npacks; }
const ai_pack_t *ai_pack(int i)
{ return (i >= 0 && i < fake_npacks) ? &fake_packs[i] : NULL; }
uint32_t ai_resident(void) { return 1540000; }
int ai_failed_count(void) { return 0; }
const char *ai_error(void) { return "no packs found"; }
int ai_load_all(const char *r, void (*p)(uint32_t, uint32_t, const char *))
{ (void)r; (void)p; return fake_npacks; }
void ai_free_all(void) {}
bool ai_query_begin(ai_query_t *q, const char *t, int w)
{ (void)q; (void)t; (void)w; return false; }
bool ai_query_step(ai_query_t *q) { (void)q; return true; }
void ai_query_cancel(ai_query_t *q) { (void)q; }
int ai_preview(const ai_hit_t *h, char *b, int n)
{ (void)h; (void)b; (void)n; return 0; }
int ai_browse(const ai_pack_t *p, uint32_t f, int m,
              char t[][AI_TITLE_MAX], char pa[][AI_PATH_MAX])
{ (void)p; (void)f; (void)m; (void)t; (void)pa; return 0; }
int ai_selftest(const char *p) { (void)p; return 0; }
bool ai_launch_arg(const ai_hit_t *h, char *b, int n)
{ (void)h; (void)b; (void)n; return false; }

int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int h, void *b, int n) { (void)h; (void)b; (void)n; return 0; }
int fs_seek(int h, uint32_t o) { (void)h; (void)o; return 0; }
int fs_close_handle(int h) { (void)h; return 0; }
int fs_size(char *p) { (void)p; return 0; }

static void set_hit(int i, const char *pack, const char *title,
                    const char *head, int score)
{
    ai_hit_t *h = &Q.hits[i];
    memset(h, 0, sizeof(*h));
    h->pack = &fake_packs[0];
    scat(h->title, AI_TITLE_MAX, 0, title);
    scat(h->head, AI_HEAD_MAX, 0, head);
    scat(h->path, AI_PATH_MAX, 0, "/ark/arklite/books/00000072.md");
    h->off = 39006;
    h->len = 1848;
    h->score = score;
    (void)pack;
}

int main(int argc, char **argv)
{
    int w = (argc > 2) ? atoi(argv[2]) : WIN_W;
    int h = (argc > 3) ? atoi(argv[3]) : WIN_H;

    scat(fake_packs[0].name, sizeof(fake_packs[0].name), 0, "arklite");
    scat(fake_packs[1].name, sizeof(fake_packs[1].name), 0, "zdocs");
    scat(fake_packs[2].name, sizeof(fake_packs[2].name), 0, "arkmed");
    fake_packs[0].nchunks = 9872;
    fake_packs[1].nchunks = 1773;
    fake_packs[2].nchunks = 12641;
    fake_npacks = 3;

    if (!z_render_open(&win, w, h)) return 77;

    loaded = true;
    qlen = scat(query, sizeof(query), 0, "how do I treat a snake bite");
    query_dirty = false;
    sel = 1;

    set_hit(0, "arklite", "FM 21-11 FIRST AID -- CHAPTER 5",
            "Chapter 5 > Bites and Stings", 882);
    set_hit(1, "arklite", "FM 21-77 SURVIVAL -- APPENDIX C",
            "Appendix C > Poisonous Snakes of the World", 771);
    set_hit(2, "arklite", "The Complete Herbal", "", 640);
    set_hit(3, "arklite",
            "A deliberately very long document title that must truncate",
            "and a heading that is also far too long to fit", 512);
    set_hit(4, "zdocs", "Medicine", "Medicine > Trauma", 430);
    Q.nhits = 5;

    status_idle();
    draw_all();

    z_render_write((argc > 1) ? argv[1] : "/tmp/ask.pbm", &win, 2);
    return 0;
}
