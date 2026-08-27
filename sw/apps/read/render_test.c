
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
static void scroll_down(int n){
    if(n<=0)return;
    md_state_t st; state_at(top_line,&st);
    md_line_t ml;
    long line=top_line, sub=top_sub;
    long used=read_block(line,&st,&ml);
    if(!used)return;
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
