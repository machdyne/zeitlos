/*
 * Host test for zgfx.c's visible-region logic.
 *
 *   cc -std=gnu99 -o /tmp/t sw/common/tests/test_gfx_region.c && /tmp/t
 *
 * Runs on the build machine, not the target: the logic under test is
 * pure integer arithmetic over rectangles, and it is far easier to be
 * confident in it here than by looking at a screen.
 *
 * The functions below are LIFTED VERBATIM from sw/common/zgfx.c rather
 * than #included, because that file pulls in the whole MMIO register
 * map. If you change the originals, change these -- the test is worth
 * more than the duplication costs, and the third case below is the
 * reason.
 *
 * That case: a BOUNDING BOX around an L-shaped visible region is a
 * superset of it, so it permits drawing on exactly the pixels the
 * region exists to protect. The test asserts that a bbox gets it
 * wrong, which is what documents why the region is a set of
 * rectangles and not a single one.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#define Z_SCREEN_W 640
#define Z_SCREEN_H 480
typedef struct { int x0,y0,x1,y1; } z_clip_t;

// --- the code under test, lifted verbatim ---
#define Z_GFX_MAX_CLIP 8
static z_clip_t gfx_region[Z_GFX_MAX_CLIP];
static int gfx_region_n;
void z_gfx_set_visible(const z_clip_t *rects,int n){
  if(n>Z_GFX_MAX_CLIP)n=Z_GFX_MAX_CLIP; if(n<0)n=0;
  for(int i=0;i<n;i++)gfx_region[i]=rects[i]; gfx_region_n=n; }
void z_gfx_clear_visible(void){ gfx_region_n=0; }
int z_gfx_visible_count(void){ return gfx_region_n; }
bool z_gfx_visible_clip(int i,const z_clip_t *clip,z_clip_t *out){
  z_clip_t r;
  if(gfx_region_n==0){ r.x0=0;r.y0=0;r.x1=Z_SCREEN_W-1;r.y1=Z_SCREEN_H-1; }
  else { if(i<0||i>=gfx_region_n) return false; r=gfx_region[i]; }
  if(clip){ if(clip->x0>r.x0)r.x0=clip->x0; if(clip->y0>r.y0)r.y0=clip->y0;
            if(clip->x1<r.x1)r.x1=clip->x1; if(clip->y1<r.y1)r.y1=clip->y1; }
  if(r.x1<r.x0||r.y1<r.y0) return false;
  *out=r; return true; }
static inline bool region_allows(int x,int y){
  if(gfx_region_n==0) return true;
  for(int i=0;i<gfx_region_n;i++){ const z_clip_t*r=&gfx_region[i];
    if(x>=r->x0&&x<=r->x1&&y>=r->y0&&y<=r->y1) return true; }
  return false; }
static inline bool clip_allows(int x,int y,const z_clip_t*clip){
  if(x<0||x>=Z_SCREEN_W||y<0||y>=Z_SCREEN_H) return false;
  if(clip){ if(x<clip->x0||x>clip->x1||y<clip->y0||y>clip->y1) return false; }
  return region_allows(x,y); }
// --- end ---

static int fails;
static void ck(const char*n,int got,int want){
  if(got!=want){ printf("  FAIL %s: got %d want %d\n",n,got,want); fails++; }
  else printf("  ok   %s\n",n); }

int main(void){
  z_clip_t win = {100,100,299,299};   // a 200x200 window

  printf("== no region set means unrestricted ==\n");
  z_gfx_clear_visible();
  ck("count is 0", z_gfx_visible_count(), 0);
  ck("centre draws", clip_allows(200,200,&win), 1);
  ck("outside the clip still blocked", clip_allows(50,50,&win), 0);
  z_clip_t e;
  ck("pass 0 exists", z_gfx_visible_clip(0,&win,&e), 1);
  ck("  and equals the clip", e.x0==100&&e.y0==100&&e.x1==299&&e.y1==299, 1);

  printf("\n== an L-shape: window covered in its bottom-right ==\n");
  // Occluder covers x>=200,y>=200. Visible = top strip + left strip.
  z_clip_t L[2] = { {100,100,299,199}, {100,200,199,299} };
  z_gfx_set_visible(L,2);
  ck("top strip draws",        clip_allows(250,150,&win), 1);
  ck("left strip draws",       clip_allows(150,250,&win), 1);
  ck("OCCLUDED corner blocked",clip_allows(250,250,&win), 0);
  ck("count is 2",             z_gfx_visible_count(), 2);

  printf("\n== a bounding box would NOT be safe ==\n");
  // The bbox of the L is the whole window -- it would allow the
  // occluded corner. This is why the region is a set, not a box.
  z_clip_t bbox[1] = { {100,100,299,299} };
  z_gfx_set_visible(bbox,1);
  ck("bbox wrongly allows the corner", clip_allows(250,250,&win), 1);

  printf("\n== fully occluded is one EMPTY rect, not zero ==\n");
  z_clip_t none[1] = { {0,0,-1,-1} };
  z_gfx_set_visible(none,1);
  ck("nothing draws",          clip_allows(200,200,&win), 0);
  ck("pass 0 is empty",        z_gfx_visible_clip(0,&win,&e), 0);
  ck("but count is 1 not 0",   z_gfx_visible_count(), 1);

  printf("\n== per-pass clips intersect correctly ==\n");
  z_gfx_set_visible(L,2);
  z_gfx_visible_clip(0,&win,&e);
  ck("pass 0 is the top strip", e.x0==100&&e.y0==100&&e.x1==299&&e.y1==199, 1);
  z_gfx_visible_clip(1,&win,&e);
  ck("pass 1 is the left strip",e.x0==100&&e.y0==200&&e.x1==199&&e.y1==299, 1);
  z_clip_t small = {150,150,160,160};
  ck("a clip inside pass 0",    z_gfx_visible_clip(0,&small,&e), 1);
  ck("  intersects to itself",  e.x0==150&&e.x1==160&&e.y0==150&&e.y1==160, 1);
  ck("same clip misses pass 1", z_gfx_visible_clip(1,&small,&e), 0);

  printf("\n%s\n", fails?"FAIL":"PASS -- region logic correct");
  return fails?1:0; }
