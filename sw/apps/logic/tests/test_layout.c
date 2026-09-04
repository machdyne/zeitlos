/*
 * Geometry check for sw/apps/logic/logic.c's panel layout.
 *
 *   cc -std=gnu99 -Wall -o /tmp/t sw/apps/logic/tests/test_layout.c && /tmp/t
 *
 * Compiles the REAL logic.c and calls the REAL layout(), then asserts
 * what a screen would otherwise have to tell you: that no widget sits
 * outside the window, no two widgets overlap, nothing lands on the
 * readout well or the waveform screen, the readout is tall enough for
 * its two lines, and the channel strip stays row-aligned with the
 * traces beside it.
 *
 * Hand-placed controls are the fragile part of an instrument panel.
 * The layout is cursor-driven precisely so a wrong number moves a
 * whole group instead of putting a button on top of a display, and
 * this is what confirms it still does.
 *
 * Everything the app draws with or talks to is stubbed below --
 * deliberately do-nothing, because this checks ARITHMETIC, not pixels.
 * maskirq() (sw/common/zeitlos.h) has a non-RISC-V branch, so the app
 * compiles here unmodified -- see the note there.
 */
/* Geometry check for sw/apps/logic/logic.c's layout().
 *
 * Compiled against a SHIM copy of sw/common in which maskirq()'s
 * inline RISC-V asm is stubbed out -- see the build line in the
 * harness script. Everything else, including layout() itself, is the
 * real source: the point is to catch a widget sitting on top of a
 * readout before it reaches a screen, and a lifted copy of the layout
 * would not catch anything.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define main logic_main_unused
#include "../logic.c"
#undef main
int main(void){
  int fails=0,i,j;
  memset(widgets,0,sizeof widgets);
  for(i=0;i<W_COUNT;i++){widgets[i].type=Z_WIDGET_BUTTON;widgets[i].enabled=true;}
  wset.items=widgets; wset.count=W_COUNT; wset.win=&win; wset.pressed=-1; wset.focused=-1;
  layout();
  fprintf(stderr,"content %dx%d | wave x%d..%d y%d..%d | deck %d rows %d,%d | readout y%d h%d\n",
    CONTENT_W,CONTENT_H,WV_X,WV_X+WV_W,WV_TOP,WV_TOP+WV_H,DECK_TOP,deck_y1,deck_y2,read_y,read_h);
  for(i=0;i<W_COUNT;i++){
    z_widget_t*a=&widgets[i];
    if(a->w==0||a->h==0){fprintf(stderr,"FAIL widget %d has no size\n",i);fails++;continue;}
    if(a->x<0||a->y<0||a->x+a->w>CONTENT_W||a->y+a->h>CONTENT_H){
      fprintf(stderr,"FAIL widget %d outside: %d,%d %dx%d\n",i,a->x,a->y,a->w,a->h);fails++;}
    for(j=i+1;j<W_COUNT;j++){
      z_widget_t*b=&widgets[j];
      if(a->x<b->x+b->w&&b->x<a->x+a->w&&a->y<b->y+b->h&&b->y<a->y+a->h){
        fprintf(stderr,"FAIL overlap %d(%d,%d %dx%d)/%d(%d,%d %dx%d)\n",
          i,a->x,a->y,a->w,a->h,j,b->x,b->y,b->w,b->h);fails++;}
    }
    if(a->y+a->h>read_y&&a->y<read_y+read_h){
      fprintf(stderr,"FAIL widget %d overlaps readout\n",i);fails++;}
    if(a->x+a->w>WV_X-2&&a->x<WV_X+WV_W+2&&a->y+a->h>WV_TOP-4&&a->y<WV_TOP+WV_H+2){
      fprintf(stderr,"FAIL widget %d overlaps waveform\n",i);fails++;}
  }
  /* Containment: every widget must sit INSIDE the frame it belongs to,
     with a pixel of clearance so it never sits ON the frame line.

     This is the check that was missing, and the bug it would have
     caught: the PORT +/- buttons sat at y 15..27 while the channel
     frame started at y 26, so two buttons were drawn across the top
     edge of the box they were supposed to be labelling. Widget-vs-
     widget and widget-vs-window both passed it happily. */
  {
    struct { const char *name; rect_t *f; int lo, hi; } groups[] = {
      { "channel strip", &strip_f, W_MODE0, W_LVL7 },
      { "deck",          &deck_f,  W_RATE_DN, W_DECODE },
    };
    for (unsigned g=0; g<sizeof groups/sizeof*groups; g++){
      rect_t *f = groups[g].f;
      for(i=groups[g].lo;i<=groups[g].hi;i++){
        z_widget_t*a=&widgets[i];
        if(a->x <= f->x || a->y <= f->y ||
           a->x+a->w >= f->x+f->w || a->y+a->h >= f->y+f->h){
          fprintf(stderr,"FAIL widget %d (%d,%d %dx%d) not inside %s frame "
            "(%d,%d %dx%d)\n",i,a->x,a->y,a->w,a->h,groups[g].name,
            f->x,f->y,f->w,f->h);fails++;}
      }
    }
    /* the header widgets live ABOVE the strip frame, not in it */
    for(i=W_PORT_DN;i<=W_PORT_UP;i++){
      z_widget_t*a=&widgets[i];
      if(a->y+a->h > strip_f.y){
        fprintf(stderr,"FAIL header widget %d (y %d..%d) runs into the "
          "channel frame at y %d\n",i,a->y,a->y+a->h,strip_f.y);fails++;}
    }
    /* frames inside the content area, and not on top of each other */
    rect_t *fr[3] = { &strip_f, &wave_f, &deck_f };
    const char *fn[3] = { "strip", "wave", "deck" };
    for(i=0;i<3;i++){
      if(fr[i]->x<0||fr[i]->y<0||fr[i]->x+fr[i]->w>CONTENT_W||
         fr[i]->y+fr[i]->h>CONTENT_H){
        fprintf(stderr,"FAIL %s frame outside content\n",fn[i]);fails++;}
      for(j=i+1;j<3;j++)
        if(fr[i]->x < fr[j]->x+fr[j]->w && fr[j]->x < fr[i]->x+fr[i]->w &&
           fr[i]->y < fr[j]->y+fr[j]->h && fr[j]->y < fr[i]->y+fr[i]->h){
          fprintf(stderr,"FAIL %s and %s frames overlap\n",fn[i],fn[j]);fails++;}
    }
    /* the traces must fit in the wave frame -- draw_wave_screen()'s own
       arithmetic, restated so a change to WV_ROW_H cannot outgrow it */
    {
      int top = WV_TOP + 3;
      int bot = WV_TOP + 7*WV_ROW_H + WV_ROW_H - 5;
      if(top <= wave_f.y || bot >= wave_f.y+wave_f.h){
        fprintf(stderr,"FAIL traces %d..%d escape wave frame %d..%d\n",
          top,bot,wave_f.y,wave_f.y+wave_f.h);fails++;}
    }
  }
  if(read_h<3*LINE+6){fprintf(stderr,"FAIL readout too short: %d\n",read_h);fails++;}
  /* The bug this file exists to have caught: laying out in window
     coordinates when the drawable area is smaller. z_win_content_rect()
     insets 2px per content edge and the titlebar takes the top, so a
     window is 4 wider and Z_WM_TITLEBAR_H+4 taller than its content. */
  if(WIN_W-4!=CONTENT_W||WIN_H-(Z_WM_TITLEBAR_H+4)!=CONTENT_H){
    fprintf(stderr,"FAIL window/content mismatch: win %dx%d content %dx%d\n",
      WIN_W,WIN_H,CONTENT_W,CONTENT_H);fails++;}
  if(WIN_W>Z_SCREEN_W||WIN_H>Z_SCREEN_H){
    fprintf(stderr,"FAIL window bigger than the screen: %dx%d\n",WIN_W,WIN_H);fails++;}
  if(WV_ROW_H!=CH_ROW_H){fprintf(stderr,"FAIL strip and traces misaligned\n");fails++;}
  fprintf(stderr,"\n%s\n",fails?"FAIL":"PASS");
  return fails!=0;
}

