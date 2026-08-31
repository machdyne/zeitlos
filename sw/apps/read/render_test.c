
/*
 * Host harness for the reader's LAYOUT and STREAMING logic.
 *
 * Reimplements only the file access (stdio instead of the Zeitlos fs
 * syscalls); the block joining, wrapping and indenting are the same
 * rules read.c uses, kept in step by being derived from the same
 * md.c. Draws ASCII rather than pixels.
 *
 * What it is for: proving that scrolling is REVERSIBLE. Scrolling
 * down N display lines and back up N must land exactly where it
 * started, at every position in a real document. That is the property
 * a block-based position model exists to provide, and the one that
 * would be miserable to test on hardware.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "md.h"

#define MAX_SUB 64
#define FW 5
static int view_w = 300;

static char *doc; static long doclen;
static long seeks;
static long *lineoff; static long nlines;

static void load(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f,0,SEEK_END); doclen = ftell(f); fseek(f,0,SEEK_SET);
    doc = malloc(doclen+2); fread(doc,1,doclen,f); doc[doclen]=0; fclose(f);
    nlines = 1; for (long i=0;i<doclen;i++) if (doc[i]=='\n') nlines++;
    lineoff = malloc(sizeof(long)*(nlines+2));
    long n=0; lineoff[n++]=0;
    for (long i=0;i<doclen;i++) if (doc[i]=='\n') lineoff[n++]=i+1;
    nlines = n;
}
static void getline_at(long ln, char *out, int cap) {
    out[0]=0; if (ln<0||ln>=nlines) return;
    long o=lineoff[ln]; int n=0;
    while (o<doclen && doc[o]!='\n' && n<cap-1) { if (doc[o]!='\r') out[n++]=doc[o]; o++; }
    out[n]=0;
}
static void line_indent(const md_line_t *ml, int *first, int *rest) {
    int cw=FW;
    switch (ml->kind) {
        case MD_LIST: { int base=(ml->level+1)*2*cw; int mlen=(int)strlen(ml->marker)+1;
            *first=base; *rest=base+mlen*cw; break; }
        case MD_QUOTE: *first=*rest=3*cw; break;
        case MD_CODE: case MD_TABLE: *first=*rest=cw; break;
        default: *first=*rest=0; break;
    }
}
static int wrap_line(const md_line_t *ml,int width,uint16_t*st,uint16_t*en){
    if (ml->kind==MD_CODE||ml->kind==MD_TABLE){st[0]=0;en[0]=ml->len;return 1;}
    if (!ml->len){st[0]=0;en[0]=0;return 1;}
    int fi,ri; line_indent(ml,&fi,&ri);
    int n=0,i=0;
    while (i<ml->len && n<MAX_SUB){
        int avail=width-(n?ri:fi); int cols=avail/FW; if(cols<1)cols=1;
        int end=i+cols;
        if (end>=ml->len){st[n]=i;en[n]=ml->len;n++;break;}
        int brk=-1; for(int k=end;k>i;k--) if(ml->text[k]==' '){brk=k;break;}
        if(brk<0)brk=end;
        st[n]=i;en[n]=brk;n++;
        i=brk; while(i<ml->len&&ml->text[i]==' ')i++;
    }
    if(!n){st[0]=0;en[0]=0;n=1;}
    return n;
}
// mirrors read.c's read_block()
static long read_block(long ln, md_state_t *st, md_line_t *out) {
    static char line[MD_LINE_MAX], joined[MD_LINE_MAX], peek[MD_LINE_MAX];
    if (ln>=nlines) return 0;
    getline_at(ln,line,MD_LINE_MAX);
    getline_at(ln+1,peek,MD_LINE_MAX);
    const char *pk = (ln+1<nlines)?peek:NULL;
    md_state_t probe=*st; md_line_t first;
    int ate = md_parse(&probe,line,pk,&first);
    if (ate) { *st=probe; *out=first; return 2; }
    if (!md_continues(st,first.kind,pk)) { *st=probe; *out=first; return 1; }
    int n=0; for(int i=0;line[i]&&n<MD_LINE_MAX-1;i++) joined[n++]=line[i];
    long used=1; md_kind_t kind=first.kind;
    for (;;) {
        getline_at(ln+used,peek,MD_LINE_MAX);
        const char *nx=(ln+used<nlines)?peek:NULL;
        if (!md_continues(st,kind,nx)) break;
        getline_at(ln+used,line,MD_LINE_MAX); used++;
        int i=0; while(line[i]==' '||line[i]=='\t')i++;
        if(n<MD_LINE_MAX-1)joined[n++]=' ';
        for(;line[i]&&n<MD_LINE_MAX-1;i++)joined[n++]=line[i];
    }
    joined[n]=0;
    md_parse(st,joined,NULL,out);
    return used;
}
static void state_at(long line, md_state_t *st) {
    seeks++;
    md_state_init(st); md_line_t ml; long ln=0;
    while (ln<line) { long u=read_block(ln,st,&ml); if(!u)break; ln+=u; }
}
static int subs_of(const md_line_t *ml){
    uint16_t a[MAX_SUB],b[MAX_SUB];
    if(ml->kind==MD_SKIP)return 0;
    if(ml->kind==MD_BLANK||ml->kind==MD_RULE)return 1;
    return wrap_line(ml,view_w,a,b);
}
static int block_subs(long ln, long *used) {
    md_state_t st; state_at(ln,&st); md_line_t ml;
    uint16_t a[MAX_SUB],b[MAX_SUB];
    long u=read_block(ln,&st,&ml); if(used)*used=u; if(!u)return 1;
    if(ml.kind==MD_BLANK||ml.kind==MD_RULE||ml.kind==MD_SKIP)return 1;
    return wrap_line(&ml,view_w,a,b);
}
static long prev_block(long line) {
    if(!line)return 0;
    md_state_t st; md_state_init(&st); md_line_t ml;
    long ln=0, prev=0;
    while(ln<line){ long u=read_block(ln,&st,&ml); if(!u)break; prev=ln; ln+=u; }
    return prev;
}
static long top_line, top_sub;
// single forward pass, mirroring read.c
static int scroll_down(int n){
    if(n<=0)return 0;
    md_state_t st; state_at(top_line,&st);
    md_line_t ml;
    long line=top_line, sub=top_sub;
    long used=read_block(line,&st,&ml);
    if(!used)return 0;
    int subs=subs_of(&ml);
    int rem=n;
    while(rem>0){
        if(subs>0 && sub+1<subs){sub++;rem--;continue;}
        md_state_t nst=st; md_line_t tmp;
        long nline=line+used;
        long nused=read_block(nline,&nst,&tmp);
        if(!nused)break;
        st=nst; ml=tmp; line=nline; used=nused; sub=0; subs=subs_of(&ml);
        if(subs>0)rem--;
    }
    top_line=line; top_sub=sub;
    return n-rem;

}
#define BACK_MAX 96
static int collect_before(long target,long*lines,int*subs,int max){
    if(!target)return 0;
    seeks++;
    md_state_t st; md_state_init(&st); md_line_t ml;
    long ln=0; int n=0;
    while(ln<target){
        long at=ln; long used=read_block(ln,&st,&ml); if(!used)break;
        int c=subs_of(&ml);
        if(c>0){
            if(n<max){lines[n]=at;subs[n]=c;n++;}
            else{ for(int i=1;i<max;i++){lines[i-1]=lines[i];subs[i-1]=subs[i];}
                  lines[max-1]=at; subs[max-1]=c; }
        }
        ln+=used;
    }
    return n;
}
static void scroll_up(int n){
    if(n<=0)return;
    static long lines[BACK_MAX]; static int subs[BACK_MAX];
    while(n>0){
        if(top_sub){ long take=n<top_sub?n:top_sub; top_sub-=take; n-=take; continue; }
        if(!top_line)return;
        int cnt=collect_before(top_line,lines,subs,BACK_MAX);
        if(!cnt)return;
        int i=cnt-1;
        while(n>0&&i>=0){
            top_line=lines[i]; top_sub=subs[i]-1; n--;
            if(n<=0)break;
            long inside=top_sub; long take=n<inside?n:inside;
            top_sub-=take; n-=take; i--;
        }
        if(n>0&&top_line==lines[0]&&cnt<BACK_MAX)return;
    }
}

int main(int argc,char**argv){
    if(argc<2){printf("usage: render_test FILE [width] [mode]\n");return 1;}
    load(argv[1]);
    if(argc>2)view_w=atoi(argv[2]);
    const char *mode = argc>3?argv[3]:"render";

    if(!strcmp(mode,"scroll")){
        // Reversibility: from every position, down N then up N must
        // return exactly. This is the invariant the block model buys.
        int fails=0, tested=0;
        for(int start=0; start<400 && start<nlines; start++){
            for(int dist=1; dist<=8; dist++){
                top_line=0; top_sub=0;
                scroll_down(start);
                long l0=top_line,s0=top_sub;
                scroll_down(dist);
                // At the end of the document scrolling down CLAMPS
                // while scrolling up does not, so a round trip there
                // legitimately lands earlier than it started. Skip
                // those: they are the reader refusing to scroll past
                // the end, not a position bug.
                {
                    long al=top_line, as=top_sub;
                    scroll_down(1);
                    int at_end = (top_line==al && top_sub==as);
                    top_line=al; top_sub=as;
                    if (at_end) { top_line=l0; top_sub=s0; continue; }
                }
                scroll_up(dist);
                tested++;
                if(top_line!=l0||top_sub!=s0){
                    if(fails<5) printf("  FAIL from (%ld,%ld) +%d-%d -> (%ld,%ld)\n",
                        l0,s0,dist,dist,top_line,top_sub);
                    fails++;
                }
            }
        }
        printf("%s: %d round trips, %d failed", argv[1], tested, fails);
        // cost of one PAGE up and down, in seeks
        seeks=0; top_line=0; top_sub=0; scroll_down(200);
        long s_dn=seeks; seeks=0; scroll_down(30); long page_dn=seeks;
        seeks=0; scroll_up(30); long page_up=seeks;
        printf("  |  seeks: page down %ld, page up %ld\n", page_dn, page_up);
        (void)s_dn;
        return fails!=0;
    }


    if(!strcmp(mode,"seekcache")){
        // Validates the premise read.c's position cache rests on:
        // replaying the parser forward from an ARBITRARY earlier
        // position must land in exactly the same state as replaying
        // from a fixed index checkpoint.
        //
        // If that ever failed, the cache would silently render a
        // document differently depending on how the reader arrived --
        // a fenced code block resumed as prose, every '#' inside it
        // becoming a heading. Worth proving rather than assuming,
        // since it is the one assumption the optimisation makes.
        int checked=0, bad=0;
        for(long target=1; target<nlines && target<800; target++){
            md_state_t ref; state_at(target,&ref);      // from line 0
            for(long mid=0; mid<target; mid += 7){
                md_state_t via; state_at(mid,&via);      // to the mid-point
                md_line_t ml; long ln=mid;
                while(ln<target){ long u=read_block(ln,&via,&ml); if(!u)break; ln+=u; }
                checked++;
                if(memcmp(&ref,&via,sizeof(md_state_t))){
                    if(bad<5) printf("  MISMATCH target=%ld via mid=%ld\n",target,mid);
                    bad++;
                }
            }
        }
        printf("%s: seekcache: %d replays from mid-points, %d mismatched\n",
               argv[1], checked, bad);
        return bad!=0;
    }


    if(!strcmp(mode,"seam")){
        // Models read.c's accelerated scroll INCLUDING the layout
        // cache, and requires the result to equal a full redraw pixel
        // row by pixel row:
        //   1. lay the old screen out fully; note where the layout
        //      stopped (the cache's end state)
        //   2. shift = step_y[k] - MARGIN; blit rows up by it
        //   3. resume the parser from the end state at end_y - shift,
        //      drawing only into the strip (band-filling straddlers)
        //   4. compare against a fresh full layout from the new top
        #define VH 225
        #define MG 3
        #define EV 96
        #define SMAX 96
        static char ev_a[VH][EV], ev_b[VH][EV], ev_c[VH][EV];
        static int  st_y[SMAX]; static long st_ln[SMAX]; static int st_sub[SMAX];
        static int  sa_y[SMAX]; static long sa_ln[SMAX]; static int sa_sub[SMAX];
        int nstep=0, nstep_a=0;
        // end state of the last lay()
        md_state_t end_st; long end_line; int end_y; int cache_ns; int end_cut;

        void emit(char ev[VH][EV], int yy, const char *t){ if(yy>=0&&yy<VH) strncat(ev[yy],t,EV-1-strlen(ev[yy])); }

        // lay out from (st,line) starting at y0 with sub_skip, emitting
        // draw events only for pixels at y >= thr (band-filling a row
        // that straddles thr first), exactly as read.c's layout_run.
        void lay(char ev[VH][EV], md_state_t st, long line, int y0, long sub_skip, int thr, int reset_steps){
            if(reset_steps) nstep=0;
            static md_line_t ml; uint16_t s_[MAX_SUB],e_[MAX_SUB];
            int y=y0; long cur=line;
            md_state_t snap_st=st; long snap_line=cur; int snap_y=y, snap_ns=nstep; end_cut=0;
            while(y<VH){
                snap_st=st; snap_line=cur; snap_y=y; snap_ns=nstep;
                long used=read_block(cur,&st,&ml); if(!used)break;
                long blk=cur; cur+=used; int y_blk=y;
                if(ml.kind==MD_SKIP)continue;
                if(ml.kind==MD_BLANK){
                    if(!sub_skip){ if(nstep<SMAX){st_y[nstep]=y_blk;st_ln[nstep]=blk;st_sub[nstep]=0;nstep++;} y+=5; }
                    else sub_skip=0; continue; }
                if(ml.kind==MD_RULE){
                    if(!sub_skip){ if(nstep<SMAX){st_y[nstep]=y_blk;st_ln[nstep]=blk;st_sub[nstep]=0;nstep++;}
                        y+=3; if(y+1>=thr) emit(ev,y+1,"R;"); y+=3; } else sub_skip=0;
                    continue; }
                int fh=(ml.kind==MD_HEADING&&ml.level<=2)?12:8, lh=fh+1, fi,ri; line_indent(&ml,&fi,&ri);
                int n2=wrap_line(&ml,view_w,s_,e_);
                if(!sub_skip){ if(ml.kind==MD_HEADING) y+=ml.level<=2?5:3; }
                int s2;
                for(s2=0;s2<n2;s2++){
                    if(sub_skip){sub_skip--;continue;}
                    if(y+fh>VH){ end_cut=1; break; }
                    if(nstep<SMAX){st_y[nstep]=s2?y:y_blk;st_ln[nstep]=blk;st_sub[nstep]=s2;nstep++;}
                    int indent=s2?ri:fi, extra=(ml.kind==MD_LIST&&s2==0)?((int)strlen(ml.marker)+1)*FW:0;
                    if(y+fh>thr){
                        if(thr>0&&y<thr) for(int yy=y;yy<y+fh;yy++) if(yy>=0&&yy<VH) ev[yy][0]=0;  // band fill
                        for(int yy=y;yy<y+fh;yy++){ char t[48];
                            if(ml.kind==MD_LIST&&s2==0){ sprintf(t,"M%ld;",blk); emit(ev,yy,t); }
                            if(ml.kind==MD_QUOTE) emit(ev,yy,"Q;");
                            sprintf(t,"T%ld.%d@%dh%d;",blk,s2,MG+indent+extra,fh); emit(ev,yy,t); }
                    }
                    y+=lh;
                }
                if(end_cut){ end_st=snap_st; end_line=snap_line; end_y=snap_y; cache_ns=snap_ns; }
                if(ml.kind==MD_HEADING&&ml.level==1&&y+2<VH){ if(y>=thr) emit(ev,y,"U;"); y+=3; }
                if(end_cut) return;
            }
            end_st=st; end_line=cur; end_y=y; cache_ns=nstep;
        }

        int tested=0, cached=0, blitted=0, fails=0, rej=0, chained=0;
        int dists[]={1,2,3,5,8,24};
        for(long start=0; start<600 && start<nlines; start++){
            for(int di=0; di<6; di++){
                int n=dists[di];
                top_line=0; top_sub=0; scroll_down(start);
                md_state_t st0; state_at(top_line,&st0);
                for(int i=0;i<VH;i++) ev_a[i][0]=0;
                lay(ev_a, st0, top_line, MG, top_sub, 0, 1);
                nstep_a=nstep;
                for(int i=0;i<nstep_a;i++){sa_y[i]=st_y[i];sa_ln[i]=st_ln[i];sa_sub[i]=st_sub[i];}
                md_state_t c_st=end_st; long c_line=end_line; int c_y=end_y; int c_ns=cache_ns;
                long l0=top_line, s0=top_sub;
                int k=scroll_down(n);
                tested++;
                if(k<=0||k>=nstep_a){ rej++; continue; }
                if(sa_ln[k]!=top_line||sa_sub[k]!=(int)top_sub){ rej++; continue; }
                int shift=sa_y[k]-MG;
                if(shift<=0||shift>=VH-MG){ rej++; continue; }
                int thr=VH-shift;

                // The glass after the blit, modelled HONESTLY: rows
                // above the strip are the old rows moved up, and the
                // strip [thr,VH) still holds whatever was there before
                // -- the old bottom rows -- until something clears it.
                // Modelling it as already empty is how a missing clear
                // in the cache path went unnoticed once.
                for(int y=0;y<VH;y++) strcpy(ev_c[y], ev_a[y]);
                for(int y=MG;y<thr;y++) strcpy(ev_c[y], ev_a[y+shift]);

                // read.c clears the strip on both paths; model that
                // as a distinct step so its absence would show.
                for(int y=thr;y<VH;y++) ev_c[y][0]=0;

                if(k<c_ns){
                    // cache path: shift the surviving steps as read.c
                    // does, then resume, APPENDING -- so the step table
                    // and end state after this scroll are what the
                    // next scroll will start from.
                    cached++;
                    { int w=0; for(int i=k;i<c_ns;i++){ st_y[w]=sa_y[i]-shift; st_ln[w]=sa_ln[i]; st_sub[w]=sa_sub[i]; w++; } nstep=w; }
                    lay(ev_c, c_st, c_line, c_y-shift, 0, thr, 0);

                    // CHAIN: one more line-down from the state the
                    // resume left behind. This is what reading actually
                    // does, and a resume that records its end state
                    // wrongly only shows on the SECOND scroll.
                    {
                        md_state_t c2_st=end_st; long c2_line=end_line; int c2_y=end_y; int c2_ns=cache_ns;
                        int k2=1;
                        if(k2<nstep && k2<c2_ns){
                            int shift2=st_y[k2]-MG;
                            if(shift2>0 && shift2<VH-MG){
                                int thr2=VH-shift2;
                                static char ev_d[VH][EV];
                                for(int y=0;y<VH;y++) strcpy(ev_d[y], ev_c[y]);
                                for(int y=MG;y<thr2;y++) strcpy(ev_d[y], ev_c[y+shift2]);
                                for(int y=thr2;y<VH;y++) ev_d[y][0]=0;
                                long tl=st_ln[k2]; int ts=st_sub[k2];
                                lay(ev_d, c2_st, c2_line, c2_y-shift2, 0, thr2, 1);
                                static char ev_e[VH][EV];
                                md_state_t st3; state_at(tl,&st3);
                                for(int i=0;i<VH;i++) ev_e[i][0]=0;
                                lay(ev_e, st3, tl, MG, ts, 0, 1);
                                for(int y=MG;y<VH;y++) if(strcmp(ev_e[y],ev_d[y])){
                                    if(fails<5) printf("  CHAIN FAIL start=(%ld,%ld) n=%d then 1: y=%d\n    screen: '%s'\n    fresh:  '%s'\n",
                                                       l0,s0,n,y,ev_d[y],ev_e[y]);
                                    fails++; break; }
                                chained++;
                            }
                        }
                    }
                } else {
                    // full-layout strip path
                    blitted++;
                    md_state_t st1; state_at(top_line,&st1);
                    lay(ev_c, st1, top_line, MG, top_sub, thr, 1);
                }

                // ground truth
                md_state_t st2; state_at(top_line,&st2);
                for(int i=0;i<VH;i++) ev_b[i][0]=0;
                lay(ev_b, st2, top_line, MG, top_sub, 0, 1);

                for(int y=MG;y<VH;y++){
                    if(strcmp(ev_b[y],ev_c[y])){
                        if(fails<5) printf("  SEAM FAIL start=(%ld,%ld) n=%d k=%d shift=%d y=%d %s\n    screen: '%s'\n    fresh:  '%s'\n",
                                           l0,s0,n,k,shift,y,(k<c_ns)?"[cache]":"[full]",ev_c[y],ev_b[y]);
                        fails++; break;
                    }
                }
            }
        }
        printf("%s: seam: %d scrolls, %d via cache (%d chained), %d via full layout, %d repaint, %d FAILED\n",
               argv[1], tested, cached, chained, blitted, rej, fails);
        return fails!=0;
    }

    int maxout = argc>4?atoi(argv[4]):50;
    md_state_t st; md_state_init(&st); md_line_t ml;
    uint16_t ss[MAX_SUB],ee[MAX_SUB];
    long ln=0; int out=0;
    printf("+%.*s+\n",view_w/FW,"--------------------------------------------------------------------------------");
    while(ln<nlines && out<maxout){
        long u=read_block(ln,&st,&ml); if(!u)break;
        if(ml.kind==MD_SKIP){ln+=u;continue;}
        if(ml.kind==MD_BLANK){printf("|\n");out++;ln+=u;continue;}
        if(ml.kind==MD_RULE){printf("|%.*s\n",view_w/FW,"________________________________________________________________________________");out++;ln+=u;continue;}
        int fi,ri; line_indent(&ml,&fi,&ri);
        int cnt=wrap_line(&ml,view_w,ss,ee);
        for(int s=0;s<cnt&&out<maxout;s++){
            printf("|%*s",(s?ri:fi)/FW,"");
            if(ml.kind==MD_LIST&&s==0)printf("%s ",ml.marker);
            printf("%.*s\n",ee[s]-ss[s],ml.text+ss[s]); out++;
        }
        ln+=u;
    }
    printf("+---\n");
    return 0;
}