/* -- host stubs for everything logic.c draws with or talks to.
 *
 * Deliberately do-nothing: this harness checks ARITHMETIC, not
 * pixels. A stub that recorded draw calls would let the check assert
 * about the frame around each well too, which is a reasonable next
 * step and not what the overlap bug this exists for needs. */
const z_font_t z_font_5x8 = { 5, 8, 32, 126, 0 };
void z_win_hw_box(const z_win_t*w,int a,int b,int c,int d,int e){(void)w;(void)a;(void)b;(void)c;(void)d;(void)e;}
void z_win_hw_line(const z_win_t*w,int a,int b,int c,int d,int e){(void)w;(void)a;(void)b;(void)c;(void)d;(void)e;}
void z_win_draw_text(const z_win_t*w,int x,int y,const char*s,int c,const z_font_t*f){(void)w;(void)x;(void)y;(void)s;(void)c;(void)f;}
void z_win_fill_rect(const z_win_t*w,int x,int y,int a,int b,int c){(void)w;(void)x;(void)y;(void)a;(void)b;(void)c;}
void z_win_clear(const z_win_t*w){(void)w;}
int z_win_content_w(const z_win_t*w){(void)w;return CONTENT_W;}
int z_win_content_h(const z_win_t*w){(void)w;return CONTENT_H;}
void z_widget_set_init(z_widget_set_t*s,z_widget_t*i,int n,const z_win_t*w){s->items=i;s->count=n;s->win=w;s->pressed=-1;s->focused=-1;}
void z_widget_invalidate(z_widget_set_t*s){(void)s;}
void z_widget_draw(z_widget_set_t*s,int i){(void)s;(void)i;}
void z_widget_draw_all(z_widget_set_t*s,bool f){(void)s;(void)f;}
int z_widget_mouse(z_widget_set_t*s,int x,int y,uint8_t b){(void)s;(void)x;(void)y;(void)b;return -1;}
int z_widget_group_selection(const z_widget_set_t*s,uint8_t g){(void)s;(void)g;return -1;}
void z_widget_select(z_widget_set_t*s,int i){(void)s;(void)i;}
int z_widget_focus_next(z_widget_set_t*s,bool b){(void)s;(void)b;return -1;}
void z_widget_focus_set(z_widget_set_t*s,int i){(void)s;(void)i;}
int z_widget_key_activate(z_widget_set_t*s){(void)s;return -1;}
bool z_gpio_present(void){return true;}
uint32_t z_gpio_port_count(void){return 1;}
uint8_t z_gpio_dir_get(uint32_t p){(void)p;return 0;}
void z_gpio_dir_set(uint32_t p,uint8_t m){(void)p;(void)m;}
uint8_t z_gpio_out_get(uint32_t p){(void)p;return 0;}
void z_gpio_out_put(uint32_t p,uint8_t v){(void)p;(void)v;}
uint8_t z_gpio_in_get(uint32_t p){(void)p;return 0;}
void z_gpio_mode(uint32_t p,uint32_t n,z_gpio_mode_t m){(void)p;(void)n;(void)m;}
z_gpio_mode_t z_gpio_mode_get(uint32_t p,uint32_t n){(void)p;(void)n;return Z_GPIO_IN;}
bool z_gpio_read(uint32_t p,uint32_t n){(void)p;(void)n;return false;}
void z_gpio_write(uint32_t p,uint32_t n,bool v){(void)p;(void)n;(void)v;}
void z_gpio_toggle(uint32_t p,uint32_t n){(void)p;(void)n;}
void z_gpio_od_write(uint32_t p,uint32_t n,bool v){(void)p;(void)n;(void)v;}
void z_led_set(bool b){(void)b;}
void z_led_bar_set(uint8_t b){(void)b;}
bool z_launch_arg_take(char *b,int n){(void)b;(void)n;return false;}
z_rv z_msg_read(z_msg_t*m){(void)m;return Z_FAIL;}
void z_proc_wait(uint32_t t){(void)t;}
bool z_win_apply_clip(z_win_t*w,z_obj_t*o){(void)w;(void)o;return true;}
void z_win_apply_redraw(z_win_t*w,uint32_t p){(void)w;(void)p;}
bool z_win_parse_rect(z_win_t*w,z_obj_t*o){(void)w;(void)o;return true;}
void z_win_redraw_done(const z_win_t*w){(void)w;}
int z_win_redraw_id(uint32_t p){(void)p;return -1;}
z_rv z_win_create_flags(z_win_t*w,const char*t,uint32_t a,uint32_t b,int32_t c,int32_t d,uint32_t f){
  (void)w;(void)t;(void)a;(void)b;(void)c;(void)d;(void)f;return Z_OK;}
bool z_win_mouse_content_xy(const z_win_t*w,uint32_t p,int*x,int*y){
  (void)w;(void)p;*x=0;*y=0;return false;}
/* abs_box()/abs_line() need this; the harness has no real window, so
   a content rect at the origin is all the arithmetic requires. */
void z_win_content_rect(const z_win_t *w, z_clip_t *o) {
  (void)w; o->x0 = 0; o->y0 = 0; o->x1 = CONTENT_W - 1; o->y1 = CONTENT_H - 1;
}
